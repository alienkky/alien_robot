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

## 2026-07-01 — 방향 검토: Spotpear 폐기 → M5Stack(StackChan/CoreS3) 복귀 (기영님 제안)
- 기영님: "지금까지 배제하고 M5Stack으로 가면 전용 펌웨어 넣을 수 있냐?" → 답: 네, 개방형(USB 커스텀 플래시), 잠금 없음.
- 근거: HANDOFF.md M3 원래 타깃이 StackChan(M5Stack CoreS3, ~$99). Spotpear 올인원은 6/30 우회였음 → M5는 원안 복귀.
- 이점: 공개 핀맵 + M5Unified/M5GFX → 핀 추측 종료. 올인원(카메라 GC0308·듀얼마이크·스피커·팬틸트 서보·터치). 공식 xiaozhi CoreS3 프로파일 존재.
- 펌웨어 2경로: (A) 앱로직 유지 + M5Unified로 HW계층 재작성, (B) 공식 xiaozhi CoreS3.
- 버림=Spotpear HW계층(AXS15231B/AXP2101/OV). 유지=앱로직/Brain180 백엔드/WiFi·HTTP.
- 대기: 기영님 제품 확정(StackChan 키트 권장 vs 베어 CoreS3). 확정 후 정확 핀맵+플래시 절차 작성. 비전 백엔드(Brain180 /api/see vs xiaozhi WS)는 보드무관 별도 결정.

## 2026-07-01 — 하드웨어 확정: M5Stack CoreS3 (베어 개발키트)
- 기영님 확정: M5Stack CoreS3 ESP32-S3 IoT Dev Kit (StackChan 키트 아님 → 팬틸트 서보 미포함).
- CoreS3 스펙(확인): ESP32-S3 N16R8(16MB/8MB), 화면 2.0" ILI9342C+터치, 카메라 GC0308(0x21), 마이크 ES7210(0x40), 스피커앰프 AW88298(0x36), 전원 AXP2101(0x34)+IO확장 AW9523(0x58), BMI270, SD.
- 핵심: 모든 주변장치가 AXP2101+AW9523 뒤 → M5Unified `M5.begin()`이 전원/확장/화면/오디오 자동 초기화. Spotpear의 "핀추측+전원시퀀스 지옥"이 구조적으로 제거됨.
- 팬틸트 서보는 미포함(StackChan 키트만) → 음성+비전 MVP엔 무관, 서보는 나중 Port 추가.
- 펌웨어 경로: (A·권장) PlatformIO env `cores3` + M5Unified(화면/오디오) + esp_camera(GC0308) + 기존 turn 로직 재사용 → Brain180 /api/see 유지. (B) 공식 xiaozhi CoreS3 프로파일(스모크/ WS백엔드).
- 대기: 기영님 답 (1)보드 보유/주문 여부 (2)A 진행 여부. "A 시작"이면 main_cores3.cpp + cores3 env 작성 예정.
