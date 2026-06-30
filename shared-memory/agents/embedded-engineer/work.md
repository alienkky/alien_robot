# embedded-engineer — work.md

## 2026-06-30 · ALI-21 (M3) 실보드 브링업 + 카메라/비전 설계
- 보드 식별: AliExpress 1005011936539776 = Waveshare ESP32-S3-Touch-LCD-3.5B (QSPI/AXS15231B/N16R8).
- xiaozhi-esp32 `main/boards/waveshare/esp32-s3-touch-lcd-3.5b` 프로파일 존재 확인 (소스).
  - config.json: target esp32s3, build `esp32-s3-touch-lcd-3.5b`, CAMERA_OV5640=y + OV2640=y.
  - config.h: 오디오 ES8311(I2S MCLK44/WS15/BCLK13/DIN14/DOUT16, I2C SDA8/SCL7), 디스플레이
    AXS15231B QSPI(CS12/CLK5/D0-3=1,2,3,4/BL6, 480x320), 카메라 DVP(XCLK38/PCLK41/VSYNC17/
    HREF18/D0-7=45,47,48,46,42,40,39,21), BOOT0.
- 산출물:
  - `docs/m3-board-bringup.md` (신규) — 결론/핀맵/비전경로/빌드·플래시/시리얼 체크리스트.
  - `docs/hardware.md` — M3 Board 절 추가 (DIY 보드와 구분 경고).
  - `docs/HANDOFF.md` — M3 체크박스 갱신 (타깃 변경).
  - `shared-memory/messages/...-to-infra-engineer-ali20-vision-config.md`.
- 미완(장비 의존): 실제 `idf.py build` 컴파일, USB 플래싱, 시리얼 검증 → 기영님.
