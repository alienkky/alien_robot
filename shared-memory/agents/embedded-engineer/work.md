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

## 2026-06-30 (2차) · ALI-21 방향 전환 — Brain180 AI 튜터 연동
- 기영님 지시: xiaozhi 폐기(레퍼런스만), 독사 자체 구현 + Brain180 AI 튜터를 브레인으로.
- brain180(github.com/alienkky/brain180) 조사: React+Express+PG+Lucia 웹앱. 튜터 =
  POST /api/tutor/chat (쿠키 인증, lesson/session 종속, message=텍스트, canvas_image_base64=비전).
  LLM=Kimi/Anthropic, 비전=OpenAI/Anthropic(클라우드). STT/TTS/WS 없음.
- 산출: docs/m3-brain180-integration.md (독사 아키텍처 + brain180 계약 + 결정사항).
  ESP32=단순 단말, 4090 robot-gateway가 STT/TTS+brain180 호출 흡수.
- 미해결(기영님): vLLM 거취/프라이버시, 로봇 레슨 vs 디바이스 엔드포인트, 디바이스 인증,
  brain180 로컬 vs Railway, STT/TTS 자체구현.
