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

## 2026-07-01 — CoreS3 route-A 펌웨어 실기 검증 완료 (단말측)
- v2(commit 12d4bee) 실기 로그: PSRAM OK(크래시 사라짐), `[rec] 47104 samples`(마이크 OK), `[ui] capturing...`(카메라 GC0308 초기화+촬영 OK, 코드상 cameraOk일 때만 출력), `WiFi connected 10.237.240.3`. → 단말 하드웨어 계층(마이크/카메라/화면/WiFi/멀티파트 전송) 전부 동작.
- 남은 블로커: `/api/see -> -1 connection refused (timeout 5000ms)`. 순수 서버 미도달(펌웨어 무관). 4090 게이트웨이 미기동 or AI_SERVER_BASE_URL IP:포트 불일치 or 다른 서브넷/방화벽.
- 대기: 기영님이 (1) AI_SERVER_BASE_URL 값 (2) 4090 게이트웨이 기동+IP (3) 타기기 브라우저 도달 테스트. 서버 확인돼도 refused면 infra-engineer와 포트/바인딩 조율.

## 2026-07-01 — 4090 게이트웨이: 내가 provision + 인프라 Funnel이 이미 라이브
- 확인: 이 런타임이 4090 자체(hostname ALIEN_4090, RTX4090). E:\alien_robot 직접 접근 가능. [[runtime-is-the-4090]]
- E:\alien_robot을 M3 브랜치(57014a6)로 업데이트 + backend venv/deps 설치 + .env(토큰 blank) 생성 + uvicorn `/health`={"status":"ok"} 실측. 편의 스크립트 scripts/run_gateway.ps1.
- 그러나 인프라(남기준)가 이미 **상시 공개 게이트웨이** 완성: Tailscale Funnel `https://alien-4090.taile7f882.ts.net:8443` → :8787. 실측 /health OK, 인프라가 /api/see 왕복 검증. → 내 cloudflared 안내 철회, funnel URL로 정정 코멘트.
- 펌웨어 v4(d1951ac): X-Device-Token 헤더 전송(embedded pair) + config에 funnel URL 문서화. 빌드 통과.
- 대기: 기영님 git pull + AI_SERVER_BASE_URL=funnel + 재플래시. 인프라: 인바운드 인증 enforce + 누출 토큰 로테이트.

## mistakes
- 2026-07-01: 4090의 E: 드라이브를 "못 본다"고 두 번 잘못 답함. 실제론 이 런타임이 4090 자체. → 앞으론 hostname/nvidia-smi로 먼저 확인 후 판단. [[runtime-is-the-4090]]

##  — ALI-21 마이크 peak=0 진단·수정 (v16)
- 증상: 실기 시리얼 `[mic] peak=0 gain=1x`, `/api/see -> 422`, "잘 안 들렸어요" 후 대기 복귀.
- 근본원인: 마이크가 완전 무음(모든 64000샘플=0) 캡처. 무음 PCM -> 서버 STT(backend/app.py:93-94) "No speech detected" 422 -> 펌웨어가 422->"잘 안 들렸어요" 매핑 후 idle 복귀. 즉 422 경로·"반응 없음"은 설계대로 정상, 진짜 버그는 마이크 무음.
- 조치: recordAudio() per-chunk isRecording() 대기 -> 연속 DMA 피드+마지막 1회 drain. 진단로그 `[mic] enabled` + `raw=[...]` 추가. pio run -e cores3 SUCCESS. 커밋 9a84a1e, 브랜치 agent/embedded-engineer/9aa2a597 push.
- 미해결: 실기 미검증. 다음 flash 로그의 enabled/peak/raw로 코덱-dead(하드웨어) vs 녹음루프-버그 판별.

##  — ALI-21 터치 死 원인=카메라 I2C, 카메라 OFF 기본화 (v17)
- 증상: 422 "잘 안 들렸어요" 화면에서 화면 터치 무반응, 재대화 불가.
- 원인(가설): GC0308 SCCB가 CoreS3 단일 내부 I2C(port1)를 FT6336 터치+오디오 코덱과 공유. handleTurn 매 턴 esp_camera_init/deinit가 버스 재구성 -> deinit의 i2c_driver_delete가 M5 버스를 무너뜨려 첫 턴 후 터치 死. 로그의 `i2c_driver_delete(411) install error`가 그 충돌.
- 조치: CAM_ENABLE 플래그 신설(기본0=오디오전용). 카메라 deinit 직후 M5.In_I2C.begin()으로 공유버스 회수. pio run -e cores3 SUCCESS. 커밋 f817e2b push.
- 다음 판별: 카메라 OFF 상태로 flash -> 422 후 터치 재동작 되면 카메라 I2C가 원인 확정. 그 뒤 버스 handoff 고쳐 CAM_ENABLE=1 복귀.

##  — 음성루프 OK, 카메라 재활성(버스 재사용)+생각중 애니 (v18)
- 실기 로그: mic peak=8186/4906 (마이크 정상!), /api/see->200, 한국어 답변, 터치 재트리거 OK. v16(연속DMA)+v17(카메라off)로 음성루프 완성.
- 관찰: 카메라 off인데 서버가 비전 답변("짧은머리 남성...") -> 서버가 이전 프레임/캐시 이미지 들고 있음. 실시간 비전엔 카메라 프레임 전송 필요.
- v18 조치: (1) setupCamera 버스 재사용(release/i2c_driver_delete 제거, sccb_i2c_port로 M5 드라이버 공유) -> 버스 안 부숨=터치 안 죽음, 실패시 audio-only 폴백. CAM_ENABLE 기본1 복귀. (2) postSeeThinking: HTTP를 백그라운드 task로 돌리고 생각중 얼굴 애니(눈동자 좌우+깜빡). drawFace에 pupilDx/Dy override 추가. task 실패시 blocking 폴백.
- pio run -e cores3 SUCCESS. 커밋 e42a3a6.
- 미검증: 카메라 버스 재사용이 실기 터치 유지하는지, 캡쳐 표시, 생각중 애니 실동작. 다음 로그로 확인: [cam] init OK(reuse) / 캡쳐화면 / 생각중 눈 움직임 / 터치 유지.

##  — v18 카메라ON 회귀: 마이크 死, v19 롤백 + lessons 문서
- 실기 v18 로그: [mic] peak=0 (마이크 死) + [turn] camera unavailable (카메라 init 실패). 최악: 둘 다 안 됨.
- 판정: 카메라 init이 공유 I2C(port1) 건드리면 ES7210 마이크 무음. 부팅 프로브/실패만으로도 발생. 마이크는 카메라 OFF일 때만 peak>0. 구방식(release/delete)=터치死, 재사용(v18)=마이크死. 동시 사용 현 설계로 불가.
- v19: CAM_ENABLE 기본0 롤백(음성루프 복구), 생각중 애니는 유지(버스 무관). docs/lessons-cores3.md 신규(마이크<->카메라 I2C 충돌·기준선·규칙). pio SUCCESS. 커밋 a5b27c3.
- 규칙 확립: 한 빌드에 한 변경. 카메라는 플래그 뒤 기본OFF. 공유I2C 변경은 실기로그 전까지 가설. peak로 마이크 진단.
- 카메라 부활 후보(미검증): 부팅프로브 제거+record 이후 on-demand init, deinit후 M5.Mic재init로 코덱복구, 안되면 동시사용 포기.

##  — v21 마이크-안전 온디맨드 카메라 재시도
- v20 로그: idle텍스트 제거OK, mic peak=2841 OK, /api/see 200 OK. 단 서버 답변="실시간 못본다"(이제 캐시 이미지도 없음). 유저: 카메라 해결 요구.
- v18 회귀 원인 재확정: (1)부팅 카메라 프로브가 세션 전체 마이크 오염 (2)reuse init은 GC0308 탐지 실패.
- v21 설계: 부팅 프로브 제거. 카메라는 handleTurn에서 recordAudio 이후에만 on-demand init. detect방식 복귀(자체 SCCB 드라이버 설치=센서 탐지 가능), orient설정후 i2c_driver_delete+M5.In_I2C.begin()으로 버스 반환. 마이크는 항상 깨끗한 버스에서 먼저 녹음 -> 카메라 실패해도 그 턴 이미지만 손해. [cam] captured WxH->jpeg N 로그 추가.
- CAM_ENABLE=1 복귀, CAM_ENABLE 0 즉시폴백 유지. pio SUCCESS. 커밋 2616721.
- 판별 로그(다음): [cam] GC0308 init OK / captured / 그 다음 턴 [mic] peak>0 유지 여부 / 터치 유지. 하나라도 깨지면 동시사용 불가 -> 모드분리 설계로.

##  — v21 카메라 공존 성공, 진짜 병목=서버 지연 (v22 타임아웃 상향)
- v21 실기: mic peak=2620/71 (양 턴 생존), [cam] GC0308 init OK ×2, captured 320x240 jpeg 6880/7945 bytes. **카메라+마이크 공존 성공** (v18 회귀 해결). 순서(record먼저→camera나중, 부팅프로브 제거)가 정답이었음.
- 진짜 문제: /api/see -> -11 (62266ms) = 비전 요청 62초 걸려 60초 read timeout 초과(2초 차). 서버가 죽은게 아니라 느린 것. 오디오전용도 39초였음. => 서버 지연이 UX 병목, 인프라 건.
- v22: postSee 타임아웃 60s->120s(느린 비전 완료되게), 생각중 얼굴에 경과초 카운터(멈춘것처럼 안보이게), -11 전용 메시지("서버가 느려요"). pio SUCCESS. 커밋 ccd8c4f.
- i2c_driver_delete/gdma_disconnect 에러들은 온디맨드 카메라의 정상 노이즈(캡처·마이크 다 동작). 무해.
- 남은건 서버 39-62초 추론 지연 = infra. 언어혼입(중국어)도 infra 메시지 전달함.

##  — 먹통 원인=카메라 deinit이 I2C wedge→터치read hang. 카메라 OFF (v24)
- 유저: "잘 안들렸어요" 뜨면 항상 멈춤/먹통. = 하드 프리즈(내 이전 "그냥 대기중" 설명 틀림).
- 원인: esp_camera_deinit()이 공유 I2C(port1) wedge -> 다음 loop M5.update() FT6336 터치 I2C read가 블록 -> 루프 정지. 워치독 꺼서(esp_task_wdt_deinit) 리부트도 안됨 -> 영구 먹통. v21~23 캡처성공+마이크생존이 이걸 가림(터치/루프가 진짜 피해자).
- v24: CAM_ENABLE=0 복귀 -> 음성루프+터치+볼륨2x 안정. lessons 문서에 프리즈 메커니즘+재활성 전 증명해야할 deinit 버스복구 절차 기록. pio SUCCESS. 커밋 afba4d5.
- 교훈: "카메라 turn 성공"≠"안정". 캡처 로그 다음의 터치재동작+health지속까지 확인해야 진짜 안정. 워치독 끈 상태선 I2C hang=영구먹통.

##  — 카메라 프리즈 정면돌파: I2C bus recovery (v26, 카메라 ON)
- 유저: 카메라 끄지 말고 오류 안나게 만들라(핵심기능). 맞는 말 -> 프리즈 메커니즘 자체를 고침.
- 프리즈 원인: esp_camera_deinit()이 공유 I2C(port1) wedge -> 다음 터치 read가 루프 stall -> 워치독 없어서 영구먹통.
- v26: recoverSharedI2C() 신설(deinit 직후 호출). release+i2c_driver_delete -> 비트뱅 SCL 9펄스로 stuck slave 클럭아웃+STOP(교과서 bus recovery) -> M5.In_I2C.begin(). 터치/오디오가 깨끗한 버스 회수 -> 루프 hang 불가. 카메라는 여전히 record 이후 on-demand. CAM_ENABLE=1 복귀. pio SUCCESS. 커밋 82b33ca.
- 판별 로그: [cam] shared I2C recovered 다음 턴 터치 재동작+health 지속 = 성공. 그래도 프리즈면 다음카드=카메라 구간 워치독(Task WDT ~10s, 애니루프 feed)로 hang시 자동리부트.
- 비트뱅 핀 12(SDA)/11(SCL), OUTPUT_OPEN_DRAIN. PMIC/코덱 같은 버스지만 클럭아웃은 표준·안전.
## 2026-07-03 - ALI-21 server-slow investigation
- User reported "server is slow; things that worked just now keep failing" after v26 camera-on.
- Checked current branch: v26 kept `/api/see` read timeout at 120s; not a firmware timeout regression.
- Remote gateway health via Python/OpenSSL: 200 in ~66 ms. Text-only `/api/see` with dummy audio+text: 200 in ~5.2 s.
- Image-attached `/api/see` reproduced 502 quickly with detail: `local vLLM request failed: 400 Bad Request` at `http://127.0.0.1:8000/v1/chat/completions`.
- Conclusion: failure is isolated to the camera/image vision path after v26, not gateway reachability. Sent infra message `20260703-0515-embedded-engineer-to-infra-engineer-ali21-vision-vllm-400.md`.
## 2026-07-03 - ALI-21 v27 timeout-freeze recovery
- User clarified: when the screen says "server is slow", the robot becomes unresponsive after that.
- Implemented v27 in `firmware/src/main_cores3.cpp`: re-enable task watchdog, feed it in normal long loops, hard restart `/api/see` if the background request exceeds 135s, and run a final `recoverSharedI2C()` on empty response/bad JSON/success before returning idle.
- Boot log now says `route-A v27 (timeout recovery + watchdog)`. Expected if hang recurs: automatic reboot with `last reset reason = TASK_WDT`, not permanent freeze.
- Build not run in this runtime: no `pio` executable and no `platformio` Python module installed.
## 2026-07-03 - ALI-21 fast-thinking recommendation
- User asked how to reduce long "thinking" time. Current evidence says the fast path is text/audio-only (~5.2s observed), while image-attached vision goes through local vLLM and is the slow/failing path (502 local vLLM 400 or timeout).
- Recommended next firmware/server direction: v28 fast mode = default voice/text-only turns, attach camera only on explicit visual intent ("look", "photo", "what is this"), optionally reuse a cached last scene, compress/cap images, and add server per-stage timing/faster vision backend.
## 2026-07-03 - ALI-21 v27 field log review
- User provided v27 field logs. Camera capture succeeds (`GC0308 init OK`, JPEG 6-8 KB) and `recoverSharedI2C()` runs after camera and after empty response. Health logs continue after "server slow", then device re-enters listening, so the v27 freeze guard appears effective.
- Real failures in the log: first turn `/api/see -> -11 (56964ms)` = vision/server timeout around the 60s boundary despite firmware code intending 120s; second turn `/api/see -> 422 (3244ms)` with `peak=109 gain=10x` = audio too quiet / STT rejected, not a server outage.
## 2026-07-03 - ALI-21 v28 422/stale-touch recovery
- User clarified that touch freezes after the "could not hear you" message. Implemented v28: local low-speech gate skips camera/server when pre-gain mic peak <300, final I2C recovery after error message drawing, and bounded post-turn touch release wait with stale-pressed ignore + periodic I2C recovery.
- `git diff --check` passed. Build not run in this runtime: `pio` missing and `python -m platformio` unavailable.
