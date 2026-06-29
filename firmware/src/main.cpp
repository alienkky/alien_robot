// Phase 0 (HTTP): compiled by default. Phase 1 (WebSocket): add -DUSE_WEBSOCKET=1 to build_flags.
#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <driver/i2s.h>

#ifdef USE_WEBSOCKET
#include <WebSocketsClient.h>
#else
#include <HTTPClient.h>
#endif

#include "config.h"

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

// ---------------------------------------------------------------------------
// Shared helpers
// ---------------------------------------------------------------------------

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
  // INMP441-style mics use the high 24 bits of a 32-bit I2S frame.
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

// ---------------------------------------------------------------------------
// Phase 0: HTTP path (original M0 implementation)
// ---------------------------------------------------------------------------
#ifndef USE_WEBSOCKET

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

#else  // USE_WEBSOCKET

// ---------------------------------------------------------------------------
// Phase 1: WebSocket path (xiaozhi-esp32-server at ws://<IP>:8003/xiaozhi/v1/)
// ---------------------------------------------------------------------------

// States for the WebSocket turn state machine.
enum class WsState {
  kDisconnected,
  kConnecting,
  kHandshaking,   // sent hello, waiting for server hello
  kIdle,          // ready for a new turn
  kSendingAudio,  // sending PCM binary frame(s)
  kWaitingReply,  // waiting for transcript/TTS from server
  kReceivingAudio // streaming TTS audio from server to speaker
};

WebSocketsClient wsClient;
WsState wsState = WsState::kDisconnected;
// Server-side TTS sample rate comes from the hello handshake.
uint32_t ttsSampleRate = 16000;

// Play a raw PCM chunk (little-endian 16-bit signed) directly to I2S speaker.
void playSpeakerChunk(const uint8_t *data, size_t len) {
  size_t written = 0;
  // i2s_write blocks until all bytes are queued in DMA.
  i2s_write(kSpeakerPort, data, len, &written, portMAX_DELAY);
}

// Send the xiaozhi hello handshake. The server echoes back with its audio params.
void sendWsHello() {
  // xiaozhi protocol v3 hello: request raw PCM so no client-side codec needed.
  const char *hello = "{\"type\":\"hello\",\"version\":3,\"transport\":\"websocket\","
                      "\"audio_params\":{\"format\":\"pcm\",\"sample_rate\":16000,"
                      "\"channels\":1,\"frame_duration\":0}}";
  wsClient.sendTXT(hello);
  Serial.println("WS: sent hello");
  wsState = WsState::kHandshaking;
}

// Called by the WebSocketsClient library for each incoming event.
void onWsEvent(WStype_t type, uint8_t *payload, size_t length) {
  switch (type) {
    case WStype_DISCONNECTED:
      Serial.println("WS: disconnected");
      wsState = WsState::kDisconnected;
      break;

    case WStype_CONNECTED:
      Serial.printf("WS: connected to %s\n", (char *)payload);
      sendWsHello();
      break;

    case WStype_TEXT: {
      // JSON control frame from server.
      JsonDocument doc;
      if (deserializeJson(doc, payload, length) != DeserializationError::Ok) {
        Serial.printf("WS: bad JSON: %.*s\n", (int)length, payload);
        break;
      }
      const char *msgType = doc["type"] | "";

      if (strcmp(msgType, "hello") == 0) {
        // Server hello: extract TTS sample rate if provided.
        uint32_t rate = doc["audio_params"]["sample_rate"] | 16000;
        ttsSampleRate = rate;
        if (ttsSampleRate != kSampleRate) {
          // Speaker was set up at kSampleRate; reconfigure DMA sample rate.
          i2s_set_clk(kSpeakerPort, ttsSampleRate, I2S_BITS_PER_SAMPLE_16BIT,
                      I2S_CHANNEL_MONO);
        }
        Serial.printf("WS: server hello ok (TTS rate=%u)\n", ttsSampleRate);
        wsState = WsState::kIdle;
      } else if (strcmp(msgType, "stt") == 0) {
        const char *text = doc["text"] | "";
        Serial.printf("You: %s\n", text);
      } else if (strcmp(msgType, "llm") == 0) {
        const char *text = doc["text"] | "";
        Serial.printf("AI: %s\n", text);
        wsState = WsState::kReceivingAudio;
        setStatus(true);
      } else if (strcmp(msgType, "tts") == 0) {
        // Some server builds send a final "tts" done marker.
        if (doc["state"] == "stop") {
          Serial.println("WS: TTS done");
          setStatus(false);
          wsState = WsState::kIdle;
        }
      } else if (strcmp(msgType, "error") == 0) {
        Serial.printf("WS error: %s\n", doc["message"] | "unknown");
        setStatus(false);
        wsState = WsState::kIdle;
      }
      break;
    }

    case WStype_BIN:
      // Binary frames are TTS PCM audio from the server.
      if (wsState == WsState::kReceivingAudio || wsState == WsState::kWaitingReply) {
        wsState = WsState::kReceivingAudio;
        playSpeakerChunk(payload, length);
      }
      break;

    case WStype_PING:
      // Library auto-sends pong; nothing to do.
      break;

    case WStype_ERROR:
      Serial.println("WS: socket error");
      wsState = WsState::kDisconnected;
      break;

    default:
      break;
  }
}

// Connect (or reconnect) to the xiaozhi WebSocket server.
void wsConnect() {
  Serial.printf("WS: connecting to %s:%d%s\n", XIAOZHI_SERVER_HOST, XIAOZHI_SERVER_PORT,
                XIAOZHI_SERVER_PATH);
  wsClient.begin(XIAOZHI_SERVER_HOST, XIAOZHI_SERVER_PORT, XIAOZHI_SERVER_PATH);
  wsClient.onEvent(onWsEvent);
  // 5-second reconnect delay; library will retry automatically.
  wsClient.setReconnectInterval(5000);
  wsState = WsState::kConnecting;
}

void handleTurn() {
  if (pcmBytes < kSampleRate) {
    Serial.println("Recording too short; ignored.");
    return;
  }
  if (wsState != WsState::kIdle) {
    Serial.printf("WS: not ready (state=%d)\n", static_cast<int>(wsState));
    return;
  }

  // Send the entire PCM buffer as a single binary WebSocket frame.
  setStatus(true);
  wsClient.sendBIN(pcmBuffer, pcmBytes);
  Serial.printf("WS: sent %u bytes PCM\n", static_cast<unsigned>(pcmBytes));
  wsState = WsState::kWaitingReply;
  setStatus(false);
}

#endif  // USE_WEBSOCKET
}  // namespace

void setup() {
  Serial.begin(115200);
  delay(1000);
  pinMode(PIN_BUTTON, INPUT_PULLUP);
  pinMode(PIN_STATUS_LED, OUTPUT);
  setStatus(false);

  pcmBuffer = static_cast<uint8_t *>(ps_malloc(kMaxPcmBytes));
  if (pcmBuffer == nullptr) {
    Serial.println("PSRAM alloc failed. Use ESP32-S3 board with PSRAM and OPI memory type.");
    while (true) {
      delay(1000);
    }
  }

  connectWifi();
  setupMic();
  setupSpeaker();

#ifdef USE_WEBSOCKET
  wsConnect();
  Serial.println("Ready (WebSocket mode). Hold button to talk.");
#else
  Serial.println("Ready (HTTP mode). Hold button to talk.");
#endif
}

void loop() {
#ifdef USE_WEBSOCKET
  // WebSocket event loop must run every iteration.
  wsClient.loop();
#endif

  if (digitalRead(PIN_BUTTON) == LOW) {
    delay(30);
    if (digitalRead(PIN_BUTTON) == LOW) {
#ifdef USE_WEBSOCKET
      // Block WS events during recording — mic takes the CPU.
      recordUntilRelease();
      handleTurn();
      // Drain WS events while waiting for reply.
      unsigned long deadline = millis() + 30000UL;  // 30 s timeout
      while (wsState == WsState::kWaitingReply || wsState == WsState::kReceivingAudio) {
        wsClient.loop();
        if (millis() > deadline) {
          Serial.println("WS: reply timeout");
          setStatus(false);
          wsState = WsState::kIdle;
          break;
        }
        delay(1);
      }
#else
      recordUntilRelease();
      handleTurn();
#endif
      Serial.println("Ready. Hold button to talk.");
    }
  }
  delay(10);
}
