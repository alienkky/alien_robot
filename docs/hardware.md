# Hardware Notes

> **두 개의 서로 다른 보드가 이 문서에 있다 — 헷갈리지 말 것.**
> - **(A) DIY 음성 단말 (M0):** ESP32-S3 + INMP441 + MAX98357A. 아래 "Recommended
>   Parts ~ Bring-Up Order" 전부 이 보드용. PlatformIO `firmware/` 가 이 보드를 빌드.
> - **(B) M3 실보드:** Waveshare **ESP32-S3-Touch-LCD-3.5B** 올인원 (카메라/디스플레이/
>   ES8311). **xiaozhi-esp32(ESP-IDF)** 로 빌드하며 핀맵은 펌웨어에 내장 — 문서 맨 끝
>   "M3 Board" 절 참조. `firmware/` PlatformIO 프로젝트와 무관. 상세는 `m3-board-bringup.md`.

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

---

## M3 Board — Waveshare ESP32-S3-Touch-LCD-3.5B (xiaozhi-esp32)

별도 보드. 위 DIY 배선과 무관하며 납땜 불필요(올인원). 핀맵은 xiaozhi 펌웨어
프로파일 `esp32-s3-touch-lcd-3.5b/config.h` 에 내장돼 있고, 아래는 **대조·디버깅용
복사본**이다. 빌드/플래시/검증 전체 절차는 `docs/m3-board-bringup.md` 참조.

- SoC: ESP32-S3R8 (N16R8, 8MB PSRAM / 16MB Flash)
- 디스플레이: AXS15231B QSPI 480×320 (정전식 터치)
- 카메라: OV5640 / OV2640 (DVP, xiaozhi 빌드에 컨피그 포함)
- 오디오 코덱: ES8311 (I2C), I2S 24kHz

| 그룹 | 신호 | GPIO |   | 그룹 | 신호 | GPIO |
|---|---|---:|---|---|---|---:|
| 오디오 | I2S MCLK | 44 | | 카메라 | XCLK | 38 |
| 오디오 | I2S WS | 15 | | 카메라 | PCLK | 41 |
| 오디오 | I2S BCLK | 13 | | 카메라 | VSYNC | 17 |
| 오디오 | I2S DIN | 14 | | 카메라 | HREF | 18 |
| 오디오 | I2S DOUT | 16 | | 카메라 | D0~D7 | 45,47,48,46,42,40,39,21 |
| 코덱 I2C | SDA | 8 | | 디스플레이 | CS | 12 |
| 코덱 I2C | SCL | 7 | | 디스플레이 | CLK | 5 |
| 버튼 | BOOT | 0 | | 디스플레이 | DATA0~3 | 1,2,3,4 |
| 디스플레이 | Backlight | 6 | | | | |

> 카메라 SCCB(SIOD/SIOC)는 코덱과 같은 I2C 버스(SDA=8, SCL=7) 공유 → config.h 상 NC.
> ⚠️ 디스플레이 IC가 ST7796(비-B 변형)이면 `esp32-s3-touch-lcd-3.5` 프로파일을 써야 함.
> 실물 실크로 AXS15231B 재확인 필요.
