# M3 실증 검증 런북 (집/실보드)

집에서 **실보드 + 4090 PC**로 끝까지 검증하는 절차. 위에서부터 순서대로.
3개 구성요소: **① Brain180(브레인)** → **② 게이트웨이(4090, STT/TTS+브리지)** → **③ ESP32 펌웨어(단말)**.

> 전제: 보드 = Waveshare ESP32-S3-Touch-LCD-3.5B(N16R8). 4090 PC(Windows). 같은 Wi-Fi LAN.
> 원칙: **단계별로 통과 확인 후 다음으로.** 한 군데 막히면 그 단계에서 멈추고 로그 캡처.

---

## 0. 준비물 체크
- [ ] 보드 + USB-C 케이블(데이터용. 충전전용 케이블 주의)
- [ ] 4090 PC: Python 3.11+, Node 22+, PlatformIO(`pip install platformio` 또는 VS Code 확장)
- [ ] Brain180: **Railway 배포 접근**(권장) 또는 4090 로컬 구동용 Postgres
- [ ] API 키: Kimi/Moonshot(텍스트) + **비전용 OpenAI 또는 Anthropic 키**(카메라 검증에 필수 — Kimi는 이미지 못 봄)
- [ ] 공유 비밀 토큰 1개 직접 생성: 예) PowerShell `[guid]::NewGuid().ToString("N")` → 이게 `ROBOT_DEVICE_TOKEN`

---

## 1. Brain180 (브레인) 준비

### 경로 A — Railway (권장, Postgres 설치 불필요)
1. Railway의 brain180 서비스 **Variables**에 추가:
   - `ROBOT_DEVICE_TOKEN` = 위에서 만든 토큰
   - 카메라 비전 쓰려면 `OPENAI_API_KEY`(또는 `ANTHROPIC_API_KEY`) 추가
   - (텍스트 대화는 기존 `MOONSHOT_API_KEY`/`KIMI_API_KEY` 그대로)
2. 브랜치 배포: 로봇 엔드포인트는 **`feat/robot-bridge-ali21`** 브랜치에 있음.
   메인에 머지하거나 Railway가 이 브랜치를 빌드하도록 지정. 재배포.
3. 확인(아무 PC에서):
   ```powershell
   # PowerShell에서는 반드시 curl.exe (그냥 curl은 Invoke-WebRequest 별칭이라 -H 안 먹음)
   curl.exe -H "Authorization: Bearer <토큰>" https://<your-app>.up.railway.app/api/robot/health
   ```
   기대: `{"data":{"status":"ok","text_provider":"kimi","vision_provider":"openai"},...}`
   - `vision_provider":"none"` 이면 카메라 설명이 안 됨 → 비전 키 추가.
   - 401 이면 토큰 불일치, 503 이면 `ROBOT_DEVICE_TOKEN` 미설정.

### 경로 B — 4090 로컬 구동 (프라이빗, Postgres 필요)
```powershell
git clone -b feat/robot-bridge-ali21 https://github.com/alienkky/brain180.git
cd brain180
npm ci
Copy-Item .env.example .env
#  .env 최소 항목 채우기:
#   DATABASE_URL=postgres://app:비번@localhost:5432/brain180   (로컬 Postgres 또는 Neon)
#   ANON_SALT, SESSION_SECRET = 각각 32자 무작위
#   MOONSHOT_API_KEY=<kimi 키>
#   OPENAI_API_KEY=<비전용>            (카메라 검증 시)
#   ROBOT_DEVICE_TOKEN=<토큰>
#   PORT=3001
npm run dev:server            # 부팅 시 마이그레이션+시드 자동(AUTO_BOOTSTRAP)
# 새 터미널에서 스모크:
$env:ROBOT_DEVICE_TOKEN="<토큰>"; npm run smoke:robot   # PASS 떠야 함
```

> **통과 기준(1단계):** `/api/robot/health` 200 + `text_provider` 채워짐. (카메라까지면 `vision_provider`도.)

---

## 2. 게이트웨이 (4090, STT/TTS + Brain180 브리지)

```powershell
git clone https://github.com/alienkky/alien_robot.git
cd alien_robot/backend
python -m venv .venv ; .\.venv\Scripts\Activate.ps1
pip install -r requirements.txt
Copy-Item config.example.env .env
```
`.env` 편집(핵심만):
```
LLM_PROVIDER=brain180
BRAIN180_BASE_URL=https://<your-app>.up.railway.app   # 경로 A. 로컬이면 http://127.0.0.1:3001
BRAIN180_DEVICE_TOKEN=<1단계와 동일 토큰>
WHISPER_MODEL=base        # CPU면 tiny/base, 4090 GPU면 small + WHISPER_DEVICE=cuda
WHISPER_DEVICE=cpu
# 음성 출력(스피커)까지 검증하려면 Piper 설치 후:
PIPER_BIN=                # 예) C:\piper\piper.exe
PIPER_MODEL=              # 예) C:\piper\ko_KR-xxxx.onnx
```
실행:
```powershell
uvicorn app:app --host 0.0.0.0 --port 8787
```

### 보드 없이 먼저 브레인 루프 검증 (중요 — 펌웨어 굽기 전에)
STT를 건너뛰고 텍스트로 전체 경로를 때려본다. `.env`에 임시로 `MOCK_TRANSCRIPT=안녕, 너 누구야?` 추가 후 재시작, 그 다음:
```powershell
curl.exe http://127.0.0.1:8787/health
# 더미 PCM(1초 무음=32000바이트)로 /api/turn 호출:
$bytes = New-Object byte[] 32000
[IO.File]::WriteAllBytes("$PWD\silence.pcm", $bytes)
curl.exe -X POST -H "Content-Type: application/octet-stream" --data-binary "@silence.pcm" http://127.0.0.1:8787/api/turn
```
기대 응답: `{"transcript":"안녕, 너 누구야?","answer":"<로봇 한국어 답>","audio_url":null}`
- `answer`에 한국어가 오면 **브레인 연동 OK.** 검증 끝나면 `MOCK_TRANSCRIPT` 다시 비우기.
- 502 `Brain180 ...` → 1단계 URL/토큰 확인. 502 `upstream_error` → brain180쪽 LLM 키 확인.

> **통과 기준(2단계):** `/api/turn`이 한국어 `answer` 반환. (LAN의 다른 PC에서 `http://<4090-IP>:8787/health` 도 되는지 방화벽 확인.)

---

## 3. ESP32 펌웨어 (단말 — 보드에 굽기)

### 3-1. 실보드 1건 재확인
- 디스플레이 IC가 **AXS15231B**인지 실크/스펙 확인. (ST7796면 다른 변형 — 카메라/오디오 핀은 동일하니 본 펌웨어 그대로 진행 가능, 디스플레이만 추후.)

### 3-2. 설정 파일 작성
```powershell
cd alien_robot/firmware
Copy-Item include\config_waveshare.h.example include\config_waveshare.h
```
`include\config_waveshare.h` 편집:
```c
#define WIFI_SSID       "집_와이파이"
#define WIFI_PASSWORD   "비번"
#define AI_SERVER_BASE_URL "http://<4090_LAN_IP>:8787"   // 예: http://192.168.0.20:8787
```
> `<4090_LAN_IP>`는 게이트웨이 PC의 LAN IP(`ipconfig`의 IPv4). `127.0.0.1` 아님.

### 3-3. 빌드 + 플래시
보드를 USB로 연결. 안 잡히면 **BOOT 누른 채 RESET**(다운로드 모드).
```powershell
# 빌드만(점검):
pio run -e waveshare-s3-touch-lcd-35b
# 빌드+업로드(COM 포트는 자동감지, 안되면 --upload-port COMx):
pio run -e waveshare-s3-touch-lcd-35b -t upload
# 시리얼 로그:
pio device monitor -e waveshare-s3-touch-lcd-35b
```

### 3-4. 시리얼 검증 체크리스트 (115200)
부팅 로그를 보며 순서대로:
1. [ ] `[es8311] chip id1=0x83` (또는 ACK) — 코덱 I2C 인식. (`no I2C ACK`면 §4 참고)
2. [ ] `[cam] OV5640/OV2640 init OK` — 카메라 인식.
3. [ ] `WiFi connected: 192.168...` — Wi-Fi 접속 + IP.
4. [ ] `[boot] ready — hold BOOT to talk`
5. [ ] **BOOT 버튼 누른 채 말하기** → 떼면: `[rec] N bytes` → `[turn] you: <받아쓴 말>` → `[turn] bot: <한국어 답>`
6. [ ] 카메라 앞에 물건 두고 "이거 뭐야?" → 답에 그 물건이 묘사되면 **비전 OK**.
7. [ ] (Piper 설정 시) 스피커에서 답이 음성으로 들림.

> **최종 완료 기준:** 5(음성 왕복) + 6(카메라 묘사). 7은 Piper 있을 때.

---

## 4. 트러블슈팅

| 증상 | 원인 / 조치 |
|---|---|
| `[es8311] no I2C ACK` | SDA=8/SCL=7 확인. **ES8311 init 레지스터는 미검증** — 소리 이상하면 `firmware/src/es8311.cpp`의 init 시퀀스를 ES8311 데이터시트 기준으로 조정(클럭 계수 0x02~0x05). |
| 녹음은 되는데 소리 깨짐/무음 | I2S 클럭(`fixed_mclk`, `use_apll`)·ES8311 볼륨. 먼저 `/api/turn` 텍스트 답이 정상인지로 STT/브레인은 분리 확인. |
| `[cam] init failed` | DVP 핀(config_waveshare.h)·리본 케이블. PSRAM 미인식이면 빌드 env의 `qio_opi`/`-DBOARD_HAS_PSRAM` 확인. |
| 시리얼에 `/api/see -> 0` 또는 타임아웃 | `AI_SERVER_BASE_URL` IP·포트, 4090 방화벽(8787 인바운드 허용), 같은 LAN인지. |
| `answer`가 "이미지를 볼 수 없습니다" | brain180에 비전 키(OPENAI/ANTHROPIC) 없음 → 1단계에서 추가. |
| 게이트웨이 502 `Brain180 unreachable` | `BRAIN180_BASE_URL`/토큰. Railway면 URL https 확인. |
| Wi-Fi 접속 무한 점(`.....`) | SSID/비번, 2.4GHz 대역인지(ESP32는 5GHz 불가). |
| 첫 STT가 느림 | faster-whisper 모델 최초 다운로드. 한 번 받으면 캐시됨. |

---

## 5. 한눈 흐름
```
[BOOT 누르고 말 + 카메라] → ESP32(ES8311 녹음 + OV5640 JPEG)
   → POST /api/see → 게이트웨이(4090): STT(faster-whisper)
   → POST /api/robot/chat(텍스트+이미지, Bearer) → Brain180 튜터(Kimi/비전키)
   → 한국어 답 → 게이트웨이 TTS(Piper) → WAV
   → ESP32 ES8311 스피커 재생
```
- 음성만 빠르게: ESP32가 `/api/turn`(M0 프로토콜)만 써도 됨(이미지 없이). 카메라 검증은 `/api/see`.
- 프라이빗 전환(나중): brain180 `AI_PROVIDER`를 4090 vLLM/Qwen3.6으로 → 게이트웨이/펌웨어 변경 0.

상세 설계·계약: `docs/m3-brain180-integration.md` / 핀맵: `docs/m3-board-bringup.md`, `docs/hardware.md`.
