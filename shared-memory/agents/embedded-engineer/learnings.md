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

## 2026-06-30 · Brain180 튜터 API
- 튜터 = Express POST /api/tutor/chat. 인증=Lucia 쿠키 b180_session(승인 student). 디바이스 API-key 없음.
- 본문 {session_id, lesson_id, message, canvas_image_base64?}. 선행: /api/auth/login + /api/practice/sessions.
- canvas_image_base64(PNG)로 비전 입력 가능 → 카메라 프레임을 여기에 넣으면 튜터가 봄(별도 비전모델 불필요).
- LLM=Kimi(기본)/Anthropic, 비전=OpenAI gpt-4.1-mini/Anthropic. 전부 클라우드. STT/TTS/WS 미제공.

## 2026-07-01 — waveshare 실보드: I2C 버스 0 device = 보드 정체 미확정 신호
- 실기 플래시/부팅 로그: I2C 스캔 5개 후보핀(8/7, 47/48, 7/8, 8/9, 9/8) 전부 `0 device`. AXP2101/ES8311/카메라(0x105) 모두 no-ACK.
- `idle=1/1` → 풀업·배선 정상, 버스 안 죽음. 그런데 아무도 ACK 안 함 → "그 핀에 칩 없음".
- 화면 깜깜(`[lcd] init OK` 떠도)의 원인도 동일: 백라이트·오디오·카메라가 AXP2101 전원 뒤에 매달림. I2C 죽으면 전원 순서 못 넣어 통째로 죽음.
- **적신호 2개**: (1) esptool `8MB Flash` vs 문서 N16R8(16MB) 불일치. (2) m3-board-bringup.md §1 "보드 실크/AXS15231B 육안확인" 미완료.
- 결론: 블라인드 핀 추측(probe matrix) 중단. 기영님께 보드 앞·뒷면 사진(실크/화면칩)+구매링크 요청. 진짜 3.5B면 수제 PlatformIO 대신 공식 xiaozhi ESP-IDF 펌웨어가 정석(문서 TL;DR과도 일치).

## 2026-07-01 (2) — 보드 확정: Spotpear ESP32-S3-MAX35 (GC0308 · Deepseek)
- 기영님이 보드 URL 제공: spotpear.cn .../ESP32-S3-3.5-inch-LCD-...-GC0308-Deepseek.html
- 스펙: 카메라 **GC0308**(OV5640 아님), 코덱 **ES8311**(일치), 충전 **MP2636GR**(AXP2101 아님!), 앰프 NS4150B. → main_waveshare.cpp 가정(OV5640/OV2640 + AXP2101 + AXS15231B SDA8/SCL7)과 불일치.
- 지난 로그 실패 1:1 대응: axp2101 FAILED=칩없음, cam 0x105=GC0308 vs OV, I2C 0-device=핀불일치.
- Spotpear 형제 보드 2종(OV5640: AXS15231B-QSPI / ST7796)과 별개 — 기영님 건 GC0308 변형.
- **결론/방향**: 수제 PlatformIO 핀추측 중단. 이 보드는 공식 xiaozhi-esp32(github.com/78/xiaozhi-esp32) 네이티브. 1단계=공식 xiaozhi로 보드 살리기, 2단계=Brain180 /api/see 비전 연동은 별도(인프라 조율, xiaozhi 기본은 WS to xiaozhi-server). m3-board-bringup.md TL;DR과 일치.
- 대기중: 기영님이 실크 모델명 확인 + Spotpear 자료(원리도/데모펌웨어) 제공 예정.

## 2026-07-01 (3) — CoreS3 실기 첫 플래시: PSRAM 모드 + 카메라 I2C 충돌 (v2로 수정)
- 증상: Guru Meditation StoreProhibited(EXCVADDR 0x0) 무한 재부팅 + camera 0xffffffff.
- 원인1 (PSRAM): platformio [common] `board_build.arduino.memory_type=qio_opi`(옥타). CoreS3는 **쿼드(QSPI) PSRAM** → "wrong PSRAM line mode" → ps_malloc null → 녹음버퍼 null 쓰기 크래시. m5stack-cores3.json엔 memory_type 미설정. → cores3 env에 `qio_qspi` 오버라이드 + null 가드.
- 원인2 (카메라): CoreS3 내부 I2C(포트1 SDA12/SCL11)를 PMIC/코덱/터치/카메라 공유. M5.begin()이 선점 → esp_camera가 포트1 재install 시도 "i2c driver install error". sccb_i2c_port=1 재사용 안 됨(esp32-camera 2.0.4는 무조건 i2c_driver_install). → 패턴: `M5.In_I2C.release()` → esp_camera_init → `i2c_driver_delete(1)` → `M5.In_I2C.begin()`(캡처는 DVP라 SCCB 불요). 실패 시 CAM_SCCB_I2C_PORT=0 시도.
- 카메라 실패해도 음성 검증되게 postSee 이미지 옵셔널화(오디오-only 전송).
- 커밋 12d4bee push (branch 9aa2a597). 대기: 기영님 재플래시 새 로그.

## 마이크 무음(peak=0) 판별법 — CoreS3 ES7210
- peak가 정확히 0 = 코덱이 무음 전달(dead path), 조용한 방(작은 노이즈플로어~수십)과 다름. SW게인 무의미(0×n=0).
- 서버 `/api/see 422` = STT "No speech detected"(backend/app.py:94). 이미지/포맷 문제 아님 — 무음 오디오가 원인.
- I2C `i2c_driver_delete/SCCB` 에러 + `i2s_driver_uninstall port1 not installed`는 카메라 온디맨드 init + 첫 Speaker.end() 정상로그(무해).
- M5Unified 마이크: record()는 비동기 더블버퍼. per-chunk isRecording() busy-wait는 첫 버퍼 미충전 위험 -> 연속 피드 후 끝에서 1회 drain이 권장 패턴.
## 2026-07-03 - /api/see text vs image failure split
- For ALI-21, `/health` and text-only `/api/see` can be fast/healthy while image-attached `/api/see` fails. Do not diagnose "server slow" from gateway reachability alone.
- Current observed split: health ~66 ms, text-only `/api/see` ~5.2 s, image `/api/see` -> 502 with local vLLM 400.
- When camera is re-enabled in firmware, "worked just now" may flip because the request path changes from text/audio-only to multimodal local-vLLM vision.
## 2026-07-03 - Watchdog is required for CoreS3 camera/touch recovery
- For CoreS3 camera experiments, leaving `esp_task_wdt_deinit()` in production firmware hides real I2C/touch wedges as permanent freezes. Keep a watched loop task and feed only known-good long loops.
- For `/api/see`, a UI animation loop can keep the board alive while the HTTP task runs, but a hard upper bound is still needed. If the background request never returns, reboot deliberately rather than preserving dangling audio/jpeg pointers.
