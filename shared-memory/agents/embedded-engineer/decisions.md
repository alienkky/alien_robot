# embedded-engineer — decisions.md

## 2026-06-30 · M3 펌웨어 전략 = xiaozhi 공식 펌웨어 채택 (1순위)
- 결정: 우리 main.cpp 핀포팅(2순위) 대신 **xiaozhi-esp32 `esp32-s3-touch-lcd-3.5b`
  공식 프로파일을 그대로 빌드/플래시.** 근거: 보드 프로파일이 이미 존재하고 카메라·
  디스플레이·코덱·터치 핀이 전부 정의됨 → 핀포팅·카메라 신규코드 불필요.
- 결정: 빌드 검증 = ESP-IDF `idf.py build`. 본 런타임엔 ESP-IDF 미설치 → **소스 레벨
  검증만 수행하고, 컴파일/플래시/시리얼은 장비 확보 후 기영님**으로 명확히 인계.
  ("빌드 통과 ≠ 실기 동작" 정체성 규칙 준수.)
- 권고(인프라 판단 대기): xiaozhi 공식 펌웨어가 WebSocket+MCP+비전 제공 →
  **ALI-20(커스텀 WebSocket) cancel/축소 후보.** shared-memory 메시지로 infra에 전달.

## 2026-06-30 (2차) · 브레인 = Brain180 AI 튜터 (xiaozhi 레퍼런스만)
- 결정(기영님): 로봇 브레인을 brain180의 AI 튜터로. xiaozhi 구성 폐기 → 레퍼런스만.
- 설계: ESP32는 음성/카메라 I/O만, 4090 robot-gateway가 STT/TTS + brain180 /api/tutor/chat
  (쿠키 인증·레슨세션·카메라 base64) 전담. 단말↔게이트웨이는 M0 turn 계약 재사용→WS.
- 보류(결정 대기): vLLM/Qwen3.6 거취(클라우드 LLM과 프라이버시 충돌), 레슨 종속 해소법, 인증 방식.

## 2026-06-30 (3차) · 펌웨어 = 우리 PlatformIO 코드 (xiaozhi ESP-IDF 폐기)
- 결정: M3 펌웨어를 xiaozhi ESP-IDF 빌드 대신 **우리 PlatformIO Arduino 코드**로 작성.
  근거: 독사 게이트웨이(/api/see)와 직접 통신, M0 코드/툴체인 재사용, pio run 빌드검증 가능.
- M0 env 보존(build_src_filter 분리). 실보드 플래싱·ES8311 오디오 튜닝만 실물 의존으로 남김.

## 2026-07-03 · ALI-24 프레임 소스 = 게이트웨이 push (pull 아님)
- 결정: ESP32가 idle시 주기적으로 /api/frame 에 프레임 push → 게이트웨이가 brain180 최신1장 슬롯 갱신.
  브라우저 버튼은 그 최신 프레임을 pull. 근거: 브라우저가 로봇 직접 접근 불가, brain180 브로커.
- 하트비트는 게이트웨이가 침(로봇 아닌 게이트웨이 liveness). 서버 판정창 30s → 10s 간격.
- Stage 2(온디맨드 캡처: 버튼 순간 촬영)는 명령 채널 필요 → 후속 하위이슈.
