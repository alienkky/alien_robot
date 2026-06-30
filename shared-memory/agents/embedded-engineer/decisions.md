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
