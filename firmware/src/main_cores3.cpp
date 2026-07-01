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

#include "config_cores3.h"

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
  c.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
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
char faceText[96] = "";

void drawFace(Emotion e, bool eyesOpen) {
  if (!face) return;
  M5Canvas &c = *face;
  const int w = c.width(), h = c.height();
  c.fillSprite(TFT_BLACK);

  const uint16_t col = TFT_CYAN;
  const int eyeY = h / 2 - 24;
  const int lx = w / 2 - 58, rx = w / 2 + 58;
  const int er = 30;  // eye radius

  // Eyes — blink collapses them to bars; THINK looks up-right.
  if (eyesOpen) {
    c.fillCircle(lx, eyeY, er, col);
    c.fillCircle(rx, eyeY, er, col);
    const int pdy = (e == EMO_THINK) ? -12 : 0;
    const int pdx = (e == EMO_THINK) ? 8 : 0;
    c.fillCircle(lx + pdx, eyeY + pdy, 12, TFT_BLACK);
    c.fillCircle(rx + pdx, eyeY + pdy, 12, TFT_BLACK);
  } else {
    c.fillRoundRect(lx - er, eyeY - 5, er * 2, 10, 5, col);
    c.fillRoundRect(rx - er, eyeY - 5, er * 2, 10, 5, col);
  }

  // Mouth per emotion.
  const int mx = w / 2, my = eyeY + 78;
  switch (e) {
    case EMO_HAPPY:  // upward smile
      for (int i = -44; i <= 44; i++) c.fillRect(mx + i, my + 14 - (i * i) / 70, 2, 4, col);
      break;
    case EMO_SAD:  // downward frown
      for (int i = -44; i <= 44; i++) c.fillRect(mx + i, my - 14 + (i * i) / 70, 2, 4, col);
      break;
    case EMO_THINK:  // small off-centre mouth
      c.fillCircle(mx + 26, my, 9, col);
      break;
    case EMO_LISTEN:  // open (listening)
      c.fillEllipse(mx, my, 26, 16, col);
      break;
    default:  // NEUTRAL
      c.fillRoundRect(mx - 30, my - 3, 60, 7, 3, col);
      break;
  }

  // Status / speech line (Korean-capable font).
  if (faceText[0]) {
    c.setFont(&fonts::efontKR_16);
    c.setTextSize(1);
    c.setTextColor(TFT_WHITE);
    c.setCursor(6, h - 20);
    c.print(faceText);
    c.setFont(&fonts::Font0);
  }
  c.pushSprite(0, 0);
}

// Set emotion + status/speech text (also mirrored to serial). Replaces status().
void faceSay(Emotion e, const char *text) {
  curEmo = e;
  snprintf(faceText, sizeof(faceText), "%s", text ? text : "");
  Serial.printf("[ui] %s\n", faceText);
  drawFace(e, true);
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
  M5.Display.setSwapBytes(true);  // esp_camera RGB565 is byte-swapped vs M5GFX
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
  Serial.println("[cam] GC0308 init OK");
  return true;
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
  while (M5.Speaker.isPlaying()) delay(5);  // buffer must persist until done
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
  if (strlen(DEVICE_TOKEN) > 0) http.addHeader("X-Device-Token", DEVICE_TOKEN);
  http.addHeader("Content-Type", "multipart/form-data; boundary=" + boundary);
  int code = http.POST(body, bodyLen);
  String resp;
  if (code == 200) {
    resp = http.getString();
  } else {
    Serial.printf("[http] /api/see -> %d\n", code);
  }
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
    faceSay(EMO_NEUTRAL, "너무 짧아요");
    return;
  }

  // Camera is optional. When available: grab one frame, SHOW it on the display
  // ("what the robot saw"), then software-encode it to JPEG for the upload.
  uint8_t *jpeg = nullptr;
  size_t jpegLen = 0;
  if (cameraOk) {
    camera_fb_t *fb = esp_camera_fb_get();
    if (fb) {
      showPhoto(fb);  // display the captured photo for ~1.5s
      if (!frame2jpg(fb, kJpegQuality, &jpeg, &jpegLen)) Serial.println("[cam] frame2jpg failed");
      esp_camera_fb_return(fb);
    } else {
      Serial.println("[cam] fb_get failed, audio-only");
    }
  } else {
    Serial.println("[turn] camera unavailable, audio-only");
  }

  faceSay(EMO_THINK, "생각 중...");
  String resp = postSee(reinterpret_cast<uint8_t *>(pcm), samples * 2, jpeg, jpegLen);
  if (jpeg) free(jpeg);
  if (resp.isEmpty()) {
    faceSay(EMO_SAD, "서버 오류");
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
  // Happy face + the spoken answer as the on-screen speech line, while it plays.
  faceSay(EMO_HAPPY, answer[0] ? answer : "(대답)");
  fetchAndPlay(String(audioUrl));
  faceSay(EMO_NEUTRAL, "대기 중 — 화면 터치 / 't'");
}

void connectWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("WiFi connecting");
  faceSay(EMO_NEUTRAL, "WiFi 연결 중...");
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 30000) {
    delay(300);
    Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\nWiFi connected: %s\n", WiFi.localIP().toString().c_str());
  } else {
    Serial.println("\nWiFi FAILED (check config_cores3.h)");
    faceSay(EMO_SAD, "WiFi 실패");
  }
}
}  // namespace

void setup() {
  auto cfg = M5.config();
  cfg.internal_mic = true;   // ES7210 dual mic
  cfg.internal_spk = true;   // AW88298 speaker amp
  M5.begin(cfg);             // powers AXP2101/AW9523 rails, display, codecs
  M5.Display.setRotation(1);

  Serial.begin(115200);
  delay(300);
  Serial.println("[boot] alien_robot CoreS3 fw route-A v5 (face + photo display)");
  Serial.printf("[boot] gateway = %s\n", AI_SERVER_BASE_URL);

  faceInit();                       // robot face on the LCD
  faceSay(EMO_NEUTRAL, "부팅 중...");

  pcm = static_cast<int16_t *>(ps_malloc(kMaxSamples * sizeof(int16_t)));
  if (!pcm) Serial.println("[boot] PSRAM alloc failed — is N16R8 PSRAM enabled?");

  cameraOk = setupCamera();
  connectWifi();

  faceSay(EMO_NEUTRAL, "대기 중 — 화면 터치 / 't'");
  Serial.println("[boot] ready — hold the touch screen, or send 't' over serial, to talk");
}

void loop() {
  M5.update();
  static uint32_t lastBlink = 0;
  bool touch = M5.Touch.getDetail().isPressed();
  bool serialTrig = (Serial.available() && Serial.read() == 't');
  if (touch) {
    handleTurn(/*holdMode=*/true);
    while (touchPressed()) delay(10);  // wait for release
  } else if (serialTrig) {
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
