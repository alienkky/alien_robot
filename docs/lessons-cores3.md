---
tags: [alien-robot, cores3, firmware, lessons, i2c, mic, camera]
updated: 2026-07-07
---

> **현재 상태(2026-07-07, fw v48):** 아래 0~4번은 "카메라 OFF가 기준선"이던 시절 기록이다.
> v43부터 전략이 바뀌었다 — **카메라 ON을 유지**하고, 카메라가 공유 I2C를 건드린 직후
> **터치·마이크·스피커 컨트롤러를 매 턴 강제 리셋**한다. 최신 상태·검증 결과는 **6번 섹션** 참고.

# CoreS3 펌웨어 — 반복하면 안 되는 실수 (lessons learned)

> 목적: 같은 실수를 다시 저지르지 않기 위한 단일 진실 문서.
> ESP32-S3 M5Stack **CoreS3** + M5Unified + esp32-camera 조합에서 실기로 확인된 것만 적는다.
> **원칙: "빌드 통과 ≠ 실기 동작". I2C·오디오 같은 공유 하드웨어 자원은 `pio run`으로 검증 불가.**

---

## 0. 지금 "동작하는 기준선" (여기서 벗어나면 의심하라)

| 항목 | 동작 조건 | 확인 로그 |
|---|---|---|
| 마이크(ES7210) | **카메라 OFF일 때만** 소리 잡음 | `[mic] peak>0` (예: 8186) |
| STT→LLM→TTS 왕복 | 마이크 정상일 때 | `[http] /api/see -> 200` + 한국어 답변 |
| 터치(FT6336) | **카메라가 버스를 안 건드릴 때** | 턴 후에도 `듣는 중...` 재진입 |
| 생각중 얼굴 애니 | 백그라운드 task로 HTTP | 눈동자 좌우 + 깜빡임 |

**펌웨어 버전:** v16(마이크 연속 DMA 수정) + v17/v19(카메라 OFF) = 음성 루프 완전 동작.

---

## 1. 핵심 충돌: 카메라 초기화가 마이크를 죽인다 ⚠️

CoreS3는 **내부 I2C 버스(port 1) 하나**를 다음이 공유한다:
`AXP2101 PMIC` · `ES7210 마이크 코덱` · `AW88298 스피커 앰프` · `FT6336 터치` · `GC0308 카메라 SCCB`.

실기로 확인된 사실:

- **`esp_camera_init()`가 이 버스를 건드리면 ES7210 마이크가 무음이 된다** → `[mic] enabled=1` 인데 `peak=0 raw=[0 0 0 0]`.
  - 부팅 시 카메라 **프로브만** 해도 발생.
  - 카메라 init이 **실패해도** 발생(버스를 반쯤 망가뜨림).
  - 마이크가 무음이면 서버 STT가 `/api/see -> 422 "No speech detected"` 반환.
- **카메라를 완전히 끄면**(`CAM_ENABLE=0`) 마이크가 즉시 살아난다(`peak=8186`).

### 재현된 방식과 결과
| 카메라 접근 방식 | 결과 |
|---|---|
| `M5.In_I2C.release()` + `i2c_driver_delete()` + `begin()` (구방식) | **터치 死** (턴 후 화면 터치 무반응) |
| `sccb_i2c_port` 버스 재사용 (v18) | **카메라 init 실패 + 마이크 死** (최악: 둘 다 안 됨) |
| 녹음 먼저 + on-demand init + deinit 후 `M5.In_I2C.begin()` (v21~v23) | **캡처·마이크는 OK, 그러나 deinit이 I2C 버스를 wedge → 다음 `M5.update()` 터치 read에서 루프 HANG = 영구 먹통** ⚠️ |
| 카메라 OFF (v17/v19/v24) | **마이크·터치·음성 전부 정상** ✅ |

> **v21~v23의 함정:** 캡처가 성공하고(`[cam] captured ... jpeg N bytes`) 마이크도 살아있어서 "됐다"고 착각하기 쉬움. 하지만 `esp_camera_deinit()`이 공유 I2C를 반쯤 부순 상태로 두면, **다음 루프의 `M5.update()`(FT6336 터치 I2C read)가 완료되지 않는 트랜잭션에서 블록 → 전체 루프 정지**. 워치독을 꺼놨기(`esp_task_wdt_deinit()`) 때문에 리부트도 안 되고 **영구 먹통**. 즉 "카메라 turn 성공" ≠ "안정". 성공 로그 그 다음의 **터치 재동작 + health 로그 지속**까지 확인해야 진짜 안정.

### v26 시도 — I2C 버스 리커버리로 프리즈 정면 대응 (카메라 ON 복귀)
`esp_camera_deinit()` 후 `recoverSharedI2C()` 추가:
1. `M5.In_I2C.release()` + `i2c_driver_delete(port1)` — 드라이버 완전 제거.
2. **비트뱅** SCL 9펄스로 SDA 잡고 있는 슬레이브를 클럭아웃 + STOP (교과서 I2C bus recovery).
3. `M5.In_I2C.begin()` — 깨끗한 버스에 M5 드라이버 재설치.
→ 터치/오디오가 항상 건강한 버스를 돌려받아 다음 루프의 터치 read가 멈추지 않음.
- 카메라는 여전히 `recordAudio()` 이후 on-demand → 마이크 무위험.
- **미검증**: 실기에서 프리즈 재발 여부는 `[cam] shared I2C recovered` 로그 다음의 **터치 재동작 + health 지속**으로 확인.
- 그래도 프리즈면 다음 카드: 카메라 turn 구간에 워치독(Task WDT ~10s, 애니 루프에서 feed)으로 hang 시 자동 리부트 복구.

---

## 2. 규칙 (다음에 반드시 지킬 것)

1. **한 빌드에서 한 가지만 바꾼다.** 음성 루프를 건드리는 빌드에서 카메라를 같이 켜지 마라. 무엇이 깼는지 못 가린다.
2. **카메라는 항상 `CAM_ENABLE` 플래그 뒤, 기본 OFF.** 실기 로그(`[mic] peak`, `[cam]`)로 마이크 생존을 확인하기 전엔 기본 ON으로 커밋 금지.
3. **마이크 무음 진단은 `peak`로.** `enabled=1`은 코덱이 켜졌다는 뜻일 뿐, 소리 잡았다는 뜻이 아니다. `peak=0`이면 무음, `raw`도 함께 본다.
4. **공유 I2C 자원을 만지는 변경은 빌드로 검증했다고 "동작"이라 쓰지 마라.** 실기 시리얼 로그 첨부 전까지 "가설"로 보고.
5. **되돌리기 경로를 항상 남긴다.** `CAM_ENABLE 0` 한 줄로 즉시 기준선 복귀 가능하게.

---

## 3. 카메라를 살리려면 (다음 실험 후보 — 전부 미검증)

동시 사용이 불가능할 수 있음을 전제로:

- **부팅 카메라 프로브 제거** + on-demand init을 `recordAudio()` **이후에만**. 마이크가 먼저 깨끗한 버스에서 녹음하고, 그 다음 카메라를 켜서 캡처.
- **카메라 deinit 후 ES7210 재설정**: `M5.Mic.begin()`이 코덱을 I2C로 재구성하도록 강제해 버스 손상을 원복하는지 검증.
- 그래도 안 되면 **동시 사용 포기 설계**: "사진 볼 땐 그 턴만 음성 없이 카메라 전용" 같은 모드 분리.
- 서버가 이미 **이전 프레임을 캐시**하고 있어(카메라 OFF에서도 비전 답변 나옴) 저빈도 캡처로도 실시간성 일부 확보 가능.

---

## 4. 곁다리로 확인된 환경 이슈 (코드 아님)

- **Windows 권한**: `.git`·`.platformio` 폴더가 관리자 권한으로 생성됨 → 일반 PowerShell에서 `Permission denied`(git reflog append / esptool intelhex 읽기). **관리자 PowerShell**에서 실행하면 통과. 근본 해결은 `takeown`/`icacls`로 소유권 정리.
- `git reset --hard` 막힐 때: `git config core.logAllRefUpdates false` 로 reflog 기록만 잠깐 끄면 우회 가능.
### v27 — 서버 타임아웃 후 먹통 방지: 워치독 + 실패 경로 I2C 재복구
사용자 재현: `서버가 느려요`가 뜬 뒤 다시 터치해도 먹통. v26의 I2C 복구만으로는 서버 실패/타임아웃 뒤 idle 복귀 직전까지 완전히 안전하다고 볼 수 없음.
- `esp_task_wdt_deinit()` 제거, loop task watchdog 재활성화. 터치/I2C read가 다시 block되면 영구 먹통 대신 `TASK_WDT` 리셋으로 복구.
- 정상 장시간 루프(record, thinking animation, volume panel, audio playback, Wi-Fi connect)에 `feedWatchdog()` 추가.
- `/api/see` 백그라운드 task가 135초를 넘기면 `ESP.restart()`로 강제 복구. HTTP read timeout 120초보다 여유를 둔 hard limit.
- `resp.isEmpty()` / bad JSON / 성공 후 playback 종료 시점에 `recoverSharedI2C()`를 한 번 더 호출해서 idle 복귀 전 touch bus를 재정리.
- 기대 로그: 부팅 `route-A v27`, `[wdt] armed (12s)`. 만약 여전히 멈추면 다음 부팅 `last reset reason = TASK_WDT (watchdog)`가 찍혀 영구 먹통 대신 자동복구됨을 확인.

### v28 — 422 뒤 stale touch 먹통 방지
사용자 재현: `잘 안 들렸어요, 다시 말해줘`가 뜬 뒤 터치가 다시 먹통. 직전 로그는 `/api/see -> 422`와 `peak=109 gain=10x`.
- 원인 후보: 너무 작은 녹음도 카메라/서버 경로를 탔고, 카메라 I2C 복구 뒤 post-turn `while (touchPressed())`가 stale pressed 상태를 영원히 release 대기. 이 루프는 watchdog을 feed하므로 리셋도 안 되어 영구 먹통처럼 보일 수 있음.
- 규칙: pre-gain mic peak가 로컬 기준보다 낮으면 카메라와 `/api/see`를 호출하지 말고 로컬에서 "소리가 작아요"로 끝낸다.
- 규칙: 카메라/서버 turn 뒤 터치 release 대기는 반드시 bounded wait로 한다. timeout이면 stale pressed 상태를 무시하고, release가 관측될 때까지 주기적으로 I2C recovery를 재시도한다.

---

## 6. v40~v48 — 카메라 ON 유지 + 공유버스 코덱 매 턴 리셋 (현재 진행)

전략 전환: "카메라 OFF"라는 회피 대신, **카메라를 켠 채로** 공유 I2C에 물린 각 컨트롤러를
카메라 사용 직후 **강제로 되살린다.** 브랜치 `agent/embedded-engineer/9aa2a597`(PR #2).

### 핵심 깨달음
`recoverSharedI2C()`(비트뱅 버스 클럭아웃)는 **전선만** 되살린다. 버스에 물린 **컨트롤러들의
내부 상태(레지스터)** 는 원복하지 않는다 → 카메라 후 각 칩이 망가진 채로 돌아온다:
- **FT6336 터치** → stuck-"pressed"(가짜 눌림): 볼륨 패널 자동 열림 + 터치 먹통
- **ES7210 마이크** → 무음(`peak≈1`): "소리가 작아요" 무한 루프
- **AW88298 스피커** → 무음: `audio_url` 정상인데 답변 음성 안 남

→ **해결: 버스 복구 후 각 컨트롤러를 재초기화한다.** 그리고 **버스 복구가 일어나는 모든
지점(캡처 직후 + 답변 직후)에서** 재초기화해야 한다 — 안 그러면 뒤의 복구가 방금 살린 코덱을
다시 죽인다(v46에서 발견한 재훼손 버그).

### 버전별 변경
| 버전 | 내용 |
|---|---|
| v40 | 실패 후 재시도는 audio-only(카메라 스킵) + I2C 복구를 턴당 1회로 축소 |
| v41 | 스와이프 인식 강화(`confirmSwipe`, 70px·연속·700ms 상한) → 가짜 터치 볼륨 자동열림 차단 |
| v42 | `/api/see` read timeout 120s→30s (Kimi 정상 ~3s라 30s면 stuck 판정) |
| v43 | **FT6336 터치 컨트롤러 리셋**(`settleTouchAfterCamera`): 디바이스모드 레지스터 재기입 + not-pressed 안정화 폴링. **실기 확인 `[touch] controller settled clean after camera` ✅** |
| v44 | tap-to-listen 시간제한(15s) + `confirmRealPress`(~60ms 지속) → 가짜 터치 자동 듣기 루프 차단 |
| v45 | ES7210 **마이크 코덱 재초기화** |
| v46 | **마이크+스피커 둘 다 재초기화**(`settleAudioAfterCamera`) + `recoverAfterCamera()`(복구+터치+오디오)를 캡처·답변 후 양쪽 적용 → 재훼손 버그 제거 |
| v47 | 무해한 `i2c/gdma/I2S` ESP-IDF 에러 로그 음소거(`esp_log_level_set NONE`) — 성공 로그가 고장처럼 보이던 문제 |
| v48 | 풀다운 상태바에 fw 버전 표시(`kFwVersion`) + 음성 경로 전구간 로그(`[audio] GET code/len/vol`, `playWav samples/rate/vol`) |

### 실기 확인됨 (사용자 로그)
- 터치: `[touch] controller settled clean after camera` ✅
- 마이크(카메라 턴): `[mic] peak=6573` ✅
- 서버: `/api/see -> 200 (~6s)` ✅ (인프라가 STT를 threadpool로 빼 50초 wedge 해소)
- 비전: 한국어 이미지 묘사 정답 ✅
- 서버 TTS: `audio_url = /audio/….wav` 생성 ✅
- 보안: X-Device-Token 인증(공개 URL 토큰 없으면 401) ✅

### 미검증 (실기 로그 대기)
- 스피커 음성이 실제로 들리는지(v46 스피커 재초기화 효과) — v48 `[audio]`/`playWav` 로그로 서버 vs 스피커 판별.
- **카메라 턴 다음** 녹음의 `[mic] peak`이 계속 높은지(v46 재훼손 제거 효과).

### 다음 카드 (그래도 안 되면)
- ES7210/AW88298 **레지스터 직접 리셋**(M5.Mic/Speaker begin으로 부족할 때).
- 최후: 카메라 OFF(오디오 전용) — 즉시 완전 안정, 비전 포기. `CAM_ENABLE 0`.

### 서버측(인프라 영역, 참고)
- STT를 `run_in_threadpool`로 분리 → 게이트웨이 50초 wedge 해소, 턴 ~7~8s.
- `/api/see`·`/api/turn`·`/audio` 등에 `X-Device-Token` 인증 강제, Funnel 유지.
- `/api/see` 비전 게이트: transcript가 "뭐가 보여" 류일 때만 이미지 전달.
