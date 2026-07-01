// ─────────────────────────────────────────────────────────────────────
// Alien Robot — M3 firmware for Waveshare ESP32-S3-Touch-LCD-3.5B.
// Compiled ONLY in the `waveshare-s3-touch-lcd-3.5b` PlatformIO env.
//
// Flow (push-to-talk): hold BOOT → record mic (ES8311 ADC, I2S) + capture one
// OV5640 JPEG → POST multipart /api/see {audio, image} to the 4090 gateway →
// gateway runs STT + Brain180 tutor (vision) + TTS → returns {answer, audio_url}
// → GET the WAV and play it on the ES8311 DAC speaker.
//
// ⚠️ BUILD vs DEVICE: this file is build-verified with `pio run` only. The
// ES8311 register init (es8311.cpp) and exact I2S clocking are HARDWARE-
// UNVERIFIED — confirm on the physical board (serial log + scope/audio).
// Camera pins are verified against the xiaozhi board profile (docs/m3-board-bringup.md).
// ─────────────────────────────────────────────────────────────────────

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <driver/i2s.h>
#include "esp_camera.h"

#include "config_waveshare.h"
#include "es8311.h"

#ifndef I2S_COMM_FORMAT_STAND_I2S
#define I2S_COMM_FORMAT_STAND_I2S I2S_COMM_FORMAT_I2S
#endif

namespace {
constexpr int kSampleRate = 16000;
constexpr int kMaxRecordSeconds = 4;
constexpr size_t kMaxPcmBytes = kSampleRate * 2 * kMaxRecordSeconds;  // 16-bit mono
constexpr size_t kI2sChunkSamples = 512;
constexpr i2s_port_t kI2sPort = I2S_NUM_0;  // ES8311 is full-duplex on one bus

ES8311 codec;
uint8_t *pcmBuffer = nullptr;

void connectWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("WiFi connecting");
  while (WiFi.status() != WL_CONNECTED) {
    delay(300);
    Serial.print(".");
  }
  Serial.printf("\nWiFi connected: %s\n", WiFi.localIP().toString().c_str());
}

// Full-duplex I2S master: MCLK feeds the ES8311 (256×fs), DIN=mic, DOUT=spk.
bool setupI2S() {
  i2s_config_t config = {};
  config.mode = static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_RX | I2S_MODE_TX);
  config.sample_rate = kSampleRate;
  config.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  config.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;
  config.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  config.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  config.dma_buf_count = 8;
  config.dma_buf_len = 256;
  config.use_apll = true;                 // APLL for a clean 256×fs MCLK
  config.tx_desc_auto_clear = true;
  config.fixed_mclk = kSampleRate * 256;  // 4.096 MHz @ 16 kHz

  if (i2s_driver_install(kI2sPort, &config, 0, nullptr) != ESP_OK) {
    Serial.println("[i2s] driver install failed");
    return false;
  }

  i2s_pin_config_t pins = {};
  pins.mck_io_num = I2S_MCLK_PIN;
  pins.bck_io_num = I2S_BCLK_PIN;
  pins.ws_io_num = I2S_WS_PIN;
  pins.data_out_num = I2S_DOUT_PIN;
  pins.data_in_num = I2S_DIN_PIN;
  if (i2s_set_pin(kI2sPort, &pins) != ESP_OK) {
    Serial.println("[i2s] set pin failed");
    return false;
  }
  return true;
}

bool setupCamera() {
  camera_config_t config = {};
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = CAM_PIN_D0;
  config.pin_d1 = CAM_PIN_D1;
  config.pin_d2 = CAM_PIN_D2;
  config.pin_d3 = CAM_PIN_D3;
  config.pin_d4 = CAM_PIN_D4;
  config.pin_d5 = CAM_PIN_D5;
  config.pin_d6 = CAM_PIN_D6;
  config.pin_d7 = CAM_PIN_D7;
  config.pin_xclk = CAM_PIN_XCLK;
  config.pin_pclk = CAM_PIN_PCLK;
  config.pin_vsync = CAM_PIN_VSYNC;
  config.pin_href = CAM_PIN_HREF;
  // SCCB shares the ES8311 I2C bus (port 0) — reuse it instead of own pins.
  config.pin_sccb_sda = CAM_PIN_SIOD;  // -1
  config.pin_sccb_scl = CAM_PIN_SIOC;  // -1
  config.sccb_i2c_port = 0;
  config.pin_pwdn = CAM_PIN_PWDN;
  config.pin_reset = CAM_PIN_RESET;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.frame_size = FRAMESIZE_VGA;     // 640×480 — enough for vision, small payload
  config.jpeg_quality = 12;
  config.fb_count = 1;
  config.fb_location = CAMERA_FB_IN_PSRAM;
  config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("[cam] init failed: 0x%x\n", err);
    return false;
  }
  Serial.println("[cam] OV5640/OV2640 init OK");
  return true;
}

// Records until the button is released or the buffer is full. Returns byte count.
size_t recordAudio() {
  size_t total = 0;
  static int16_t chunk[kI2sChunkSamples];
  Serial.println("[rec] recording...");
  while (digitalRead(PIN_BUTTON) == LOW && total + sizeof(chunk) <= kMaxPcmBytes) {
    size_t bytesRead = 0;
    if (i2s_read(kI2sPort, chunk, sizeof(chunk), &bytesRead, pdMS_TO_TICKS(200)) == ESP_OK) {
      memcpy(pcmBuffer + total, chunk, bytesRead);
      total += bytesRead;
    }
  }
  Serial.printf("[rec] %u bytes\n", static_cast<unsigned>(total));
  return total;
}

// Speaker volume, 0..100 (software scale applied to the PCM before the DAC).
// Lower = quieter; 100 = original level. Set small (10) for quiet playback.
constexpr int kSpeakerVolume = 10;

// Plays a WAV body: skip the 44-byte header, scale each sample down to
// kSpeakerVolume percent, then stream the PCM to the I2S DAC.
void playWav(const uint8_t *wav, size_t len) {
  if (len <= 44) return;
  const int16_t *pcm = reinterpret_cast<const int16_t *>(wav + 44);
  size_t samples = (len - 44) / 2;
  static int16_t buf[512];
  size_t i = 0;
  while (i < samples) {
    size_t n = min(static_cast<size_t>(512), samples - i);
    // int32 intermediate avoids 16-bit overflow while scaling.
    for (size_t k = 0; k < n; k++) {
      buf[k] = static_cast<int16_t>((static_cast<int32_t>(pcm[i + k]) * kSpeakerVolume) / 100);
    }
    size_t off = 0;
    const size_t bytes = n * 2;
    while (off < bytes) {
      size_t written = 0;
      if (i2s_write(kI2sPort, reinterpret_cast<uint8_t *>(buf) + off, bytes - off,
                    &written, pdMS_TO_TICKS(200)) != ESP_OK) {
        return;
      }
      off += written;
    }
    i += n;
  }
}

// Builds multipart/form-data with the audio PCM and JPEG frame, POSTs /api/see.
// Returns the response JSON body (empty on failure).
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

// Fetches audio_url (relative to the gateway) and plays it.
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

void handleTurn() {
  size_t audioLen = recordAudio();
  if (audioLen < kSampleRate) {  // < ~0.25s → ignore accidental taps
    Serial.println("[turn] too short, skip");
    return;
  }

  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("[turn] camera capture failed");
    return;
  }

  String resp = postSee(pcmBuffer, audioLen, fb->buf, fb->len);
  esp_camera_fb_return(fb);
  if (resp.isEmpty()) return;

  JsonDocument doc;
  if (deserializeJson(doc, resp)) {
    Serial.println("[turn] bad JSON response");
    return;
  }
  const char *transcript = doc["transcript"] | "";
  const char *answer = doc["answer"] | "";
  const char *audioUrl = doc["audio_url"] | "";
  Serial.printf("[turn] you: %s\n[turn] bot: %s\n", transcript, answer);
  fetchAndPlay(String(audioUrl));
}
}  // namespace

void setup() {
  Serial.begin(115200);
  delay(200);
  pinMode(PIN_BUTTON, INPUT_PULLUP);

  pcmBuffer = static_cast<uint8_t *>(ps_malloc(kMaxPcmBytes));
  if (!pcmBuffer) {
    Serial.println("[boot] PSRAM alloc failed — is N16R8 PSRAM enabled?");
  }

  // ES8311 must init the shared I2C bus before the camera reuses port 0.
  codec.begin(ES8311_I2C_SDA, ES8311_I2C_SCL, ES8311_I2C_ADDR, ES8311_I2C_FREQ, kSampleRate);
  setupI2S();
  setupCamera();
  connectWifi();
  Serial.println("[boot] ready — hold BOOT to talk");
}

void loop() {
  if (digitalRead(PIN_BUTTON) == LOW) {
    delay(30);  // debounce
    if (digitalRead(PIN_BUTTON) == LOW) handleTurn();
    while (digitalRead(PIN_BUTTON) == LOW) delay(10);  // wait for release
  }
  delay(10);
}
