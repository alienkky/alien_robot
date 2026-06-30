# embedded-engineer — learnings.md

## 2026-06-30 · xiaozhi-esp32 보드/빌드
- xiaozhi-esp32(github.com/78/xiaozhi-esp32)는 **ESP-IDF** 프로젝트. 보드별 프로파일이
  `main/boards/<vendor>/<board>/` (config.json + config.h + *.cc). 빌드는 `idf.py set-target
  esp32s3` → menuconfig BOARD_TYPE 선택 → `idf.py build`. PlatformIO/`pio run` 아님.
- Waveshare 3.5 두 변형 구분 필수: `-3.5`=ST7796/SPI, `-3.5b`=AXS15231B/QSPI. 실크로 IC 확인.
- 비전은 MCP 기반: 빌트인 도구 `self.camera.take_photo`. 펌웨어가 JPEG 캡처→서버 비전 엔드포인트.
  서버(xiaozhi-esp32-server)의 VLLM 프로바이더를 vision 모델(우리 Qwen3.6-27B)로 지정하면 끝.
- 이 보드 오디오는 ES8311 코덱 — 우리 M0 DIY(INMP441/MAX98357A I2S 직결)와 하드웨어가 다름.
  → M0 `firmware/main.cpp` 는 이 보드에 안 쓰임. 별도 코드베이스(xiaozhi)로 빌드.
- 보드 contents/파일 목록·config 원문은 GitHub API(api.github.com/.../contents/...) + raw URL로 직접 확인 가능.
