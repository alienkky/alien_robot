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

#include <climits>  // INT_MIN sentinel for the optional pupil-offset args

#include <M5Unified.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>  // TLS for https tunnel URLs (cloudflared/ngrok)
#include <Preferences.h>       // persist the speaker volume across reboots (NVS)

#include "esp_camera.h"
#include "img_converters.h"  // frame2jpg() — software RGB565 -> JPEG encoder
#include "driver/i2c.h"      // i2c_driver_delete() — hand the SCCB bus back to M5
#include "soc/soc.h"          // brownout detector register
#include "soc/rtc_cntl_reg.h"
#include "esp_task_wdt.h"     // recover from hard hangs instead of staying frozen
#include "esp_system.h"       // esp_reset_reason() — log why it last rebooted

#include "config_cores3.h"

// Optional camera tuning knobs default here, so an older config_cores3.h that
// predates them still builds (only WIFI_*, AI_SERVER_BASE_URL, DEVICE_TOKEN are
// truly required). Override any of these in config_cores3.h to change them.
// Master camera switch — ON, with the freeze finally addressed. The permanent
// freeze was esp_camera_deinit() leaving the shared internal I2C bus (port 1)
// wedged, so the next touch read stalled the loop. v26 adds recoverSharedI2C()
// after every camera teardown: it drops the driver, bit-bangs the classic I2C
// bus-recovery (clock SCL to free a stuck slave + STOP), then re-begins M5's
// driver on a clean bus — so touch/audio always get a healthy bus back and the
// loop can't hang. Camera is still inited on-demand AFTER recordAudio() so the
// mic is never at risk. If a board still misbehaves, CAM_ENABLE 0 in
// config_cores3.h falls back to the proven audio-only loop.
#ifndef CAM_ENABLE
#define CAM_ENABLE 1
#endif
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
constexpr uint8_t kDefaultVolume = 160; // 0..255 default (2x the original 80)
uint8_t g_speakerVolume = kDefaultVolume; // runtime, user-adjustable via the panel
Preferences g_prefs;                      // NVS store — persists the volume
String g_wifiSsid;                         // optional NVS override; blank = config_cores3.h
String g_wifiPass;                         // optional NVS override; never printed
constexpr uint8_t kJpegQuality = 80;   // frame2jpg quality 0..100
constexpr uint32_t kWatchdogTimeoutSec = 12;      // hard hang -> automatic reboot
constexpr uint32_t kSeeHardRestartMs = 60000UL;   // backstop above 30s read + 15s connect
constexpr int32_t kMinSpeechPeakForServer = 300;  // below this, STT returns 422 and camera/I2C risk is wasted
constexpr uint32_t kTouchReleaseWaitMs = 1200;    // never wait forever on a stale touch state
constexpr uint32_t kStaleTouchRecoverMs = 1000;   // retry I2C recovery while stale-pressed is ignored
constexpr uint32_t kStaleTouchSoftUnlockMs = 3000; // stop blocking the loop if release stays stale

int16_t *pcm = nullptr;   // PSRAM record buffer (kMaxSamples int16 samples)
bool cameraOk = false;
bool g_ignoreTouchUntilRelease = false;
bool g_queueImmediateTurn = false;
// After a turn that failed to get an answer (too short/quiet, "잘 안 들렸어요 —
// 다시 말해줘", server error), the next screen tap should jump straight into
// listening — a plain tap, no press-and-hold. Set on every turn, cleared only
// when a real answer comes back.
bool g_tapToListenNext = false;
uint32_t g_lastStaleTouchRecover = 0;
uint32_t g_staleTouchIgnoreStart = 0;

void connectWifi();

void feedWatchdog() {
  esp_task_wdt_reset();
}

void armLoopWatchdog() {
  esp_err_t err = esp_task_wdt_init(kWatchdogTimeoutSec, true);
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    Serial.printf("[wdt] init failed 0x%x\n", static_cast<unsigned>(err));
  }
  err = esp_task_wdt_add(nullptr);  // watch the Arduino loop task
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    Serial.printf("[wdt] add loop task failed 0x%x\n", static_cast<unsigned>(err));
  }
  Serial.printf("[wdt] armed (%lus)\n", static_cast<unsigned long>(kWatchdogTimeoutSec));
}

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
// pupilDx/pupilDy shift both pupils (eye-darting during "생각 중"). When left at
// the sentinel (INT_MIN) THINK keeps its default up-right glance; any explicit
// value overrides it so the thinking loop can sweep the pupils left/right.
void drawFace(Emotion e, bool eyesOpen, int talkMouth = -1, int revealGlyphs = -1,
              int pupilDx = INT_MIN, int pupilDy = INT_MIN) {
  if (!face) return;
  M5Canvas &c = *face;
  const int w = c.width(), h = c.height();
  c.fillSprite(TFT_BLACK);

  const uint16_t col = TFT_CYAN;
  // Drop the whole face ~5mm on the 320x240 CoreS3 LCD (~7.9 px/mm → 40px).
  const int kFaceDrop = 40;
  const int eyeY = 44 + kFaceDrop;
  const int lx = w / 2 - 50, rx = w / 2 + 50;
  const int er = 24;

  // Eyes — blink collapses to bars. Pupil offset: caller override wins, else
  // THINK glances up-right, everyone else looks straight ahead.
  if (eyesOpen) {
    c.fillCircle(lx, eyeY, er, col);
    c.fillCircle(rx, eyeY, er, col);
    const int pdy = (pupilDy != INT_MIN) ? pupilDy : ((e == EMO_THINK) ? -10 : 0);
    const int pdx = (pupilDx != INT_MIN) ? pupilDx : ((e == EMO_THINK) ? 7 : 0);
    c.fillCircle(lx + pdx, eyeY + pdy, 10, TFT_BLACK);
    c.fillCircle(rx + pdx, eyeY + pdy, 10, TFT_BLACK);
  } else {
    c.fillRoundRect(lx - er, eyeY - 4, er * 2, 8, 4, col);
    c.fillRoundRect(rx - er, eyeY - 4, er * 2, 8, 4, col);
  }

  // Mouth — lip-sync (open/closed) while speaking, otherwise per-emotion.
  const int mx = w / 2, my = 84 + kFaceDrop;
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
bool pollAnswerInterruptTouch() {
  M5.update();
  feedWatchdog();
  auto d = M5.Touch.getDetail();
  if (d.wasPressed()) {
    // Tap while the robot is speaking: cut the current answer off RIGHT NOW and
    // queue the next turn so the user can keep talking. Stopping the speaker here
    // (not just when the call stack unwinds) makes the barge-in feel immediate.
    Serial.println("[touch] answer interrupted; stopping speech, queueing new turn");
    M5.Speaker.stop();
    g_queueImmediateTurn = true;
    return true;
  }
  return false;
}

bool waitAnswerInterruptWindow(uint32_t durationMs) {
  const uint32_t startT = millis();
  while (millis() - startT < durationMs) {
    if (pollAnswerInterruptTouch()) return true;
    delay(30);
  }
  return false;
}

// Returns true when the user tapped during the answer and a new turn was queued.
bool animateMouthWhilePlaying(uint32_t durationMs) {
  const int total = countGlyphs(faceText);
  const uint32_t startT = millis();
  uint32_t lastMouth = 0;
  bool open = false;
  while (M5.Speaker.isPlaying()) {
    feedWatchdog();
    if (pollAnswerInterruptTouch()) {
      return true;
    }
    const uint32_t el = millis() - startT;
    int reveal = (durationMs > 0) ? static_cast<int>((uint64_t)total * el / durationMs) : total;
    if (reveal > total) reveal = total;
    if (millis() - lastMouth > 130) { open = !open; lastMouth = millis(); }
    drawFace(curEmo, true, open ? 1 : 0, reveal);
    delay(30);
  }
  drawFace(curEmo, true, 0, total);  // full text, mouth closed
  return false;
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

// Short preview beep at the current level so the user HEARS the volume while
// adjusting. Speaker + mic share the I2S peripheral; caller owns begin()/end().
void volumeBeep() {
  M5.Speaker.setVolume(g_speakerVolume);
  M5.Speaker.tone(880, 90);  // async 90ms tone
}

bool hitRect(int px, int py, int x, int y, int w, int h) {
  return px >= x && px <= x + w && py >= y && py <= y + h;
}

void drawButton(int x, int y, int w, int h, const char *label,
                uint16_t border = TFT_WHITE, uint16_t text = TFT_WHITE) {
  M5.Display.drawRoundRect(x, y, w, h, 6, border);
  M5.Display.setFont(&fonts::Font0);
  M5.Display.setTextSize(1);
  M5.Display.setTextColor(text);
  const int tw = M5.Display.textWidth(label);
  M5.Display.setCursor(x + (w - tw) / 2, y + h / 2 - 4);
  M5.Display.print(label);
}

const char *activeWifiSsid() {
  return g_wifiSsid.length() > 0 ? g_wifiSsid.c_str() : WIFI_SSID;
}

const char *activeWifiPass() {
  return g_wifiSsid.length() > 0 ? g_wifiPass.c_str() : WIFI_PASSWORD;
}

void loadWifiCredentials() {
  g_wifiSsid = g_prefs.getString("wifi_ssid", "");
  g_wifiPass = g_prefs.getString("wifi_pass", "");
  Serial.printf("[boot] wifi source = %s, ssid = %s\n",
                g_wifiSsid.length() > 0 ? "saved" : "config",
                activeWifiSsid());
}

void saveWifiCredentials(const String &ssid, const String &pass) {
  g_wifiSsid = ssid;
  g_wifiPass = pass;
  g_prefs.putString("wifi_ssid", g_wifiSsid);
  g_prefs.putString("wifi_pass", g_wifiPass);
  Serial.printf("[wifi] saved ssid = %s\n", g_wifiSsid.c_str());
}

bool editWifiPassword(const String &ssid, String &pass) {
  const int w = M5.Display.width();   // CoreS3 @ rotation 1 = 320 px wide
  // Bigger keys than before (was 28x26): fill the 320px width and make the row
  // taller so a fingertip actually lands on one key. 10 keys * 30 + 9 * 2 = 318.
  const int keyW = 30, keyH = 34, gap = 2;
  const int row1Y = 68, row2Y = 106, row3Y = 144;   // step 38 = keyH + 4
  const int bottomY = 188, bottomH = 44;
  int mode = 0;  // 0 lower, 1 upper, 2 number/symbol
  const char *lowerRows[] = {"qwertyuiop", "asdfghjkl", "zxcvbnm"};
  const char *upperRows[] = {"QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM"};
  const char *symRows[] = {"0123456789", "-_.@#$!?/", "%&*=+~"};

  // Bottom action row — positions declared once so draw and hit-test never drift.
  const int modeX = 2,   modeW = 52;
  const int spaceX = 58, spaceW = 86;
  const int delX = 148,  delW = 52;
  const int okX = 204,   okW = 48;
  const int cancelX = 256, cancelW = 62;

  auto rowText = [&]() -> const char ** {
    if (mode == 1) return upperRows;
    if (mode == 2) return symRows;
    return lowerRows;
  };

  // Center each keyboard row for the given key count — used by BOTH draw and hit
  // test so a tapped pixel maps to the same key that was drawn there.
  auto rowStartX = [&](const char *row) {
    int n = 0;
    while (row[n]) n++;
    const int total = n * keyW + (n - 1) * gap;
    return (w - total) / 2;
  };

  auto drawKey = [&](int x, int y, char c) {
    M5.Display.drawRoundRect(x, y, keyW, keyH, 5, TFT_DARKGREY);
    M5.Display.setFont(&fonts::Font0);
    M5.Display.setTextSize(2);                 // 16px glyph — readable on a big key
    M5.Display.setTextColor(TFT_WHITE);
    char label[2] = {c, 0};
    const int tw = M5.Display.textWidth(label);
    M5.Display.setCursor(x + (keyW - tw) / 2, y + (keyH - 16) / 2);
    M5.Display.print(label);
    M5.Display.setTextSize(1);
  };

  auto redraw = [&]() {
    M5.Display.fillScreen(TFT_BLACK);
    // Korean title needs the KR font.
    M5.Display.setFont(&fonts::efontKR_16);
    M5.Display.setTextSize(1);
    M5.Display.setTextColor(TFT_CYAN);
    M5.Display.setCursor(6, 3);
    M5.Display.print("Wi-Fi 비밀번호 입력");
    M5.Display.setFont(&fonts::Font0);
    M5.Display.setTextColor(TFT_WHITE);
    M5.Display.setCursor(6, 24);
    M5.Display.printf("SSID: %.28s", ssid.c_str());
    // length counter (top-right) — proves a key registered even if text scrolls off
    M5.Display.setTextColor(TFT_DARKGREY);
    M5.Display.setCursor(w - 42, 24);
    M5.Display.printf("[%d]", pass.length());
    // PASSWORD IN PLAIN TEXT, size 2 (green) so the user can verify every tap.
    // This is a private on-device screen; masking only hurt usability here.
    M5.Display.setTextSize(2);
    M5.Display.setTextColor(TFT_GREEN);
    M5.Display.setCursor(6, 40);
    if (pass.length() == 0) {
      M5.Display.setTextColor(TFT_DARKGREY);
      M5.Display.print("________");            // empty field marker
    } else {
      // ~26 chars fit at size2/320px; show the tail so the latest key is visible.
      String shown = pass;
      if (shown.length() > 24) shown = "..." + shown.substring(shown.length() - 21);
      M5.Display.print(shown);
    }
    M5.Display.setTextSize(1);

    const char **rows = rowText();
    const int ys[] = {row1Y, row2Y, row3Y};
    for (int r = 0; r < 3; r++) {
      const int sx = rowStartX(rows[r]);
      for (int i = 0; rows[r][i]; i++) {
        drawKey(sx + i * (keyW + gap), ys[r], rows[r][i]);
      }
    }

    drawButton(modeX, bottomY, modeW, bottomH,
               mode == 0 ? "abc" : (mode == 1 ? "ABC" : "123"), TFT_CYAN, TFT_CYAN);
    drawButton(spaceX, bottomY, spaceW, bottomH, "SPACE", TFT_WHITE, TFT_WHITE);
    drawButton(delX, bottomY, delW, bottomH, "DEL", TFT_YELLOW, TFT_YELLOW);
    drawButton(okX, bottomY, okW, bottomH, "OK", TFT_GREEN, TFT_GREEN);
    drawButton(cancelX, bottomY, cancelW, bottomH, "CANCEL", TFT_RED, TFT_RED);
  };

  auto addKeyFromRow = [&](const char *row, int rowY, int x, int y) {
    if (y < rowY || y > rowY + keyH) return false;
    const int startX = rowStartX(row);
    for (int i = 0; row[i]; i++) {
      int kx = startX + i * (keyW + gap);
      if (hitRect(x, y, kx, rowY, keyW, keyH)) {
        if (pass.length() < 63) pass += row[i];
        return true;
      }
    }
    return false;
  };

  redraw();
  uint32_t lastAct = millis();
  while (true) {
    M5.update();
    feedWatchdog();
    auto d = M5.Touch.getDetail();
    if (d.wasPressed()) {
      const int x = d.x, y = d.y;
      bool changed = false;
      if (hitRect(x, y, modeX, bottomY, modeW, bottomH)) {
        mode = (mode + 1) % 3;
        changed = true;
      } else if (hitRect(x, y, spaceX, bottomY, spaceW, bottomH)) {
        if (pass.length() < 63) pass += ' ';
        changed = true;
      } else if (hitRect(x, y, delX, bottomY, delW, bottomH)) {
        if (pass.length() > 0) pass.remove(pass.length() - 1);
        changed = true;
      } else if (hitRect(x, y, okX, bottomY, okW, bottomH)) {
        return true;
      } else if (hitRect(x, y, cancelX, bottomY, cancelW, bottomH)) {
        return false;
      } else {
        const char **rows = rowText();
        changed = addKeyFromRow(rows[0], row1Y, x, y) ||
                  addKeyFromRow(rows[1], row2Y, x, y) ||
                  addKeyFromRow(rows[2], row3Y, x, y);
      }
      if (changed) {
        redraw();
        lastAct = millis();
      }
    }
    if (millis() - lastAct > 30000) return false;
    delay(20);
  }
}

void showWifiSettings() {
  const int w = M5.Display.width();
  const int rowX = 10, rowW = w - 20, rowH = 34, firstRowY = 50;
  const int rowsPerPage = 4;
  int page = 0;
  int networks = 0;

  auto drawScanning = [&]() {
    M5.Display.fillScreen(TFT_BLACK);
    M5.Display.setFont(&fonts::Font0);
    M5.Display.setTextSize(1);
    M5.Display.setTextColor(TFT_CYAN);
    M5.Display.setCursor(12, 18);
    M5.Display.print("WiFi scanning...");
  };

  auto scan = [&]() {
    drawScanning();
    feedWatchdog();
    // Why the scan "didn't work": boot's connectWifi() sets setAutoReconnect(true),
    // so when the saved/target AP is missing the driver keeps hammering association
    // in the background (the endless NO_AP_FOUND log). An in-progress association
    // makes WiFi.scanNetworks() come back WIFI_SCAN_FAILED(-2) or 0 — an empty list.
    // Fix: free the radio first (stop reconnect + drop the half-open association),
    // force STA, then scan with a few retries since the first post-disconnect scan
    // can still return busy.
    WiFi.setAutoReconnect(false);
    WiFi.disconnect(false, false);   // radio stays ON, saved AP kept
    delay(150);
    WiFi.mode(WIFI_STA);
    networks = 0;
    for (int attempt = 0; attempt < 3; attempt++) {
      WiFi.scanDelete();
      feedWatchdog();
      const int n = WiFi.scanNetworks(/*async=*/false, /*show_hidden=*/true);
      Serial.printf("[wifi] scan attempt %d -> %d\n", attempt + 1, n);
      if (n > 0) { networks = n; break; }   // got APs
      networks = 0;                          // n == 0 (empty) or n < 0 (failed)
      delay(400);                            // let the radio settle, then retry
    }
    WiFi.setAutoReconnect(true);   // restore normal hold-connection behavior
    page = 0;
    Serial.printf("[wifi] scan found %d networks\n", networks);
  };

  auto redraw = [&]() {
    M5.Display.fillScreen(TFT_BLACK);
    M5.Display.setFont(&fonts::Font0);
    M5.Display.setTextSize(1);
    M5.Display.setTextColor(TFT_CYAN);
    M5.Display.setCursor(8, 8);
    M5.Display.print("WiFi setup");
    M5.Display.setTextColor(TFT_WHITE);
    M5.Display.setCursor(8, 26);
    M5.Display.printf("Current: %.24s", activeWifiSsid());

    const int start = page * rowsPerPage;
    for (int i = 0; i < rowsPerPage; i++) {
      const int idx = start + i;
      const int y = firstRowY + i * (rowH + 4);
      if (idx >= networks) {
        M5.Display.drawRoundRect(rowX, y, rowW, rowH, 6, TFT_DARKGREY);
        continue;
      }
      const String ssid = WiFi.SSID(idx);
      const int32_t rssi = WiFi.RSSI(idx);
      const bool locked = WiFi.encryptionType(idx) != WIFI_AUTH_OPEN;
      M5.Display.drawRoundRect(rowX, y, rowW, rowH, 6, TFT_WHITE);
      M5.Display.setCursor(rowX + 8, y + 8);
      M5.Display.printf("%c %.21s  %ld", locked ? '*' : ' ', ssid.c_str(), static_cast<long>(rssi));
    }

    // Empty result: say so in Korean so a 0-network scan reads as "scanned, none
    // here" (turn on a 2.4GHz hotspot) rather than a frozen/blank panel.
    if (networks <= 0) {
      M5.Display.setFont(&fonts::efontKR_16);
      M5.Display.setTextColor(TFT_YELLOW);
      M5.Display.setCursor(rowX + 6, firstRowY + 8);
      M5.Display.print("검색된 Wi-Fi 없음");
      M5.Display.setCursor(rowX + 6, firstRowY + 30);
      M5.Display.print("2.4GHz 핫스팟 켜고 RESCAN");
      M5.Display.setFont(&fonts::Font0);
    }

    drawButton(8, 206, 64, 28, "BACK", TFT_RED, TFT_RED);
    drawButton(82, 206, 72, 28, "RESCAN", TFT_WHITE, TFT_WHITE);
    drawButton(164, 206, 64, 28, "NEXT", TFT_CYAN, TFT_CYAN);
    drawButton(238, 206, 74, 28, "CLEAR", TFT_YELLOW, TFT_YELLOW);
  };

  scan();
  redraw();
  uint32_t lastAct = millis();
  while (true) {
    M5.update();
    feedWatchdog();
    auto d = M5.Touch.getDetail();
    if (d.wasPressed()) {
      const int x = d.x, y = d.y;
      if (hitRect(x, y, 8, 206, 64, 28)) {
        break;
      } else if (hitRect(x, y, 82, 206, 72, 28)) {
        WiFi.scanDelete();
        scan();
        redraw();
      } else if (hitRect(x, y, 164, 206, 64, 28)) {
        const int maxPage = networks > 0 ? (networks - 1) / rowsPerPage : 0;
        page = (page >= maxPage) ? 0 : page + 1;
        redraw();
      } else if (hitRect(x, y, 238, 206, 74, 28)) {
        g_prefs.remove("wifi_ssid");
        g_prefs.remove("wifi_pass");
        g_wifiSsid = "";
        g_wifiPass = "";
        Serial.println("[wifi] saved credentials cleared; using config");
        redraw();
      } else {
        for (int i = 0; i < rowsPerPage; i++) {
          const int idx = page * rowsPerPage + i;
          const int rowY = firstRowY + i * (rowH + 4);
          if (idx < networks && hitRect(x, y, rowX, rowY, rowW, rowH)) {
            String ssid = WiFi.SSID(idx);
            String pass = (ssid == g_wifiSsid) ? g_wifiPass : "";
            if (editWifiPassword(ssid, pass)) {
              saveWifiCredentials(ssid, pass);
              WiFi.scanDelete();
              M5.Display.fillScreen(TFT_BLACK);
              M5.Display.setCursor(12, 18);
              M5.Display.setTextColor(TFT_CYAN);
              M5.Display.print("Connecting saved WiFi...");
              // Bug: right after OK the new creds often would NOT associate, yet a
              // reboot connected fine. Cause: the scan leaves the radio in a stale
              // state and disconnect() with the radio still ON doesn't clear it, so
              // the first begin() stalls. Power the radio fully OFF, then let
              // connectWifi() bring STA up clean — exactly the fresh-boot path that
              // always worked.
              WiFi.disconnect(true, false);   // wifioff = true → clear stale scan/assoc
              delay(400);
              connectWifi();                  // re-inits WIFI_STA + begin() from clean
              delay(300);
              return;
            }
            redraw();
            break;
          }
        }
      }
      lastAct = millis();
    }
    if (millis() - lastAct > 30000) break;
    delay(20);
  }
  WiFi.scanDelete();
}

// Phone-style pull-UP panel (mirror of the top battery pull-down): adjust the
// TTS playback volume. Tap the bar to set a level, use −/+ for fine steps, tap
// "완료" (or 6s idle) to close. The new level is previewed with a beep and saved
// to NVS so it survives a reboot.
void showVolumeControl() {
  const int w = M5.Display.width(), h = M5.Display.height();
  const int barX = 24, barW = w - 48, barY = 90, barH = 40;
  const int btnW = 70, btnH = 50, btnY = h - btnH - 8;
  const int minusX = 16, plusX = w - 16 - btnW, doneX = w / 2 - btnW / 2;
  const int wifiX = w - 84, wifiY = 10, wifiW = 68, wifiH = 30;

  M5.Speaker.begin();                 // grab I2S for the preview beeps
  M5.Speaker.setVolume(g_speakerVolume);

  auto redraw = [&]() {
    const int pct = g_speakerVolume * 100 / 255;
    M5.Display.fillScreen(TFT_BLACK);
    M5.Display.setFont(&fonts::efontKR_16);
    M5.Display.setTextSize(1);
    M5.Display.setTextColor(TFT_CYAN);
    M5.Display.setCursor(20, 14);
    M5.Display.print("음량 조절");
    drawButton(wifiX, wifiY, wifiW, wifiH, "WiFi", TFT_CYAN, TFT_CYAN);
    M5.Display.setTextColor(TFT_WHITE);
    M5.Display.setTextSize(2);
    M5.Display.setCursor(w / 2 - 30, 40);
    M5.Display.printf("%d%%", pct);
    M5.Display.setTextSize(1);
    // volume bar (tappable to set level directly)
    M5.Display.drawRoundRect(barX, barY, barW, barH, 6, TFT_WHITE);
    const int fillw = g_speakerVolume * (barW - 4) / 255;
    if (fillw > 0) M5.Display.fillRoundRect(barX + 2, barY + 2, fillw, barH - 4, 5, TFT_CYAN);
    // 작게 / 완료 / 크게 buttons.
    // drawButton(WiFi) above switched the active font to Font0 and did NOT restore
    // it, so we MUST re-select the Korean font here — otherwise "완료" renders as
    // garbage boxes and the volume +/- glyphs come out tiny/off-center (their
    // cursor offsets were sized for the 16px KR font, not 8px Font0). This was the
    // "음량조절 글씨가 이상하게 나온다" bug. Labels are centered with textWidth so
    // they stay put regardless of glyph width.
    M5.Display.setFont(&fonts::efontKR_16);
    M5.Display.setTextSize(1);
    M5.Display.setTextColor(TFT_WHITE);
    auto centeredLabel = [&](int bx, int bw, const char *label) {
      const int tw = M5.Display.textWidth(label);
      M5.Display.setCursor(bx + (bw - tw) / 2, btnY + btnH / 2 - 8);
      M5.Display.print(label);
    };
    M5.Display.drawRoundRect(minusX, btnY, btnW, btnH, 8, TFT_WHITE);
    centeredLabel(minusX, btnW, "작게");                 // − : quieter
    M5.Display.drawRoundRect(plusX, btnY, btnW, btnH, 8, TFT_WHITE);
    centeredLabel(plusX, btnW, "크게");                  // + : louder
    M5.Display.drawRoundRect(doneX, btnY, btnW, btnH, 8, TFT_GREEN);
    centeredLabel(doneX, btnW, "완료");
    M5.Display.setFont(&fonts::Font0);
  };
  redraw();

  uint32_t lastAct = millis();
  while (true) {
    M5.update();
    feedWatchdog();
    auto d = M5.Touch.getDetail();
    if (d.wasPressed()) {
      const int x = d.x, y = d.y;
      bool changed = false, done = false;
      if (hitRect(x, y, wifiX, wifiY, wifiW, wifiH)) {
        M5.Speaker.end();
        showWifiSettings();
        M5.Speaker.begin();
        M5.Speaker.setVolume(g_speakerVolume);
        redraw();
        lastAct = millis();
        continue;
      } else if (y >= btnY && y <= btnY + btnH) {          // button row
        if (x >= minusX && x <= minusX + btnW) {
          g_speakerVolume = (g_speakerVolume <= 15) ? 0 : g_speakerVolume - 15;
          changed = true;
        } else if (x >= plusX && x <= plusX + btnW) {
          g_speakerVolume = (g_speakerVolume >= 240) ? 255 : g_speakerVolume + 15;
          changed = true;
        } else if (x >= doneX && x <= doneX + btnW) {
          done = true;
        }
      } else if (y >= barY - 12 && y <= barY + barH + 12 &&
                 x >= barX && x <= barX + barW) {    // tap the bar → set level
        int v = (x - barX) * 255 / barW;
        g_speakerVolume = static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v));
        changed = true;
      }
      if (changed) { redraw(); volumeBeep(); lastAct = millis(); }
      if (done) break;
    }
    if (millis() - lastAct > 6000) break;  // auto-close when idle
    delay(20);
  }

  M5.Speaker.end();                        // release I2S for the next mic record
  g_prefs.putUChar("vol", g_speakerVolume);  // persist across reboots
  Serial.printf("[vol] set to %d (%d%%)\n", g_speakerVolume, g_speakerVolume * 100 / 255);
  drawFace(curEmo, true);                  // restore the face
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

// Detect-capable, mic-safe camera bring-up. CoreS3 shares ONE internal I2C bus
// (port 1) across the AXP2101 PMIC, ES7210 mic / AW88298 speaker codecs, FT6336
// touch AND the GC0308 SCCB. Hard-won on hardware (docs/lessons-cores3.md):
//   • The "reuse M5's driver" path (v18) could NOT probe the sensor AND left the
//     mic silent — worst case. So we go back to letting esp_camera_init install
//     its OWN SCCB driver, which actually detects the GC0308.
//   • SCCB is only needed to CONFIGURE the sensor, not to grab DVP frames. So
//     right after init we set orientation, delete the camera's SCCB driver, and
//     hand port 1 back to M5 — touch + audio keep the bus for the rest of the turn.
//   • This is only ever called from handleTurn AFTER recordAudio(), never at boot,
//     so a camera hiccup can never poison the mic for a whole session, and the
//     current turn's audio is already captured before we touch the bus.
bool setupCamera() {
  camera_config_t c = makeCameraConfig();
  M5.In_I2C.release();                    // free port 1 so the camera can probe SCCB
  esp_err_t err = esp_camera_init(&c);
  if (err == ESP_OK) {
    // Orientation must be written while the camera's SCCB driver is still alive.
    sensor_t *s = esp_camera_sensor_get();
    if (s) {
      if (s->set_hmirror) s->set_hmirror(s, CAM_HMIRROR);
      if (s->set_vflip) s->set_vflip(s, CAM_VFLIP);
    }
  }
  i2c_driver_delete(static_cast<i2c_port_t>(CAM_SCCB_I2C_PORT));
  M5.In_I2C.begin();                      // M5 reclaims the shared bus, pass or fail
  if (err != ESP_OK) {
    Serial.printf("[cam] init failed 0x%x — audio-only this turn (try CAM_SCCB_I2C_PORT=0)\n", err);
    return false;
  }
  Serial.println("[cam] GC0308 init OK");
  return true;
}

// Full recovery of the shared internal I2C bus after a camera turn. THIS is the
// fix for the freeze: esp_camera_deinit() could leave port 1 wedged (a slave —
// GC0308/PMIC — holding SDA, or the driver in a half state), and the next touch
// read would then stall the whole loop into a permanent freeze. So we:
//   1) drop any driver on the port,
//   2) bit-bang up to 9 SCL pulses to clock a stuck slave off SDA, then a STOP —
//      the textbook I2C bus-recovery sequence,
//   3) hand the freshly-clean pins back to M5's I2C driver.
// After this, touch + audio codecs get a healthy bus and the loop never hangs.
void recoverSharedI2C() {
  const int kSdaPin = 12, kSclPin = 11;  // CoreS3 internal I2C (port 1)
  M5.In_I2C.release();
  i2c_driver_delete(static_cast<i2c_port_t>(CAM_SCCB_I2C_PORT));  // harmless if gone

  // Clock the bus free: with SDA released (input), pulse SCL until the slave
  // stops holding SDA low (or 9 tries — one full byte + ack).
  pinMode(kSclPin, OUTPUT_OPEN_DRAIN);
  pinMode(kSdaPin, INPUT_PULLUP);
  digitalWrite(kSclPin, HIGH);
  for (int i = 0; i < 9; i++) {
    digitalWrite(kSclPin, LOW);  delayMicroseconds(5);
    digitalWrite(kSclPin, HIGH); delayMicroseconds(5);
    if (digitalRead(kSdaPin) == HIGH) break;  // slave let go — bus is free
  }
  // Emit a STOP condition (SDA low->high while SCL high) so masters resync.
  pinMode(kSdaPin, OUTPUT_OPEN_DRAIN);
  digitalWrite(kSdaPin, LOW);  delayMicroseconds(5);
  digitalWrite(kSclPin, HIGH); delayMicroseconds(5);
  digitalWrite(kSdaPin, HIGH); delayMicroseconds(5);

  M5.In_I2C.begin();  // reinstall M5's driver on the now-clean bus (touch/audio)
  Serial.println("[cam] shared I2C recovered (bus clocked free + re-begun)");
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

void waitForTouchReleaseBounded() {
  const uint32_t start = millis();
  while (millis() - start < kTouchReleaseWaitMs) {
    M5.update();
    feedWatchdog();
    if (!M5.Touch.getDetail().isPressed()) return;
    delay(10);
  }
  Serial.println("[touch] release wait timeout; ignoring stale pressed state until release");
  g_ignoreTouchUntilRelease = true;
  g_lastStaleTouchRecover = 0;
  g_staleTouchIgnoreStart = millis();
}

// Confirm a DELIBERATE vertical swipe from the touch that just went down.
// dirUp=true wants an upward drag (volume panel), false a downward drag (battery).
// Hardened against the phantom/stuck touches the camera leaves on the shared I2C
// bus — those were opening the volume panel by themselves:
//   • bigger travel (70px, was 50) and
//   • the finger must HOLD past the threshold for several consecutive reads, so a
//     one-frame jitter spike no longer counts, and
//   • the whole thing is time-bounded so a stuck phantom can never hang the loop.
bool confirmSwipe(bool dirUp) {
  const int kThreshold = 70;
  const int kNeedConsecutive = 3;
  const uint32_t kMaxMs = 700;
  int hits = 0;
  const uint32_t start = millis();
  while (millis() - start < kMaxMs) {
    M5.update();
    feedWatchdog();
    auto d = M5.Touch.getDetail();
    if (!d.isPressed()) return false;   // finger lifted → not a swipe
    const int dy = d.distanceY();
    const bool past = dirUp ? (dy < -kThreshold) : (dy > kThreshold);
    hits = past ? hits + 1 : 0;         // must be SUSTAINED, not a single jitter spike
    if (hits >= kNeedConsecutive) return true;
    delay(10);
  }
  return false;                         // timed out → treat as no swipe (never hangs)
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
  // If the mic did not actually enable, the ES7210 hands back pure silence
  // (the peak=0 seen on hardware). Log the enabled flag so a dead/muted codec
  // is distinguishable from a merely quiet room.
  Serial.printf("[mic] enabled=%d\n", M5.Mic.isEnabled() ? 1 : 0);

  size_t total = 0;
  Serial.println("[rec] recording...");
  // Keep the I2S DMA continuously fed: queue the next chunk as soon as record()
  // accepts it (it returns false only while its internal double-buffer is full),
  // and yield briefly when the queue is momentarily full. The previous code
  // waited on isRecording() after EVERY chunk, serialising capture and — at
  // start-up — potentially leaving the first buffers unfilled, a candidate for
  // the all-zero recording. We drain the last queued buffers once, after loop.
  while (total + kRecordChunk <= kMaxSamples) {
    feedWatchdog();
    if (!M5.Mic.record(pcm + total, kRecordChunk, kSampleRate)) {
      delay(1);            // double-buffer full — let it flush, then retry
      continue;
    }
    total += kRecordChunk;
    if (holdMode && !touchPressed()) break;
  }
  while (M5.Mic.isRecording()) {
    feedWatchdog();
    delay(1);
  }  // let the last queued chunk(s) finish
  M5.Mic.end();
  Serial.printf("[rec] %u samples\n", static_cast<unsigned>(total));
  return total;
}

// Auto-amplify the recorded PCM toward a target peak so quiet mic audio isn't
// dropped by the server's speech detector (the cause of the /api/see 422). The
// logged peak also tells us if the mic captured anything at all.
int32_t applyMicGain(int16_t *buf, size_t n) {
  int32_t peak = 0;
  int16_t s0 = n > 0 ? buf[0] : 0, s1 = n > 1 ? buf[1] : 0;
  int16_t s2 = n > 2 ? buf[2] : 0, s3 = n > 3 ? buf[3] : 0;  // raw, pre-gain
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
  // raw[] are the first four samples BEFORE gain: all-zero here + peak=0 means
  // the codec delivered pure silence (hardware/mute), not just a quiet room.
  Serial.printf("[mic] peak=%d gain=%dx raw=[%d %d %d %d]\n",
                static_cast<int>(peak), gain, s0, s1, s2, s3);
  return peak;
}

// Plays a WAV body on the speaker: parse the sample rate from the 44-byte
// header, skip the header, stream the PCM to the AW88298 via M5.Speaker.
bool playWav(const uint8_t *wav, size_t len) {
  if (len <= 44) return false;
  uint32_t rate = static_cast<uint32_t>(wav[24]) | (static_cast<uint32_t>(wav[25]) << 8) |
                  (static_cast<uint32_t>(wav[26]) << 16) | (static_cast<uint32_t>(wav[27]) << 24);
  if (rate < 8000 || rate > 48000) rate = kSampleRate;  // fall back if not a std WAV
  const int16_t *samples = reinterpret_cast<const int16_t *>(wav + 44);
  size_t n = (len - 44) / 2;

  M5.Mic.end();  // free the shared I2S before switching to the speaker
  M5.Speaker.begin();
  M5.Speaker.setVolume(g_speakerVolume);
  M5.Speaker.playRaw(samples, n, rate, false);
  // Stream the caption + lip-sync, paced to the audio length.
  const uint32_t durationMs = (rate > 0) ? static_cast<uint32_t>((uint64_t)n * 1000 / rate) : 0;
  bool interrupted = animateMouthWhilePlaying(durationMs);
  M5.Speaker.end();
  return interrupted;
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
  // A normal turn on the current server (Kimi via brain180) answers in ~3s, so a
  // response that has not arrived in 30s means the server is stuck/overloaded, not
  // "still working". The old 120s cap made the board sit on "생각 중" for a full
  // minute before failing, which is when the phantom-touch / volume-popup mess
  // piles up. Cap the wait at 30s so it fails fast and recovers; the underlying
  // server slowness is an infra concern flagged separately.
  http.setConnectTimeout(15000);
  http.setTimeout(30000);
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

// ── "생각 중" animation while the gateway thinks ───────────────────────
// The /api/see round-trip (STT + Brain180 vision + TTS) blocks for many seconds
// — long enough that a still face looks frozen. Run that POST on a background
// task and animate here instead: the pupils sweep left/right and the eyes blink.
// handleTurn blocks in the animation loop until the task signals done, so the
// audio/jpeg buffers it owns stay valid for the whole request.
struct SeeJob {
  const uint8_t *audio; size_t audioLen;
  const uint8_t *jpeg;  size_t jpegLen;
  volatile bool done;
  String resp;
};

void seeJobTask(void *param) {
  SeeJob *j = static_cast<SeeJob *>(param);
  j->resp = postSee(j->audio, j->audioLen, j->jpeg, j->jpegLen);
  j->done = true;
  vTaskDelete(nullptr);
}

// Runs postSee off-thread and animates the thinking face until it returns.
String postSeeThinking(const uint8_t *audio, size_t audioLen,
                       const uint8_t *jpeg, size_t jpegLen) {
  static SeeJob job;               // static: outlives this frame; one turn at a time
  job.audio = audio; job.audioLen = audioLen;
  job.jpeg = jpeg;   job.jpegLen = jpegLen;
  job.done = false;  job.resp = String();

  // 16 KB stack covers a plain-HTTP POST + getString(); a LAN URL does no TLS
  // handshake. Pin to core 0 (WiFi core) so the Arduino loop core stays free.
  TaskHandle_t h = nullptr;
  BaseType_t ok = xTaskCreatePinnedToCore(seeJobTask, "see", 16384, &job, 5, &h, 0);
  if (ok != pdPASS) {              // could not spawn — fall back to a blocking call
    Serial.println("[think] task spawn failed, blocking");
    return postSee(audio, audioLen, jpeg, jpegLen);
  }

  const uint32_t t0 = millis();
  uint32_t shownSec = 999;
  while (!job.done) {
    const uint32_t el = millis() - t0;
    if (el > kSeeHardRestartMs) {
      Serial.println("[wdt] /api/see stuck past hard limit; restarting");
      delay(100);
      ESP.restart();
    }
    feedWatchdog();
    // Live elapsed-seconds counter so a slow server (vision can take ~1 min)
    // reads as "working", not "frozen". Updated once per second into faceText,
    // which drawFace shows as the bottom status line.
    const uint32_t sec = el / 1000;
    if (sec != shownSec) {
      shownSec = sec;
      snprintf(faceText, sizeof(faceText), "생각 중... %lus", static_cast<unsigned long>(sec));
    }
    // NOTE: no touch reading here. The camera (GC0308) shares the I2C bus with the
    // touch controller and leaves phantom "pressed" reports for a while after a
    // capture — which is exactly the window this thinking loop runs in. A v35
    // attempt to open the volume panel from a bottom-swipe here false-triggered
    // (panel popped up mid-thinking with no real swipe), so mid-turn volume is
    // gone; volume is adjusted at idle, between turns, where touch is clean.
    // Pupils sweep L → centre → R → centre every ~1.4s; quick blink every ~2.4s.
    const int step = (el / 350) % 4;
    const int pdx = (step == 0) ? -9 : (step == 2) ? 9 : 0;
    const bool blink = (el % 2400) < 150;
    drawFace(EMO_THINK, !blink, -1, -1, pdx, -3);
    delay(45);
  }
  return job.resp;
}

// Fetches audio_url (relative to the gateway) into PSRAM and plays it.
bool fetchAndPlay(const String &audioUrl) {
  if (audioUrl.isEmpty()) return waitAnswerInterruptWindow(2500);
  HTTPClient http;
  WiFiClientSecure secure;
  WiFiClient plain;
  bool interrupted = false;
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
          feedWatchdog();
          if (pollAnswerInterruptTouch()) {
            interrupted = true;
            break;
          }
          int avail = stream->available();
          if (avail > 0) got += stream->readBytes(wav + got, min(avail, len - got));
          else delay(1);
        }
        if (!interrupted) interrupted = playWav(wav, got);
        free(wav);
      } else {
        interrupted = waitAnswerInterruptWindow(1500);
      }
    } else {
      interrupted = waitAnswerInterruptWindow(1500);
    }
  } else {
    Serial.printf("[http] audio GET -> %d\n", code);
    interrupted = waitAnswerInterruptWindow(1500);
  }
  http.end();
  return interrupted;
}

// allowCamera=false runs a pure audio-only turn (no camera init, no I2C
// recovery). Used for retry turns after an unheard/too-quiet result: the camera
// on the shared I2C bus is what corrupts the mic and jams touch, so retrying
// WITHOUT it lets the mic settle on a clean bus and breaks the failure spiral.
void handleTurn(bool holdMode, bool allowCamera = true) {
  if (!pcm) {  // PSRAM record buffer never allocated — cannot record
    Serial.println("[turn] no PSRAM buffer, abort");
    faceSay(EMO_SAD, "PSRAM 없음");
    return;
  }

  // Assume this turn may not land an answer; if so, the next tap should go
  // straight to listening. Cleared below once a real answer arrives.
  g_tapToListenNext = true;

  faceSay(EMO_LISTEN, "듣는 중...");
  size_t samples = recordAudio(holdMode);
  if (samples < kSampleRate / 4) {  // < ~0.25s → ignore accidental taps
    Serial.println("[turn] too short, skip");
    faceSay(EMO_NEUTRAL, "너무 짧아요 — 길게 말해줘");
    return;
  }
  int32_t micPeak = applyMicGain(pcm, samples);  // boost quiet audio so STT hears it
  if (micPeak < kMinSpeechPeakForServer) {
    Serial.printf("[turn] speech too quiet (peak=%d < %d), skip camera/server\n",
                  static_cast<int>(micPeak), static_cast<int>(kMinSpeechPeakForServer));
    faceSay(EMO_NEUTRAL, "소리가 작아요 — 다시 말해줘");
    return;
  }

  // Camera is optional. When available: grab one frame, SHOW it on the display
  // ("what the robot saw"), then software-encode it to JPEG for the upload.
  // On-demand camera: init only for this capture, then deinit. The cam_task
  // stack overflows if the camera runs continuously (the reboot cause), so we
  // never leave it running at idle.
  uint8_t *jpeg = nullptr;
  size_t jpegLen = 0;
  bool usedCamera = false;   // gates the I2C recovery below — no camera, nothing to recover
  if (cameraOk && allowCamera && setupCamera()) {
    usedCamera = true;
    camera_fb_t *fb = captureFresh();
    if (fb) {
      showPhoto(fb);  // display the captured photo for ~1.5s
      if (!frame2jpg(fb, kJpegQuality, &jpeg, &jpegLen)) Serial.println("[cam] frame2jpg failed");
      Serial.printf("[cam] captured %ux%u -> jpeg %u bytes\n",
                    static_cast<unsigned>(fb->width), static_cast<unsigned>(fb->height),
                    static_cast<unsigned>(jpegLen));
      esp_camera_fb_return(fb);
    } else {
      Serial.println("[cam] fb_get failed, audio-only");
    }
    esp_camera_deinit();  // stop cam_task right away — avoids the stack-overflow reboot
    // Recover the shared I2C bus ONCE, right after the camera touched it, so the
    // next touch read cannot stall the loop (the permanent-freeze cause). The old
    // extra recoveries in the error branches below were removed — they just churned
    // the bus (repeated i2c_driver_delete errors) without adding safety.
    recoverSharedI2C();
  } else if (cameraOk && !allowCamera) {
    Serial.println("[turn] audio-only retry — camera skipped to keep the mic/bus clean");
  } else {
    Serial.println("[turn] camera unavailable, audio-only");
  }

  faceSay(EMO_THINK, "생각 중...");
  // Off-thread POST + animated thinking face (pupils dart, eyes blink) so the
  // robot doesn't freeze during the multi-second server round-trip.
  String resp = postSeeThinking(reinterpret_cast<uint8_t *>(pcm), samples * 2, jpeg, jpegLen);
  if (jpeg) free(jpeg);
  if (resp.isEmpty()) {
    Serial.println("[turn] empty response");
    // No I2C recovery here: if the camera ran this turn it was already recovered
    // once above; if not (audio-only), there is nothing to recover.
    // Friendly, specific messages instead of a scary "server error".
    if (g_seeCode == 422) faceSay(EMO_NEUTRAL, "잘 안 들렸어요, 다시 말해줘");
    else if (g_seeCode == 400) faceSay(EMO_NEUTRAL, "너무 짧아요, 길게 말해줘");
    else if (g_seeCode == -11) faceSay(EMO_NEUTRAL, "서버가 느려요 — 다시 말해줘");
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
  // Pinpoint where voice breaks: "(none)" here = the server returned text but no
  // TTS audio_url (server/TTS side); a URL here but no sound = download/playback.
  Serial.printf("[turn] audio_url = %s\n", audioUrl[0] ? audioUrl : "(none)");
  g_tapToListenNext = false;   // got a real answer — next touch is normal hold-to-talk
  // Happy face + speech bubble (only now, while talking) with the answer.
  faceSay(EMO_HAPPY, answer[0] ? answer : "(대답)", /*showBubble=*/true);
  if (!fetchAndPlay(String(audioUrl)) && audioUrl[0]) {
    waitAnswerInterruptWindow(800);
  }
  if (usedCamera) recoverSharedI2C();   // only if the camera touched the bus this turn
  faceSay(EMO_NEUTRAL, "");  // idle: face only, no status text (bubble off)
}

void runQueuedImmediateTurns(uint8_t maxTurns = 2) {
  for (uint8_t i = 0; i < maxTurns && g_queueImmediateTurn; i++) {
    g_queueImmediateTurn = false;
    Serial.println("[turn] starting queued touch turn");
    handleTurn(/*holdMode=*/false);
  }
  if (g_queueImmediateTurn) {
    Serial.println("[turn] queued touch turn limit reached; dropping extra request");
    g_queueImmediateTurn = false;
  }
}

void connectWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  WiFi.setSleep(false);  // stop the idle drop/re-associate cycle (a reboot trigger)
  const char *ssid = activeWifiSsid();
  const char *pass = activeWifiPass();
  if (strlen(pass) > 0) WiFi.begin(ssid, pass);
  else WiFi.begin(ssid);
  Serial.print("WiFi connecting");
  faceSay(EMO_NEUTRAL, "WiFi 연결 중...");
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 30000) {
    feedWatchdog();
    delay(300);
    Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) {
    // Lower TX power trims the current spikes that brown-out the board on TX.
    WiFi.setTxPower(WIFI_POWER_13dBm);
    Serial.printf("\nWiFi connected: %s (tx 13dBm, ssid=%s)\n",
                  WiFi.localIP().toString().c_str(), ssid);
  } else {
    Serial.printf("\nWiFi FAILED (ssid=%s)\n", ssid);
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
  Serial.println("[boot] alien_robot CoreS3 fw route-A v42 (30s server timeout, fail fast)");
  Serial.printf("[boot] gateway = %s\n", AI_SERVER_BASE_URL);

  // Restore the saved speaker volume (defaults to kDefaultVolume on first boot).
  g_prefs.begin("robot", false);
  g_speakerVolume = g_prefs.getUChar("vol", kDefaultVolume);
  Serial.printf("[boot] volume = %d (%d%%)\n", g_speakerVolume, g_speakerVolume * 100 / 255);
  loadWifiCredentials();

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

  // Watch the loop task. If touch/I2C wedges again, reboot instead of staying
  // permanently frozen until power-cycled.
  armLoopWatchdog();

  faceInit();                       // robot face on the LCD
  faceSay(EMO_NEUTRAL, "부팅 중...");

  pcm = static_cast<int16_t *>(ps_malloc(kMaxSamples * sizeof(int16_t)));
  if (!pcm) Serial.println("[boot] PSRAM alloc failed — is N16R8 PSRAM enabled?");

  // NO boot-time camera probe. A camera init on the shared I2C bus can leave the
  // ES7210 mic silent, and doing it at boot poisons the mic for the WHOLE session
  // (that was the v18 regression). Instead the camera is inited strictly
  // on-demand inside handleTurn(), AFTER recordAudio() — so the mic always
  // records on a clean bus first, and a camera failure only costs that one turn's
  // image, never the voice loop. cameraOk here is just the master enable.
#if CAM_ENABLE
  cameraOk = true;
  Serial.println("[cam] on-demand mode — init per turn AFTER mic record (no boot probe)");
#else
  cameraOk = false;
  Serial.println("[cam] disabled (CAM_ENABLE=0) — audio-only, touch/mic first");
#endif
  connectWifi();

  faceSay(EMO_NEUTRAL, "");  // idle: face only, no on-screen status text
  Serial.println("[boot] ready — hold the touch screen, or send 't' over serial, to talk");
}

void loop() {
  M5.update();
  feedWatchdog();
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
  if (g_ignoreTouchUntilRelease) {
    if (!td.isPressed()) {
      g_ignoreTouchUntilRelease = false;
      Serial.println("[touch] stale pressed state cleared");
    } else {
      if (millis() - g_staleTouchIgnoreStart > kStaleTouchSoftUnlockMs) {
        g_ignoreTouchUntilRelease = false;
        Serial.println("[touch] stale pressed soft-unlocked; waiting for next touch edge");
      }
      if (cameraOk && millis() - g_lastStaleTouchRecover > kStaleTouchRecoverMs) {
        g_lastStaleTouchRecover = millis();
        Serial.println("[touch] stale pressed state; recovering I2C");
        recoverSharedI2C();
      }
    }
  }
  if (g_queueImmediateTurn) {
    runQueuedImmediateTurns();
    waitForTouchReleaseBounded();
    delay(10);
    return;
  }
  if (!g_ignoreTouchUntilRelease && td.wasPressed()) {
    if (td.base_y < 40) {
      // Top strip: a deliberate downward swipe pulls down the battery status.
      if (confirmSwipe(/*dirUp=*/false)) {
        showBattery();
        waitForTouchReleaseBounded();  // consume the rest of the gesture
      }
      // released/jittered at the top without a real swipe -> ignore
    } else if (td.base_y > 200) {
      // Bottom strip: a deliberate UPWARD swipe pulls up the volume panel. The
      // hardened confirmSwipe() stops the phantom-touch auto-open seen in the log.
      if (confirmSwipe(/*dirUp=*/true)) {
        showVolumeControl();
        waitForTouchReleaseBounded();  // consume the rest of the gesture
      }
      // tap/jitter at the bottom without a real swipe -> ignore (not a talk trigger)
    } else if (g_tapToListenNext) {
      // Right after a "잘 안 들렸어요 — 다시 말해줘" (or any failed turn), a plain
      // TAP goes straight into listening (fixed window, no need to keep holding).
      // Retry is AUDIO-ONLY (allowCamera=false): the camera is what corrupts the
      // mic and jams touch, so skipping it lets the retry actually succeed instead
      // of spiralling through more 422s and stale-touch recoveries.
      Serial.println("[turn] tap-to-listen (retry after unheard, audio-only)");
      handleTurn(/*holdMode=*/false, /*allowCamera=*/false);
      runQueuedImmediateTurns();
      waitForTouchReleaseBounded();
    } else {
      handleTurn(/*holdMode=*/true);       // hold the face (centre) to talk
      runQueuedImmediateTurns();
      waitForTouchReleaseBounded();         // wait for release, but never forever
    }
  } else if (serialCmd == 'b') {
    showBattery();                          // serial 'b' = show battery (testing)
  } else if (serialCmd == 'v') {
    showVolumeControl();                    // serial 'v' = volume panel (testing)
  } else if (serialCmd == 't') {
    handleTurn(/*holdMode=*/false);
    runQueuedImmediateTurns();
  } else if (curEmo == EMO_NEUTRAL && millis() - lastBlink > 3500) {
    // Idle blink to keep the face alive.
    drawFace(EMO_NEUTRAL, false);
    delay(120);
    drawFace(EMO_NEUTRAL, true);
    lastBlink = millis();
  }
  delay(10);
}
