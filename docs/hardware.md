# Hardware Notes

## Recommended Parts

- ESP32-S3 DevKit with PSRAM
- INMP441 I2S microphone module
- MAX98357A I2S amplifier module
- 4 ohm / 3 W speaker
- Momentary push button
- Breadboard jumper wires

## Default Pin Map

| Function | ESP32-S3 GPIO |
|---|---:|
| Push-to-talk button | 0 |
| Status LED | 2 |
| Mic BCLK/SCK | 4 |
| Mic WS/LRCLK | 5 |
| Mic SD/DOUT | 6 |
| Speaker BCLK | 15 |
| Speaker WS/LRC | 16 |
| Speaker DIN | 17 |

The button uses `INPUT_PULLUP`, so wire one side to GPIO 0 and the other side to GND.

## INMP441 Wiring

| INMP441 | ESP32-S3 |
|---|---|
| VDD | 3V3 |
| GND | GND |
| SCK | GPIO 4 |
| WS | GPIO 5 |
| SD | GPIO 6 |
| L/R | GND |

## MAX98357A Wiring

| MAX98357A | ESP32-S3 |
|---|---|
| VIN | 5V |
| GND | GND |
| BCLK | GPIO 15 |
| LRC | GPIO 16 |
| DIN | GPIO 17 |
| SD | VIN |

## Bring-Up Order

1. Flash firmware with only Serial connected.
2. Confirm Wi-Fi connects and `/health` is reachable from another machine.
3. Wire the microphone and verify the backend receives non-empty PCM.
4. Enable `MOCK_TRANSCRIPT` on the backend to test LLM replies without debugging STT.
5. Add Piper TTS and speaker output after text replies are stable.
