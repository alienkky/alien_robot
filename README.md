# ESP32 AI Dialogue System

ALI-112 reference prototype for a self-hosted AI conversation device built around an ESP32-class board.

The ESP32 is used as a low-cost voice terminal. Speech recognition, language model inference, and speech synthesis run on a local PC/server, so the conversation system can stay private and work without cloud AI services.

## Architecture

```text
I2S mic -> ESP32-S3 -> HTTP PCM upload -> local AI server
                                      -> faster-whisper STT
                                      -> Ollama chat model
                                      -> optional Piper TTS WAV
                                      -> ESP32-S3 I2S speaker
```

Recommended first hardware target:

- ESP32-S3 board with PSRAM
- INMP441 I2S microphone
- MAX98357A I2S amplifier
- 4 ohm / 3 W speaker
- Momentary push button for push-to-talk
- 5 V USB power supply

## Folder Layout

- `firmware/` - PlatformIO Arduino firmware for the ESP32-S3 voice terminal
- `backend/` - FastAPI local AI server
- `docs/hardware.md` - wiring and board notes
- `docs/protocol.md` - HTTP/audio contract between firmware and backend

## Quick Start

1. Install Ollama and pull a compact local model:

   ```powershell
   ollama pull llama3.2:3b
   ```

2. Start the local backend:

   ```powershell
   cd backend
   python -m venv .venv
   .\.venv\Scripts\Activate.ps1
   pip install -r requirements.txt
   Copy-Item config.example.env .env
   uvicorn app:app --host 0.0.0.0 --port 8787
   ```

3. Configure the ESP32 firmware:

   ```powershell
   cd firmware
   Copy-Item include\config.h.example include\config.h
   ```

   Edit `include/config.h` with Wi-Fi credentials and the LAN IP of the backend PC.

4. Flash with PlatformIO:

   ```powershell
   pio run -t upload
   pio device monitor
   ```

5. Hold the push-to-talk button, speak, release it, and wait for the reply.

## First Milestone

The first stable milestone is text reply over Serial. Audio reply is enabled once a Piper voice model is installed and `PIPER_BIN` / `PIPER_MODEL` are configured in `backend/.env`.

## Practical Limits

- ESP32 records and plays audio, but does not run the LLM.
- Use an ESP32-S3 with PSRAM. A plain ESP32 can be adapted, but available RAM will force much shorter recordings.
- Start with 4 second utterances at 16 kHz mono PCM. Extend later with streaming/WebSocket once the simple request-response loop is stable.
