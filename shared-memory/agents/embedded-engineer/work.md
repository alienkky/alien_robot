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

## 2026-06-30 (3차) · ALI-21 Brain180 연동 구현
- 기영님: 직접 판단해 구현, 우선=연동, Railway 우선, vLLM 추후, STT/TTS 효율.
- Brain180(브랜치 feat/robot-bridge-ali21): POST /api/robot/chat 신설(베어러 토큰, 무상태,
  로봇 페르소나, 텍스트+비전). GET /api/robot/health. RobotChatBody/ROBOT_DEVICE_TOKEN/8mb바디.
  smoke:robot. tsc+eslint 통과. 추가 전용(토큰 없으면 503).
- 게이트웨이(backend/app.py): LLM_PROVIDER=brain180 + ask_brain180(history,image). /api/turn 유지
  (M0 펌웨어 무수정). /api/see(카메라 멀티파트)·/api/reset. faster-whisper+Piper 재사용.
  스텁 종단 통합테스트 PASS. 문서: m3-brain180-integration.md §4.5, protocol.md.
- 남음: 실보드 카메라 펌웨어 포팅, brain180 AI_PROVIDER→vLLM(프라이빗), 4090 라이브 실호출.

## 2026-06-30 (4차) · ALI-21 Waveshare 펌웨어 (실물의존 낮은 다음 작업)
- 기영님: 실물의존 빼고 실물의존성 낮은 다음 작업 → 펌웨어 작성+pio 빌드검증(플래싱만 실물).
- 우리 PlatformIO 독사 펌웨어(xiaozhi ESP-IDF 대신): env waveshare-s3-touch-lcd-35b.
  main_waveshare.cpp(푸시투토크→ES8311녹음+OV5640 JPEG→멀티파트 /api/see→재생),
  es8311.{h,cpp} 코덱 드라이버, config_waveshare.h.example 핀맵.
- platformio.ini: M0 env는 build_src_filter로 분리 보존. 두 env 모두 pio run SUCCESS
  (RAM15.7%/Flash30.5%). esp_camera/I2S/멀티파트 링크 OK.
- 미검증(실보드): ES8311 레지스터 init·I2S 클럭(드라이버 UNVERIFIED 배너). 카메라 핀은 보드 프로파일 일치.

## 2026-07-03 · ALI-24 화면 브리지 Stage 1 (하트비트+프레임 push)
- 게이트웨이(backend/app.py): 백그라운드 하트비트 루프 GET brain180 /api/robot/health
  ROBOT_HEARTBEAT_INTERVAL(기본10s) → 🟢 online 유지. LLM_PROVIDER=brain180 & 토큰 있을 때만.
- push_frame_to_brain180(): POST /api/robot/frame {image_base64(prefix무),media_type}, <5MB.
- 신규 POST /api/frame(멀티파트 image 또는 raw body) → brain180 프레임 전달.
- /api/see 는 캡처 프레임을 best-effort 로 최신화면에도 publish(턴 실패 안 시킴).
- 펌웨어 main_waveshare.cpp: idle 타이머 FRAME_PUSH_INTERVAL_MS(기본2000)마다 /api/frame push.
- 빌드: waveshare-s3-touch-lcd-35b + http 둘 다 pio run SUCCESS. py_compile OK.
- 커밋 2b1319f, 브랜치 agent/embedded-engineer/aee95520 push. 실보드 검증은 하드웨어 확보 후.
