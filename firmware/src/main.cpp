#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <driver/i2s.h>

#include "config.h"

#ifndef I2S_COMM_FORMAT_STAND_I2S
#define I2S_COMM_FORMAT_STAND_I2S I2S_COMM_FORMAT_I2S
#endif

namespace {
constexpr int kSampleRate = 16000;
constexpr int kBitsPerSample = 16;
constexpr int kMaxRecordSeconds = 4;
constexpr size_t kMaxPcmBytes = kSampleRate * (kBitsPerSample / 8) * kMaxRecordSeconds;
constexpr size_t kI2sChunkSamples = 512;
constexpr i2s_port_t kMicPort = I2S_NUM_0;
constexpr i2s_port_t kSpeakerPort = I2S_NUM_1;

uint8_t *pcmBuffer = nullptr;
size_t pcmBytes = 0;

void setStatus(bool on) {
  digitalWrite(PIN_STATUS_LED, on ? HIGH : LOW);
}

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

void setupMic() {
  i2s_config_t config = {};
  config.mode = static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_RX);
  config.sample_rate = kSampleRate;
  config.bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT;
  config.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;
  config.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  config.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  config.dma_buf_count = 8;
  config.dma_buf_len = 256;
  config.use_apll = false;
  config.tx_desc_auto_clear = false;
  config.fixed_mclk = 0;

  i2s_pin_config_t pins = {};
  pins.bck_io_num = I2S_MIC_BCLK;
  pins.ws_io_num = I2S_MIC_WS;
  pins.data_out_num = I2S_PIN_NO_CHANGE;
  pins.data_in_num = I2S_MIC_DIN;

  ESP_ERROR_CHECK(i2s_driver_install(kMicPort, &config, 0, nullptr));
  ESP_ERROR_CHECK(i2s_set_pin(kMicPort, &pins));
  ESP_ERROR_CHECK(i2s_zero_dma_buffer(kMicPort));
}

void setupSpeaker() {
  i2s_config_t config = {};
  config.mode = static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_TX);
  config.sample_rate = kSampleRate;
  config.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  config.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;
  config.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  config.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  config.dma_buf_count = 8;
  config.dma_buf_len = 256;
  config.use_apll = false;
  config.tx_desc_auto_clear = true;
  config.fixed_mclk = 0;

  i2s_pin_config_t pins = {};
  pins.bck_io_num = I2S_SPK_BCLK;
  pins.ws_io_num = I2S_SPK_WS;
  pins.data_out_num = I2S_SPK_DOUT;
  pins.data_in_num = I2S_PIN_NO_CHANGE;

  ESP_ERROR_CHECK(i2s_driver_install(kSpeakerPort, &config, 0, nullptr));
  ESP_ERROR_CHECK(i2s_set_pin(kSpeakerPort, &pins));
  ESP_ERROR_CHECK(i2s_zero_dma_buffer(kSpeakerPort));
}

int16_t convertMicSample(int32_t sample) {
  // INMP441-style mics commonly use the high 24 bits of a 32-bit frame.
  int32_t shifted = sample >> 14;
  if (shifted > INT16_MAX) return INT16_MAX;
  if (shifted < INT16_MIN) return INT16_MIN;
  return static_cast<int16_t>(shifted);
}

void recordUntilRelease() {
  pcmBytes = 0;
  int32_t raw[kI2sChunkSamples];
  Serial.println("Recording...");
  setStatus(true);

  while (digitalRead(PIN_BUTTON) == LOW && pcmBytes + (kI2sChunkSamples * 2) <= kMaxPcmBytes) {
    size_t bytesRead = 0;
    esp_err_t result = i2s_read(kMicPort, raw, sizeof(raw), &bytesRead, portMAX_DELAY);
    if (result != ESP_OK || bytesRead == 0) {
      continue;
    }
    size_t samplesRead = bytesRead / sizeof(int32_t);
    for (size_t i = 0; i < samplesRead && pcmBytes + 2 <= kMaxPcmBytes; i++) {
      int16_t pcm = convertMicSample(raw[i]);
      pcmBuffer[pcmBytes++] = static_cast<uint8_t>(pcm & 0xff);
      pcmBuffer[pcmBytes++] = static_cast<uint8_t>((pcm >> 8) & 0xff);
    }
  }

  setStatus(false);
  Serial.printf("Recorded %u bytes\n", static_cast<unsigned>(pcmBytes));
}

String postTurn() {
  HTTPClient http;
  String url = String(AI_SERVER_BASE_URL) + "/api/turn";
  http.begin(url);
  http.addHeader("Content-Type", "application/octet-stream");
  int status = http.POST(pcmBuffer, pcmBytes);
  if (status <= 0) {
    Serial.printf("POST failed: %s\n", http.errorToString(status).c_str());
    http.end();
    return "";
  }
  String body = http.getString();
  Serial.printf("Server status: %d\n", status);
  http.end();
  if (status != 200) {
    Serial.println(body);
    return "";
  }
  return body;
}

void playWavFromUrl(const char *path) {
  if (path == nullptr || strlen(path) == 0) {
    return;
  }

  HTTPClient http;
  String url = String(AI_SERVER_BASE_URL) + path;
  http.begin(url);
  int status = http.GET();
  if (status != 200) {
    Serial.printf("Audio GET failed: %d\n", status);
    http.end();
    return;
  }

  WiFiClient *stream = http.getStreamPtr();
  int remaining = http.getSize();
  uint8_t header[44];
  if (stream->readBytes(header, sizeof(header)) != sizeof(header)) {
    Serial.println("Invalid WAV header");
    http.end();
    return;
  }
  if (remaining > 0) {
    remaining -= sizeof(header);
  }

  uint8_t chunk[1024];
  while (http.connected() && remaining != 0) {
    int available = stream->available();
    if (available <= 0) {
      delay(1);
      continue;
    }
    int wanted = min(available, static_cast<int>(sizeof(chunk)));
    if (remaining > 0) {
      wanted = min(wanted, remaining);
    }
    int readLen = stream->readBytes(chunk, wanted);
    if (readLen <= 0) {
      break;
    }
    if (remaining > 0) {
      remaining -= readLen;
    }
    size_t bytesWritten = 0;
    i2s_write(kSpeakerPort, chunk, readLen, &bytesWritten, portMAX_DELAY);
  }
  http.end();
}

void handleTurn() {
  if (pcmBytes < kSampleRate) {
    Serial.println("Recording too short; ignored.");
    return;
  }

  setStatus(true);
  String response = postTurn();
  setStatus(false);
  if (response.isEmpty()) {
    return;
  }

  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, response);
  if (error) {
    Serial.printf("JSON parse failed: %s\n", error.c_str());
    Serial.println(response);
    return;
  }

  const char *transcript = doc["transcript"] | "";
  const char *answer = doc["answer"] | "";
  const char *audioUrl = doc["audio_url"] | "";

  Serial.printf("You: %s\n", transcript);
  Serial.printf("AI: %s\n", answer);
  playWavFromUrl(audioUrl);
}
}  // namespace

void setup() {
  Serial.begin(115200);
  delay(1000);
  pinMode(PIN_BUTTON, INPUT_PULLUP);
  pinMode(PIN_STATUS_LED, OUTPUT);
  setStatus(false);

  pcmBuffer = static_cast<uint8_t *>(ps_malloc(kMaxPcmBytes));
  if (pcmBuffer == nullptr) {
    Serial.println("Failed to allocate PSRAM buffer. Use an ESP32-S3 board with PSRAM.");
    while (true) {
      delay(1000);
    }
  }

  connectWifi();
  setupMic();
  setupSpeaker();
  Serial.println("Ready. Hold button to talk.");
}

void loop() {
  if (digitalRead(PIN_BUTTON) == LOW) {
    delay(30);
    if (digitalRead(PIN_BUTTON) == LOW) {
      recordUntilRelease();
      handleTurn();
      Serial.println("Ready. Hold button to talk.");
    }
  }
  delay(10);
}
