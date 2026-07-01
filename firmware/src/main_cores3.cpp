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

#include "esp_camera.h"
#include "img_converters.h"  // frame2jpg() — software RGB565 -> JPEG encoder

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

// Bottom status line on the CoreS3 display (also mirrored to serial).
void status(const char *text) {
  Serial.printf("[ui] %s\n", text);
  const int h = M5.Display.height();
  const int w = M5.Display.width();
  M5.Display.fillRect(0, h - 30, w, 30, TFT_BLACK);
  M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
  M5.Display.setTextSize(2);
  M5.Display.setCursor(6, h - 26);
  M5.Display.print(text);
}

bool setupCamera() {
  camera_config_t c = makeCameraConfig();
  esp_err_t err = esp_camera_init(&c);
  if (err != ESP_OK) {
    Serial.printf("[cam] init failed 0x%x (try CAM_SCCB_I2C_PORT=0)\n", err);
    return false;
  }
  Serial.println("[cam] GC0308 init OK");
  return true;
}

// Captures one frame and software-encodes it to JPEG. Caller frees *outJpeg.
bool captureJpeg(uint8_t **outJpeg, size_t *outLen) {
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("[cam] fb_get failed");
    return false;
  }
  bool ok = frame2jpg(fb, kJpegQuality, outJpeg, outLen);
  esp_camera_fb_return(fb);
  if (!ok) Serial.println("[cam] frame2jpg failed");
  return ok;
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

  size_t bodyLen = head.length() + audioLen + midA.length() + imgHead.length() +
                   jpegLen + tail.length();
  uint8_t *body = static_cast<uint8_t *>(ps_malloc(bodyLen));
  if (!body) {
    Serial.println("[http] ps_malloc body failed");
    return String();
  }
  size_t p = 0;
  memcpy(body + p, head.c_str(), head.length()); p += head.length();
  memcpy(body + p, audio, audioLen); p += audioLen;
  memcpy(body + p, midA.c_str(), midA.length()); p += midA.length();
  memcpy(body + p, imgHead.c_str(), imgHead.length()); p += imgHead.length();
  memcpy(body + p, jpeg, jpegLen); p += jpegLen;
  memcpy(body + p, tail.c_str(), tail.length()); p += tail.length();

  HTTPClient http;
  http.begin(String(AI_SERVER_BASE_URL) + "/api/see");
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
  http.begin(String(AI_SERVER_BASE_URL) + audioUrl);
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
  status("listening...");
  size_t samples = recordAudio(holdMode);
  if (samples < kSampleRate / 4) {  // < ~0.25s → ignore accidental taps
    Serial.println("[turn] too short, skip");
    status("too short");
    return;
  }

  status("capturing...");
  uint8_t *jpeg = nullptr;
  size_t jpegLen = 0;
  if (!cameraOk || !captureJpeg(&jpeg, &jpegLen)) {
    Serial.println("[turn] camera capture failed");
    status("camera fail");
    return;
  }

  status("thinking...");
  String resp = postSee(reinterpret_cast<uint8_t *>(pcm), samples * 2, jpeg, jpegLen);
  free(jpeg);
  if (resp.isEmpty()) {
    status("server error");
    return;
  }

  JsonDocument doc;
  if (deserializeJson(doc, resp)) {
    Serial.println("[turn] bad JSON response");
    status("bad json");
    return;
  }
  const char *transcript = doc["transcript"] | "";
  const char *answer = doc["answer"] | "";
  const char *audioUrl = doc["audio_url"] | "";
  Serial.printf("[turn] you: %s\n[turn] bot: %s\n", transcript, answer);
  status(answer[0] ? answer : "(ok)");
  fetchAndPlay(String(audioUrl));
  status("ready — hold screen / 't'");
}

void connectWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("WiFi connecting");
  status("wifi...");
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 30000) {
    delay(300);
    Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\nWiFi connected: %s\n", WiFi.localIP().toString().c_str());
  } else {
    Serial.println("\nWiFi FAILED (check config_cores3.h)");
    status("wifi FAILED");
  }
}
}  // namespace

void setup() {
  auto cfg = M5.config();
  cfg.internal_mic = true;   // ES7210 dual mic
  cfg.internal_spk = true;   // AW88298 speaker amp
  M5.begin(cfg);             // powers AXP2101/AW9523 rails, display, codecs

  M5.Display.setRotation(1);
  M5.Display.fillScreen(TFT_BLACK);
  M5.Display.setTextColor(TFT_GREEN, TFT_BLACK);
  M5.Display.setTextSize(3);
  M5.Display.setCursor(8, 10);
  M5.Display.println("ALIEN ROBOT");

  Serial.begin(115200);
  delay(300);
  Serial.println("[boot] alien_robot CoreS3 fw route-A v1");

  pcm = static_cast<int16_t *>(ps_malloc(kMaxSamples * sizeof(int16_t)));
  if (!pcm) Serial.println("[boot] PSRAM alloc failed — is N16R8 PSRAM enabled?");

  cameraOk = setupCamera();
  connectWifi();

  status("ready — hold screen / 't'");
  Serial.println("[boot] ready — hold the touch screen, or send 't' over serial, to talk");
}

void loop() {
  M5.update();
  bool touch = M5.Touch.getDetail().isPressed();
  bool serialTrig = (Serial.available() && Serial.read() == 't');
  if (touch) {
    handleTurn(/*holdMode=*/true);
    while (touchPressed()) delay(10);  // wait for release
  } else if (serialTrig) {
    handleTurn(/*holdMode=*/false);
  }
  delay(10);
}
