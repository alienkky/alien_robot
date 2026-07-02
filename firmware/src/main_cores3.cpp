// ─────────────────────────────────────────────────────────────────────
// Alien Robot — M3 firmware for M5Stack CoreS3 (Route A).
// Compiled ONLY in the `cores3` PlatformIO env.
//
// Why CoreS3 + M5Unified: every peripheral (screen, ES7210 mic, AW88298
// speaker, GC0308 camera) sits behind the AXP2101 PMIC + AW9523 IO expander.
// M5.begin() powers those rails and configures the codecs/display in one call —
// this is what removes the manual power-sequencing that broke the Waveshare/
// Spotpear clone (dark screen + dead I2C bus).
//
// Flow (push-to-talk): hold the touch screen (or send 't' over serial) → record
// mic PCM (M5.Mic, I2S) + capture one GC0308 frame → SW-encode JPEG → POST
// multipart /api/see {audio, image} to the 4090 gateway → gateway runs STT +
// Brain180 tutor (vision) + TTS → returns {transcript, answer, audio_url} → GET
// the WAV and play it on the AW88298 speaker (M5.Speaker).
//
// ⚠️ BUILD vs DEVICE: build-verified with `pio run -e cores3` only. The items
// below are HARDWARE-UNVERIFIED and must be confirmed on the physical board via
// serial log after flashing:
//   1) Camera <-> internal-I2C coexistence (CAM_SCCB_I2C_PORT knob in
//      config_cores3.h). GC0308 has no XCLK pin here (pin_xclk = -1).
//   2) M5.Mic / M5.Speaker begin/end switching (they share the I2S peripheral).
//   3) Touch as push-to-talk while the camera driver holds the SCCB bus.
//   4) The server WAV format (header/sample-rate) parsed in playWav().
// GC0308 outputs RGB565 (no hardware JPEG encoder), so we capture RGB565 and
// software-encode to JPEG with frame2jpg() to keep the /api/see contract.
// ─────────────────────────────────────────────────────────────────────

#include <M5Unified.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>  // TLS for https tunnel URLs (cloudflared/ngrok)

#include "esp_camera.h"
#include "img_converters.h"  // frame2jpg() — software RGB565 -> JPEG encoder
#include "driver/i2c.h"      // i2c_driver_delete() — hand the SCCB bus back to M5
#include "soc/soc.h"          // brownout detector register
#include "soc/rtc_cntl_reg.h"
#include "esp_task_wdt.h"     // disable task watchdog (avoid WDT reboots)
#include "esp_system.h"       // esp_reset_reason() — log why it last rebooted

#include "config_cores3.h"

// Optional camera tuning knobs default here, so an older config_cores3.h that
// predates them still builds (only WIFI_*, AI_SERVER_BASE_URL, DEVICE_TOKEN are
// truly required). Override any of these in config_cores3.h to change them.
#ifndef CAM_SCCB_I2C_PORT
#define CAM_SCCB_I2C_PORT 1
#endif
#ifndef CAM_SWAP_BYTES
#define CAM_SWAP_BYTES 1
#endif
#ifndef CAM_HMIRROR
#define CAM_HMIRROR 1
#endif
#ifndef CAM_VFLIP
#define CAM_VFLIP 0
#endif

namespace {
constexpr int kSampleRate = 16000;
constexpr int kMaxRecordSeconds = 4;
constexpr size_t kMaxSamples = kSampleRate * kMaxRecordSeconds;  // 16-bit mono
constexpr size_t kRecordChunk = 512;   // samples per M5.Mic.record() call
constexpr uint8_t kSpeakerVolume = 80; // 0..255 (kept low for desk use)
constexpr uint8_t kJpegQuality = 80;   // frame2jpg quality 0..100

int16_t *pcm = nullptr;   // PSRAM record buffer (kMaxSamples int16 samples)
bool cameraOk = false;

// ── Camera: M5Stack CoreS3 GC0308 (DVP) ───────────────────────────────
// Pin values verified against the M5Stack StackChan/CoreS3 GC0308 example
// (docs.m5stack.com). SCCB shares the internal I2C bus (SDA=12, SCL=11);
// pin_xclk = -1 because the board clocks the sensor, not an ESP LEDC pin.
camera_config_t makeCameraConfig() {
  camera_config_t c = {};
  c.pin_pwdn = -1;
  c.pin_reset = -1;
  c.pin_xclk = -1;
  c.pin_sccb_sda = 12;
  c.pin_sccb_scl = 11;
  c.pin_d0 = 39;
  c.pin_d1 = 40;
  c.pin_d2 = 41;
  c.pin_d3 = 42;
  c.pin_d4 = 15;
  c.pin_d5 = 16;
  c.pin_d6 = 48;
  c.pin_d7 = 47;
  c.pin_vsync = 46;
  c.pin_href = 38;
  c.pin_pclk = 45;
  c.xclk_freq_hz = 20000000;
  c.ledc_timer = LEDC_TIMER_0;
  c.ledc_channel = LEDC_CHANNEL_0;
  c.pixel_format = PIXFORMAT_RGB565;  // GC0308 has no HW JPEG; encode in SW
  c.frame_size = FRAMESIZE_QVGA;      // 320x240 — small vision payload
  c.fb_count = 2;
  c.fb_location = CAMERA_FB_IN_PSRAM;
  // GRAB_LATEST (not WHEN_EMPTY): always return the newest frame and recycle old
  // buffers. WHEN_EMPTY hands back a buffer filled while idle, so every turn
  // showed the same stale (first) image. Combined with the drain in captureFresh.
  c.grab_mode = CAMERA_GRAB_LATEST;
  // Reuse the already-initialised internal I2C port instead of releasing it,
  // so touch + the audio codecs keep their bus. See CAM_SCCB_I2C_PORT note.
  c.sccb_i2c_port = CAM_SCCB_I2C_PORT;
  return c;
}

// ── Robot face (StackChan-style, drawn with M5GFX) ────────────────────
// A simple expressive face (cyan eyes + mouth) that reacts to the turn state,
// plus a Korean-capable status/speech line at the bottom. Drawn into a PSRAM
// sprite for flicker-free updates. This is deliberately a self-contained custom
// face (not the m5stack-avatar lib) so it has no background task to fight with
// the blocking record/HTTP loop and is fully build-verifiable.
enum Emotion { EMO_NEUTRAL, EMO_LISTEN, EMO_THINK, EMO_HAPPY, EMO_SAD };

M5Canvas *face = nullptr;
Emotion curEmo = EMO_NEUTRAL;
char faceText[256] = "";   // holds the (possibly long) Korean answer
bool bubbleOn = false;     // speech bubble only shows while talking
int g_seeCode = 0;         // last /api/see HTTP status (for friendly messages)

static int utf8Len(uint8_t ch) {
  if (ch < 0x80) return 1;
  if ((ch >> 5) == 0x6) return 2;
  if ((ch >> 4) == 0xE) return 3;
  if ((ch >> 3) == 0x1E) return 4;
  return 1;
}

int countGlyphs(const char *text) {
  int g = 0;
  const uint8_t *p = reinterpret_cast<const uint8_t *>(text);
  while (*p) { p += utf8Len(*p); g++; }
  return g;
}

// Word-wrap UTF-8 (Korean-safe) text and draw it in the bubble. Only the first
// `reveal` glyphs are shown (reveal < 0 = all) so the caption can stream out as
// the robot speaks; when it overflows `maxLines` the view auto-scrolls to the
// tail (the newest lines), StackChan-style. Caller sets font + colour first.
void drawBubbleLines(M5Canvas &c, const char *text, int reveal,
                     int x, int y, int w, int lineH, int maxLines) {
  static String lines[24];
  int count = 0;
  String cur = "";
  const uint8_t *p = reinterpret_cast<const uint8_t *>(text);
  int glyphs = 0;
  while (*p) {
    if (reveal >= 0 && glyphs >= reveal) break;
    if (*p == '\n') {
      if (count < 24) lines[count++] = cur;
      cur = ""; p++; glyphs++;
      continue;
    }
    int n = utf8Len(*p);
    String cand = cur;
    for (int i = 0; i < n && p[i]; i++) cand += static_cast<char>(p[i]);
    if (c.textWidth(cand) > w && cur.length() > 0) {
      if (count < 24) lines[count++] = cur;
      cur = "";
      continue;  // re-place this glyph on the next line
    }
    cur = cand; p += n; glyphs++;
  }
  if (cur.length() > 0 && count < 24) lines[count++] = cur;
  const int start = count > maxLines ? count - maxLines : 0;  // scroll to newest
  for (int i = start; i < count; i++) {
    c.setCursor(x, y + (i - start) * lineH);
    c.print(lines[i]);
  }
}

// Draw the face. talkMouth: -1 = emotion mouth, 0 = closed, 1 = open (lip-sync).
// revealGlyphs: -1 = show all bubble text; >=0 = stream only the first N glyphs.
void drawFace(Emotion e, bool eyesOpen, int talkMouth = -1, int revealGlyphs = -1) {
  if (!face) return;
  M5Canvas &c = *face;
  const int w = c.width(), h = c.height();
  c.fillSprite(TFT_BLACK);

  const uint16_t col = TFT_CYAN;
  const int eyeY = 44;
  const int lx = w / 2 - 50, rx = w / 2 + 50;
  const int er = 24;

  // Eyes — blink collapses to bars; THINK looks up-right.
  if (eyesOpen) {
    c.fillCircle(lx, eyeY, er, col);
    c.fillCircle(rx, eyeY, er, col);
    const int pdy = (e == EMO_THINK) ? -10 : 0;
    const int pdx = (e == EMO_THINK) ? 7 : 0;
    c.fillCircle(lx + pdx, eyeY + pdy, 10, TFT_BLACK);
    c.fillCircle(rx + pdx, eyeY + pdy, 10, TFT_BLACK);
  } else {
    c.fillRoundRect(lx - er, eyeY - 4, er * 2, 8, 4, col);
    c.fillRoundRect(rx - er, eyeY - 4, er * 2, 8, 4, col);
  }

  // Mouth — lip-sync (open/closed) while speaking, otherwise per-emotion.
  const int mx = w / 2, my = 84;
  if (talkMouth >= 0) {
    if (talkMouth == 1) c.fillEllipse(mx, my, 18, 13, col);      // open
    else c.fillRoundRect(mx - 18, my - 3, 36, 6, 3, col);        // closed
  } else {
    switch (e) {
      case EMO_HAPPY: for (int i = -40; i <= 40; i++) c.fillRect(mx + i, my + 12 - (i * i) / 66, 2, 4, col); break;
      case EMO_SAD:   for (int i = -40; i <= 40; i++) c.fillRect(mx + i, my - 12 + (i * i) / 66, 2, 4, col); break;
      case EMO_THINK: c.fillCircle(mx + 22, my, 8, col); break;
      case EMO_LISTEN: c.fillEllipse(mx, my, 22, 14, col); break;
      default: c.fillRoundRect(mx - 26, my - 3, 52, 6, 3, col); break;
    }
  }

  // Speech bubble — ONLY while talking, and kept small (~3 lines, scrolls).
  if (bubbleOn && faceText[0]) {
    const int by = 150, bx = 10, bw = w - 20, bh = h - by - 6;  // small bottom band
    c.fillTriangle(mx - 8, by + 1, mx + 8, by + 1, mx, by - 9, TFT_WHITE);  // tail
    c.fillRoundRect(bx, by, bw, bh, 8, TFT_WHITE);
    c.drawRoundRect(bx, by, bw, bh, 8, col);
    c.setFont(&fonts::efontKR_16);
    c.setTextSize(1);
    c.setTextColor(TFT_BLACK);
    drawBubbleLines(c, faceText, revealGlyphs, bx + 8, by + 6, bw - 16, 20, (bh - 10) / 20);
    c.setFont(&fonts::Font0);
  } else if (faceText[0]) {
    // Not talking: just a compact status line at the bottom (no big box).
    c.setFont(&fonts::efontKR_16);
    c.setTextSize(1);
    c.setTextColor(TFT_CYAN);
    c.setCursor(8, h - 22);
    c.print(faceText);
    c.setFont(&fonts::Font0);
  }
  c.pushSprite(0, 0);
}

// Set emotion + status/speech text (also mirrored to serial). Replaces status().
void faceSay(Emotion e, const char *text, bool showBubble = false) {
  curEmo = e;
  bubbleOn = showBubble;  // bubble only when we pass true (i.e. while talking)
  snprintf(faceText, sizeof(faceText), "%s", text ? text : "");
  Serial.printf("[ui] %s\n", faceText);
  drawFace(e, true, -1);
}

// Lip-sync the mouth AND stream the caption text out (auto-scrolling) while the
// speaker plays. durationMs paces the reveal so the text finishes ~with the audio.
void animateMouthWhilePlaying(uint32_t durationMs) {
  const int total = countGlyphs(faceText);
  const uint32_t startT = millis();
  uint32_t lastMouth = 0;
  bool open = false;
  while (M5.Speaker.isPlaying()) {
    const uint32_t el = millis() - startT;
    int reveal = (durationMs > 0) ? static_cast<int>((uint64_t)total * el / durationMs) : total;
    if (reveal > total) reveal = total;
    if (millis() - lastMouth > 130) { open = !open; lastMouth = millis(); }
    drawFace(curEmo, true, open ? 1 : 0, reveal);
    delay(30);
  }
  drawFace(curEmo, true, 0, total);  // full text, mouth closed
}

// Phone-style pull-down status: a top bar with a battery gauge, % and a charging
// bolt. Drawn over the face for ~2.5s, then the face is restored.
void showBattery() {
  const int lvl = M5.Power.getBatteryLevel();                       // 0..100, -1 unknown
  const bool chg = (M5.Power.isCharging() == m5::Power_Class::is_charging);
  const int pct = (lvl < 0) ? 0 : (lvl > 100 ? 100 : lvl);
  const int w = M5.Display.width();

  M5.Display.fillRect(0, 0, w, 42, TFT_BLACK);
  M5.Display.fillRoundRect(4, 4, w - 8, 34, 6, 0x2124);  // dark bar

  // Battery glyph on the right.
  const int bw = 44, bh = 20, bx = w - bw - 18, by = 11;
  M5.Display.drawRoundRect(bx, by, bw, bh, 3, TFT_WHITE);
  M5.Display.fillRect(bx + bw, by + 6, 3, 8, TFT_WHITE);  // tip
  const int fillw = pct * (bw - 4) / 100;
  const uint16_t fc = (pct > 50) ? TFT_GREEN : (pct > 20 ? TFT_YELLOW : TFT_RED);
  if (fillw > 0) M5.Display.fillRect(bx + 2, by + 2, fillw, bh - 4, fc);
  if (chg) {  // charging bolt on the gauge
    const int cx = bx + bw / 2, cy = by + bh / 2;
    M5.Display.fillTriangle(cx + 2, by + 3, cx - 5, cy + 1, cx + 1, cy, TFT_BLACK);
    M5.Display.fillTriangle(cx + 1, cy, cx + 6, by + bh - 3, cx - 1, cy - 1, TFT_BLACK);
  }

  // Label on the left (Korean).
  M5.Display.setFont(&fonts::efontKR_16);
  M5.Display.setTextSize(1);
  M5.Display.setTextColor(TFT_WHITE);
  M5.Display.setCursor(14, 14);
  if (lvl < 0) M5.Display.print("배터리 정보 없음");
  else if (chg) M5.Display.printf("배터리 %d%%  충전중", pct);
  else M5.Display.printf("배터리 %d%%", pct);
  M5.Display.setFont(&fonts::Font0);

  delay(2500);
  drawFace(curEmo, true);  // restore the face
}

void faceInit() {
  face = new M5Canvas(&M5.Display);
  face->setPsram(true);
  face->createSprite(M5.Display.width(), M5.Display.height());
  faceSay(EMO_NEUTRAL, "");
}

// Show the captured frame full-screen for a moment — "what the robot saw".
void showPhoto(camera_fb_t *fb) {
  if (!fb || !fb->buf) return;
  M5.Display.setSwapBytes(CAM_SWAP_BYTES != 0);  // flip in config if colors look wrong
  M5.Display.pushImage(0, 0, fb->width, fb->height, reinterpret_cast<const uint16_t *>(fb->buf));
  M5.Display.setSwapBytes(false);
  M5.Display.setFont(&fonts::efontKR_16);
  M5.Display.setTextColor(TFT_WHITE);
  M5.Display.setCursor(6, 6);
  M5.Display.print("이거 봤어요");
  M5.Display.setFont(&fonts::Font0);
  delay(1500);
}

bool setupCamera() {
  camera_config_t c = makeCameraConfig();
  // CoreS3 shares ONE internal I2C bus (port 1, SDA=12/SCL=11) across the
  // AXP2101 PMIC, ES7210/AW88298 codecs, touch AND the camera SCCB. M5.begin()
  // already installed the I2C driver on that port, so esp_camera_init() cannot
  // install its own — that is the "i2c driver install error / sccb init err"
  // seen on hardware. Release M5's bus, let the camera configure GC0308, then
  // delete the camera's SCCB driver and re-acquire the bus for M5 (SCCB is idle
  // during DVP frame capture, so audio + touch keep working afterwards).
  M5.In_I2C.release();
  esp_err_t err = esp_camera_init(&c);
  i2c_driver_delete(static_cast<i2c_port_t>(CAM_SCCB_I2C_PORT));
  M5.In_I2C.begin();
  if (err != ESP_OK) {
    Serial.printf("[cam] init failed 0x%x (try CAM_SCCB_I2C_PORT=0)\n", err);
    return false;
  }
  // GC0308 image orientation. Function pointers are null-checked because the
  // sensor driver only wires up the ops it actually supports.
  sensor_t *s = esp_camera_sensor_get();
  if (s) {
    if (s->set_hmirror) s->set_hmirror(s, CAM_HMIRROR);
    if (s->set_vflip) s->set_vflip(s, CAM_VFLIP);
  }
  Serial.println("[cam] GC0308 init OK");
  return true;
}

// Returns a FRESH frame. The DVP ring buffers hold frames captured earlier
// (while idle), so we drop a couple of stale ones first — otherwise every turn
// reuses the same old image. Caller must esp_camera_fb_return() the result.
camera_fb_t *captureFresh() {
  // Drop a few frames: clears stale ring buffers AND lets the GC0308 auto-
  // exposure settle after an on-demand (cold) init so the photo isn't dark/green.
  for (int i = 0; i < 4; i++) {
    camera_fb_t *stale = esp_camera_fb_get();
    if (stale) esp_camera_fb_return(stale);
  }
  return esp_camera_fb_get();
}

bool touchPressed() {
  M5.update();
  return M5.Touch.getDetail().isPressed();
}

// Records mic PCM into the PSRAM buffer. In holdMode, stops when the touch is
// released; otherwise records the full window (used by the serial trigger).
// Returns the number of int16 samples captured.
size_t recordAudio(bool holdMode) {
  M5.Speaker.end();   // free the shared I2S before switching to the mic
  if (!M5.Mic.begin()) {
    Serial.println("[mic] begin failed");
    return 0;
  }
  size_t total = 0;
  Serial.println("[rec] recording...");
  while (total + kRecordChunk <= kMaxSamples) {
    if (M5.Mic.record(pcm + total, kRecordChunk, kSampleRate)) {
      while (M5.Mic.isRecording()) delay(1);
      total += kRecordChunk;
    }
    if (holdMode && !touchPressed()) break;
  }
  M5.Mic.end();
  Serial.printf("[rec] %u samples\n", static_cast<unsigned>(total));
  return total;
}

// Auto-amplify the recorded PCM toward a target peak so quiet mic audio isn't
// dropped by the server's speech detector (the cause of the /api/see 422). The
// logged peak also tells us if the mic captured anything at all.
void applyMicGain(int16_t *buf, size_t n) {
  int32_t peak = 0;
  for (size_t i = 0; i < n; i++) {
    int32_t a = buf[i] < 0 ? -buf[i] : buf[i];
    if (a > peak) peak = a;
  }
  int gain = (peak > 0) ? (18000 / peak) : 1;
  if (gain < 1) gain = 1;
  if (gain > 10) gain = 10;
  if (gain > 1) {
    for (size_t i = 0; i < n; i++) {
      int32_t v = static_cast<int32_t>(buf[i]) * gain;
      buf[i] = (v > 32767) ? 32767 : (v < -32768 ? -32768 : static_cast<int16_t>(v));
    }
  }
  Serial.printf("[mic] peak=%d gain=%dx\n", static_cast<int>(peak), gain);
}

// Plays a WAV body on the speaker: parse the sample rate from the 44-byte
// header, skip the header, stream the PCM to the AW88298 via M5.Speaker.
void playWav(const uint8_t *wav, size_t len) {
  if (len <= 44) return;
  uint32_t rate = static_cast<uint32_t>(wav[24]) | (static_cast<uint32_t>(wav[25]) << 8) |
                  (static_cast<uint32_t>(wav[26]) << 16) | (static_cast<uint32_t>(wav[27]) << 24);
  if (rate < 8000 || rate > 48000) rate = kSampleRate;  // fall back if not a std WAV
  const int16_t *samples = reinterpret_cast<const int16_t *>(wav + 44);
  size_t n = (len - 44) / 2;

  M5.Mic.end();  // free the shared I2S before switching to the speaker
  M5.Speaker.begin();
  M5.Speaker.setVolume(kSpeakerVolume);
  M5.Speaker.playRaw(samples, n, rate, false);
  // Stream the caption + lip-sync, paced to the audio length.
  const uint32_t durationMs = (rate > 0) ? static_cast<uint32_t>((uint64_t)n * 1000 / rate) : 0;
  animateMouthWhilePlaying(durationMs);
  M5.Speaker.end();
}

// Begins an HTTP(S) request. For https URLs (free tunnels like cloudflared/
// ngrok) it uses an insecure TLS client — cert validation is OFF so we don't
// have to bundle a CA. Fine for a hobby tunnel; do not send secrets you would
// not accept being MITM'd. `secure`/`plain` must outlive the request.
bool httpBegin(HTTPClient &http, WiFiClientSecure &secure, WiFiClient &plain, const String &url) {
  if (url.startsWith("https:")) {
    secure.setInsecure();
    return http.begin(secure, url);
  }
  return http.begin(plain, url);
}

// Builds multipart/form-data with the audio PCM and JPEG frame, POSTs /api/see.
// Returns the response JSON body (empty on failure). Identical contract to the
// Waveshare firmware so the 4090 gateway is unchanged.
String postSee(const uint8_t *audio, size_t audioLen, const uint8_t *jpeg, size_t jpegLen) {
  const String boundary = "----alienrobotESP32boundary";
  const String head =
      "--" + boundary + "\r\n"
      "Content-Disposition: form-data; name=\"audio\"; filename=\"a.pcm\"\r\n"
      "Content-Type: application/octet-stream\r\n\r\n";
  const String midA = "\r\n--" + boundary + "\r\n";
  const String imgHead =
      "Content-Disposition: form-data; name=\"image\"; filename=\"f.jpg\"\r\n"
      "Content-Type: image/jpeg\r\n\r\n";
  const String tail = "\r\n--" + boundary + "--\r\n";

  // Image part is optional: when the camera is unavailable (jpegLen == 0) we
  // POST audio-only so the voice path (STT -> LLM -> TTS) can still be validated
  // during bring-up. The gateway may require the image for a vision turn.
  const bool hasImage = (jpeg != nullptr && jpegLen > 0);
  size_t bodyLen = head.length() + audioLen +
                   (hasImage ? midA.length() + imgHead.length() + jpegLen : 0) +
                   tail.length();
  uint8_t *body = static_cast<uint8_t *>(ps_malloc(bodyLen));
  if (!body) {
    Serial.println("[http] ps_malloc body failed");
    return String();
  }
  size_t p = 0;
  memcpy(body + p, head.c_str(), head.length()); p += head.length();
  memcpy(body + p, audio, audioLen); p += audioLen;
  if (hasImage) {
    memcpy(body + p, midA.c_str(), midA.length()); p += midA.length();
    memcpy(body + p, imgHead.c_str(), imgHead.length()); p += imgHead.length();
    memcpy(body + p, jpeg, jpegLen); p += jpegLen;
  }
  memcpy(body + p, tail.c_str(), tail.length()); p += tail.length();

  HTTPClient http;
  WiFiClientSecure secure;
  WiFiClient plain;
  httpBegin(http, secure, plain, String(AI_SERVER_BASE_URL) + "/api/see");
  // A vision turn is STT + Brain180 (vision LLM) + TTS on the server — easily
  // several seconds. The HTTPClient default read timeout is only 5s, so the
  // device was giving up before the answer arrived (looked like "no reply").
  http.setConnectTimeout(15000);
  http.setTimeout(60000);
  if (strlen(DEVICE_TOKEN) > 0) http.addHeader("X-Device-Token", DEVICE_TOKEN);
  http.addHeader("Content-Type", "multipart/form-data; boundary=" + boundary);
  uint32_t t0 = millis();
  int code = http.POST(body, bodyLen);
  g_seeCode = code;
  Serial.printf("[http] /api/see -> %d (%lums)\n", code, static_cast<unsigned long>(millis() - t0));
  String resp;
  if (code == 200) resp = http.getString();
  http.end();
  free(body);
  return resp;
}

// Fetches audio_url (relative to the gateway) into PSRAM and plays it.
void fetchAndPlay(const String &audioUrl) {
  if (audioUrl.isEmpty()) return;
  HTTPClient http;
  WiFiClientSecure secure;
  WiFiClient plain;
  // audioUrl may be a full https URL or a path relative to the gateway base.
  String full = audioUrl.startsWith("http") ? audioUrl : (String(AI_SERVER_BASE_URL) + audioUrl);
  httpBegin(http, secure, plain, full);
  http.setConnectTimeout(15000);
  http.setTimeout(60000);
  if (strlen(DEVICE_TOKEN) > 0) http.addHeader("X-Device-Token", DEVICE_TOKEN);
  int code = http.GET();
  if (code == 200) {
    int len = http.getSize();
    if (len > 0) {
      uint8_t *wav = static_cast<uint8_t *>(ps_malloc(len));
      if (wav) {
        WiFiClient *stream = http.getStreamPtr();
        int got = 0;
        while (http.connected() && got < len) {
          int avail = stream->available();
          if (avail > 0) got += stream->readBytes(wav + got, min(avail, len - got));
          else delay(1);
        }
        playWav(wav, got);
        free(wav);
      }
    }
  } else {
    Serial.printf("[http] audio GET -> %d\n", code);
  }
  http.end();
}

void handleTurn(bool holdMode) {
  if (!pcm) {  // PSRAM record buffer never allocated — cannot record
    Serial.println("[turn] no PSRAM buffer, abort");
    faceSay(EMO_SAD, "PSRAM 없음");
    return;
  }

  faceSay(EMO_LISTEN, "듣는 중...");
  size_t samples = recordAudio(holdMode);
  if (samples < kSampleRate / 4) {  // < ~0.25s → ignore accidental taps
    Serial.println("[turn] too short, skip");
    faceSay(EMO_NEUTRAL, "너무 짧아요 — 길게 말해줘");
    return;
  }
  applyMicGain(pcm, samples);  // boost quiet audio so STT hears it

  // Camera is optional. When available: grab one frame, SHOW it on the display
  // ("what the robot saw"), then software-encode it to JPEG for the upload.
  // On-demand camera: init only for this capture, then deinit. The cam_task
  // stack overflows if the camera runs continuously (the reboot cause), so we
  // never leave it running at idle.
  uint8_t *jpeg = nullptr;
  size_t jpegLen = 0;
  if (cameraOk && setupCamera()) {
    camera_fb_t *fb = captureFresh();
    if (fb) {
      showPhoto(fb);  // display the captured photo for ~1.5s
      if (!frame2jpg(fb, kJpegQuality, &jpeg, &jpegLen)) Serial.println("[cam] frame2jpg failed");
      esp_camera_fb_return(fb);
    } else {
      Serial.println("[cam] fb_get failed, audio-only");
    }
    esp_camera_deinit();  // stop cam_task right away — avoids the stack-overflow reboot
  } else {
    Serial.println("[turn] camera unavailable, audio-only");
  }

  faceSay(EMO_THINK, "생각 중...");
  String resp = postSee(reinterpret_cast<uint8_t *>(pcm), samples * 2, jpeg, jpegLen);
  if (jpeg) free(jpeg);
  if (resp.isEmpty()) {
    // Friendly, specific messages instead of a scary "server error".
    if (g_seeCode == 422) faceSay(EMO_NEUTRAL, "잘 안 들렸어요, 다시 말해줘");
    else if (g_seeCode == 400) faceSay(EMO_NEUTRAL, "너무 짧아요, 길게 말해줘");
    else if (g_seeCode == -1 || g_seeCode == 0) faceSay(EMO_SAD, "서버 연결 안됨");
    else faceSay(EMO_SAD, "서버 문제 (다시 시도)");
    return;
  }

  JsonDocument doc;
  if (deserializeJson(doc, resp)) {
    Serial.println("[turn] bad JSON response");
    faceSay(EMO_SAD, "응답 오류");
    return;
  }
  const char *transcript = doc["transcript"] | "";
  const char *answer = doc["answer"] | "";
  const char *audioUrl = doc["audio_url"] | "";
  Serial.printf("[turn] you: %s\n[turn] bot: %s\n", transcript, answer);
  // Happy face + speech bubble (only now, while talking) with the answer.
  faceSay(EMO_HAPPY, answer[0] ? answer : "(대답)", /*showBubble=*/true);
  fetchAndPlay(String(audioUrl));
  faceSay(EMO_NEUTRAL, "대기 중 — 화면 터치 / 't'");  // bubble off (default)
}

void connectWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  WiFi.setSleep(false);  // stop the idle drop/re-associate cycle (a reboot trigger)
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("WiFi connecting");
  faceSay(EMO_NEUTRAL, "WiFi 연결 중...");
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 30000) {
    delay(300);
    Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) {
    // Lower TX power trims the current spikes that brown-out the board on TX.
    WiFi.setTxPower(WIFI_POWER_13dBm);
    Serial.printf("\nWiFi connected: %s (tx 13dBm)\n", WiFi.localIP().toString().c_str());
  } else {
    Serial.println("\nWiFi FAILED (check config_cores3.h)");
    faceSay(EMO_SAD, "WiFi 실패");
  }
}
}  // namespace

void setup() {
  // Disable the ESP32 brown-out detector. Camera(continuous DVP) + WiFi TX spikes
  // draw enough current to trip it on a marginal USB supply, which showed up as
  // random reboots while idle (WiFi re-associate) and during the upload
  // ("thinking"). Use a good USB-C cable/port too — this only masks a weak rail.
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);

  auto cfg = M5.config();
  cfg.internal_mic = true;   // ES7210 dual mic
  cfg.internal_spk = true;   // AW88298 speaker amp
  M5.begin(cfg);             // powers AXP2101/AW9523 rails, display, codecs
  M5.Display.setRotation(1);

  Serial.begin(115200);
  delay(300);
  Serial.println("[boot] alien_robot CoreS3 fw route-A v15 (bubble-on-talk, small bubble, mic gain)");
  Serial.printf("[boot] gateway = %s\n", AI_SERVER_BASE_URL);

  // Log WHY it last rebooted — this pins down the "turns off and back on" cause:
  // PANIC = code crash, BROWNOUT = power sag, TASK_WDT/INT_WDT = watchdog.
  const char *rr = "?";
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:  rr = "POWERON (normal)"; break;
    case ESP_RST_SW:       rr = "SW"; break;
    case ESP_RST_PANIC:    rr = "PANIC (code crash)"; break;
    case ESP_RST_INT_WDT:  rr = "INT_WDT (watchdog)"; break;
    case ESP_RST_TASK_WDT: rr = "TASK_WDT (watchdog)"; break;
    case ESP_RST_WDT:      rr = "WDT (watchdog)"; break;
    case ESP_RST_BROWNOUT: rr = "BROWNOUT (power sag)"; break;
    case ESP_RST_EXT:      rr = "EXT"; break;
    default:               rr = "OTHER"; break;
  }
  Serial.printf("[boot] last reset reason = %s\n", rr);

  // Take this task off the watchdog so nothing here can trigger a WDT reboot.
  esp_task_wdt_deinit();

  faceInit();                       // robot face on the LCD
  faceSay(EMO_NEUTRAL, "부팅 중...");

  pcm = static_cast<int16_t *>(ps_malloc(kMaxSamples * sizeof(int16_t)));
  if (!pcm) Serial.println("[boot] PSRAM alloc failed — is N16R8 PSRAM enabled?");

  // Probe the camera once, then DEINIT so cam_task is not running at idle
  // (continuous cam_task overflows its stack -> reboot). It is re-inited on
  // demand for each capture in handleTurn().
  cameraOk = setupCamera();
  if (cameraOk) {
    esp_camera_deinit();
    Serial.println("[cam] on-demand mode (idle camera off)");
  }
  connectWifi();

  faceSay(EMO_NEUTRAL, "대기 중 — 화면 터치 / 't'");
  Serial.println("[boot] ready — hold the touch screen, or send 't' over serial, to talk");
}

void loop() {
  M5.update();
  static uint32_t lastBlink = 0;
  static uint32_t lastWifiChk = 0;
  static uint32_t lastHealth = 0;
  // Heap/uptime trend — a steadily falling heap means a leak (crash after a
  // while); a sudden reboot with heap still high points at power/watchdog.
  if (millis() - lastHealth > 30000) {
    lastHealth = millis();
    Serial.printf("[health] up=%lus heap=%u psram=%u wifi=%d\n",
                  static_cast<unsigned long>(millis() / 1000),
                  static_cast<unsigned>(ESP.getFreeHeap()),
                  static_cast<unsigned>(ESP.getFreePsram()),
                  WiFi.status() == WL_CONNECTED ? 1 : 0);
  }
  // Recover from idle WiFi drops without rebooting.
  if (millis() - lastWifiChk > 8000) {
    lastWifiChk = millis();
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("[wifi] dropped — reconnecting (no reboot)");
      WiFi.reconnect();
    }
  }
  auto td = M5.Touch.getDetail();
  int serialCmd = Serial.available() ? Serial.read() : -1;
  if (td.isPressed()) {
    if (td.base_y < 40) {
      // Top strip: a downward swipe pulls down the battery status (phone-style).
      bool swiped = false;
      while (true) {
        M5.update();
        auto d = M5.Touch.getDetail();
        if (!d.isPressed()) break;
        if (d.distanceY() > 50) { swiped = true; break; }
        delay(10);
      }
      if (swiped) {
        showBattery();
        while (touchPressed()) delay(10);  // consume the rest of the gesture
      }
      // released at the top without swiping -> ignore (not a talk trigger)
    } else {
      handleTurn(/*holdMode=*/true);       // hold anywhere on the face to talk
      while (touchPressed()) delay(10);     // wait for release
    }
  } else if (serialCmd == 'b') {
    showBattery();                          // serial 'b' = show battery (testing)
  } else if (serialCmd == 't') {
    handleTurn(/*holdMode=*/false);
  } else if (curEmo == EMO_NEUTRAL && millis() - lastBlink > 3500) {
    // Idle blink to keep the face alive.
    drawFace(EMO_NEUTRAL, false);
    delay(120);
    drawFace(EMO_NEUTRAL, true);
    lastBlink = millis();
  }
  delay(10);
}
