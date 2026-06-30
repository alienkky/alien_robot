# M3 실증 검증 — 따라하기 (아주 자세히, 처음 하는 사람용)

이 문서는 "터미널/Railway/펌웨어가 뭔지 잘 모름" 상태에서 **그대로 따라치면** 되도록
쓴 것. 화면에 뭐가 떠야 하는지까지 적음. 막히면 그 줄 번호와 화면을 캡처해서 공유.

## 0. 전체 그림 (먼저 1분만)

세 군데서 작업함. **누가 어디서 도는지** 먼저 머리에 넣기:

| 이름 | 무엇 | 어디서 | 어떻게 |
|---|---|---|---|
| **Railway** | 브레인(생각하는 AI 튜터) | 웹 브라우저 | 클릭 |
| **게이트웨이** | 통역사(음성↔글자 + 브레인 호출) | **4090 PC 터미널** | 명령어 |
| **펌웨어** | 로봇 몸(보드, 마이크·카메라·스피커) | **4090 PC + USB 보드** | VS Code 버튼 |

검증 순서: **① Railway 켜기 → ② 게이트웨이 켜고 글자로 먼저 테스트 → ③ 보드에 펌웨어 굽기 → ④ 말 걸기.**
보드는 ②까지 잘 되는 걸 확인한 뒤에 굽는다(문제 범위를 좁히려고).

---

## B. Railway 하는 방법 (브라우저, 먼저 함)

> Railway = 우리 브레인(brain180)을 인터넷에 띄워 두는 곳. 보드/게이트웨이가 여기로 질문을 보냄.

### B-1. 비밀 토큰 1개 만들기 (게이트웨이랑 brain180이 서로 알아보는 암호)
- 아무 긴 문자열이면 됨. 예: 윈도우 PowerShell에 `[guid]::NewGuid().ToString("N")` 치면 나오는 32자.
- 이 값을 **메모장에 적어둠.** (뒤에서 두 군데 똑같이 넣음.)

### B-2. Railway 로그인
- 브라우저로 https://railway.app → 우상단 **Login** → **Login with GitHub** (alienkky 계정).

### B-3. brain180 서비스 열기
- 대시보드에 **brain180** 프로젝트가 있으면 클릭해서 들어감.
- **없으면**(처음이면): **New Project → Deploy from GitHub repo → `alienkky/brain180` 선택.** (DB 등 기존 설정이 필요하니, 이미 운영 중인 프로젝트가 있으면 그걸 쓰는 게 맞음 — 없으면 인프라 담당과 상의.)

### B-4. 배포할 브랜치 지정 (로봇 기능이 든 가지)
- 서비스 클릭 → 상단 **Settings** 탭 → **Source** 영역 → **Branch** 드롭다운을 **`feat/robot-bridge-ali21`** 로 바꿈 → 저장.
- (이게 로봇 엔드포인트가 들어있는 브랜치임.)

### B-5. 환경변수(Variables) 넣기
- 서비스 → **Variables** 탭 → **New Variable** 버튼으로 하나씩 추가:
  - `ROBOT_DEVICE_TOKEN` = (B-1에서 만든 토큰)
  - `OPENAI_API_KEY` = (카메라로 본 걸 설명시키려면 필요. OpenAI 키. 없으면 음성 대화는 되고 카메라 설명만 안 됨)
  - 이미 들어있을 값들: `DATABASE_URL`, `MOONSHOT_API_KEY`(또는 `KIMI_API_KEY`) — 건드리지 말 것.
- 저장하면 보통 자동 재배포됨. 안 되면 우상단 **Deploy** 클릭.

### B-6. 공개 주소(URL) 확인 — 게이트웨이에 넣을 주소
- 서비스 → **Settings → Networking → Public Domain.** 예: `brain180-production.up.railway.app`
- 없으면 **Generate Domain** 클릭. 이 주소(앞에 `https://` 붙여서)를 **메모장에 적어둠.**

### B-7. 잘 떴는지 확인
- ⚠️ **PowerShell 주의:** 그냥 `curl`은 PowerShell에서 다른 명령(Invoke-WebRequest)의 별칭이라
  `-H` 옵션을 못 받음. 반드시 **`curl.exe`** 로 쓰고, **`토큰값`은 B-1에서 만든 진짜 토큰**으로 바꿀 것.
  ```
  curl.exe -H "Authorization: Bearer 진짜토큰값" https://여기에-railway-주소/api/robot/health
  ```
  PowerShell식이 편하면 이렇게도 됨:
  ```
  Invoke-RestMethod "https://여기에-railway-주소/api/robot/health" -Headers @{ Authorization = "Bearer 진짜토큰값" }
  ```
- **`"status":"ok"` 가 보이면 브레인 준비 끝.**
  - `text_provider` 가 채워져 있어야 함(대화 가능). `vision_provider:"openai"` 면 카메라도 가능.
  - 화면에 `401` → 토큰 틀림. `503` → ROBOT_DEVICE_TOKEN 안 들어감. 아무 응답 없음 → 주소/배포 확인.

---

## A. 터미널 작업 (4090 PC) — 게이트웨이 켜기

> 터미널 = 명령어 치는 검은 창. 여기 게이트웨이(통역사)를 띄움. **보드 꽂을 그 PC**에서 함.

### A-1. 터미널(PowerShell) 열기
- 윈도우 **시작** 버튼 → `powershell` 입력 → **Windows PowerShell** 클릭. 검은(또는 파란) 창이 뜸.

### A-2. 필요한 프로그램 있는지 확인 (없으면 설치)
한 줄씩 치고 Enter. 버전 숫자가 나오면 있는 것:
```
git --version
python --version
```
- `git` 없으면: https://git-scm.com/download/win 설치(전부 Next) 또는 `winget install Git.Git`
- `python` 없으면: https://www.python.org/downloads/ 설치. **설치 첫 화면에서 "Add python.exe to PATH" 체크 필수.** 또는 `winget install Python.Python.3.12`
- 설치했으면 PowerShell 창을 **닫았다 다시 열기**(그래야 인식됨).

### A-3. 코드 내려받기 (로봇 기능이 든 가지로)
```
cd $HOME\Desktop
git clone -b agent/embedded-engineer/4d2c69d9 https://github.com/alienkky/alien_robot.git
cd alien_robot\backend
```
- 바탕화면에 `alien_robot` 폴더가 생김. (`-b ...` 는 "이 가지를 받아라"는 뜻 — 우리 최신 코드가 이 가지에 있음.)

### A-4. 게이트웨이 설치
```
python -m venv .venv
.\.venv\Scripts\Activate.ps1
pip install -r requirements.txt
Copy-Item config.example.env .env
```
- 줄 앞에 `(.venv)` 가 붙으면 정상.
- `Activate.ps1` 에서 빨간 보안 에러가 나면 한 번만:
  `Set-ExecutionPolicy -Scope CurrentUser RemoteSigned` (Y 입력) → 다시 `.\.venv\Scripts\Activate.ps1`
- `pip install` 은 몇 분 걸림(특히 처음 faster-whisper). 끝까지 기다림.

### A-5. 설정파일(.env) 채우기
```
notepad .env
```
메모장이 뜨면 아래 4줄을 채우고 저장:
```
LLM_PROVIDER=brain180
BRAIN180_BASE_URL=https://여기에-B6에서-적은-railway-주소
BRAIN180_DEVICE_TOKEN=여기에-B1에서-만든-토큰
MOCK_TRANSCRIPT=안녕, 너 누구야?
```
- ⚠️ **`LLM_PROVIDER` 는 파일에 이미 `LLM_PROVIDER=ollama` 로 들어있음. 그 줄을 `brain180`으로
  "바꿔야" 함**(새 줄을 또 추가하지 말 것). ollama 그대로면 A-7에서 `Ollama ... 404` 에러가 남.
- 마지막 `MOCK_TRANSCRIPT` 는 **보드 없이 글자로 먼저 시험**하려고 임시로 넣는 것(나중에 지움).
- 저장 후 확인(선택): `Get-Content .env | Select-String "LLM_PROVIDER|BRAIN180"` → `LLM_PROVIDER=brain180` 한 줄만 보이면 정상.

### A-6. 게이트웨이 켜기
```
uvicorn app:app --host 0.0.0.0 --port 8787
```
- **`Application startup complete.`** 가 보이면 성공. **이 창은 그대로 둠**(끄면 게이트웨이 꺼짐).

### A-7. 보드 없이 글자 테스트 (중요)
- **새 PowerShell 창**을 하나 더 열고:
```
cd $HOME\Desktop\alien_robot\backend
$bytes = New-Object byte[] 32000
[IO.File]::WriteAllBytes("$PWD\silence.pcm", $bytes)
curl.exe -X POST -H "Content-Type: application/octet-stream" --data-binary "@silence.pcm" http://127.0.0.1:8787/api/turn
```
(여기도 `curl` 아니라 **`curl.exe`**. `-X POST`/`--data-binary` 가 진짜 curl 문법.)
- 응답에 **`"answer":"...한국어 문장..."`** 이 오면 **브레인↔게이트웨이 연결 성공!**
  - `502 Brain180 ...` → A-5의 주소/토큰이 B와 다름. `upstream_error` → Railway의 LLM 키 확인.
- 성공했으면 `notepad .env` 다시 열어 **`MOCK_TRANSCRIPT` 줄을 지우고 저장**, A-6 창에서 Ctrl+C 후 다시 `uvicorn ...` 실행(이제 진짜 마이크 음성으로 동작).

### A-8. 이 PC의 IP 주소 알아두기 (펌웨어에 넣을 것)
```
ipconfig
```
- 여러 개가 나오면 **"이더넷 어댑터 이더넷"(또는 Wi-Fi 어댑터)의 IPv4** 하나만 씀. 나머지는 제외:
  - **Tailscale (100.x)** = VPN → ✗
  - **vEthernet (Default Switch / WSL) (172.x)** = 가상 네트워크 → ✗
  - **Bluetooth** = ✗
- ⚠️ **보드(ESP32)와 이 PC가 같은 공유기(같은 Wi-Fi)에 있어야** 보드가 이 IP로 붙음.
  - 보드 붙일 Wi-Fi에 스마트폰을 연결해 폰 IP 대역을 보면 같은 망인지 알 수 있음(예: 폰이 `192.168.0.x`인데 PC가 `220.x`면 서로 다른 망 → PC를 그 공유기에 유선으로 연결하고 다시 ipconfig).
- 찾은 IPv4를 **메모.** (보드가 이 주소로 게이트웨이를 부름.)
- **윈도우 방화벽**이 물어보면 "허용". (8787 포트가 막히면 보드가 못 붙음.)

---

## C. 펌웨어 하는 방법 (보드에 굽기) — VS Code 버튼으로

> 펌웨어 = 보드 안에 들어가는 프로그램. VS Code라는 프로그램의 버튼으로 굽는 게 제일 쉬움.

### C-1. VS Code + PlatformIO 설치
- https://code.visualstudio.com 에서 VS Code 설치.
- VS Code 왼쪽 세로 막대의 **퍼즐조각 아이콘(Extensions)** 클릭 → 검색창에 `PlatformIO IDE` → **Install**.
- 설치 끝나면 VS Code 한 번 **재시작**(닫았다 열기). 처음엔 PlatformIO가 도구를 받느라 몇 분 걸림(우하단 진행표시).

### C-2. 펌웨어 폴더 열기
- VS Code 상단 **File → Open Folder** → A-3에서 받은 **`alien_robot\firmware`** 폴더 선택.
- (꼭 `firmware` 폴더여야 함. 그 위 `alien_robot` 아님.)

### C-3. 설정파일 만들기
- 왼쪽 파일목록에서 `include` 폴더 펼치기 → `config_waveshare.h.example` 우클릭 → **Copy**, 다시 우클릭 **Paste** → 생긴 사본 이름을 **`config_waveshare.h`** 로 바꿈(.example 떼기).
- 그 파일을 열어 윗부분 3줄 채우기:
```c
#define WIFI_SSID       "집_와이파이_이름"
#define WIFI_PASSWORD   "와이파이_비번"
#define AI_SERVER_BASE_URL "http://192.168.x.x:8787"   // ← A-8에서 적은 IP
```
- 저장(Ctrl+S). **와이파이는 2.4GHz 대역**이어야 함(ESP32는 5GHz 못 씀).

### C-4. 보드 연결
- 보드를 USB-C로 PC에 꽂음. **데이터 되는 케이블**이어야 함(충전 전용 X).

### C-5. 빌드할 환경 고르기
- VS Code 맨 아래 파란 막대의 **집 아이콘 옆 환경 이름**(예: `Default (firmware)`) 클릭 → 목록에서 **`env:waveshare-s3-touch-lcd-35b`** 선택.

### C-6. 빌드(컴파일) → 업로드(굽기) → 모니터(로그)
맨 아래 파란 막대에 작은 아이콘들이 있음:
- **✓ (체크표시) = Build.** 눌러서 `SUCCESS` 확인.
- **→ (오른쪽 화살표) = Upload(굽기).** 누름. 보드가 안 잡히면: 보드의 **BOOT 버튼을 누른 채 RESET 한 번** 누르고 떼고, 다시 → 클릭.
- **🔌 (콘센트 모양) = Serial Monitor(로그 보기).** 눌러서 글자가 흐르는지 봄.

### C-7. 동작 확인 (시리얼 로그 보면서)
순서대로 떠야 함:
1. `[es8311] chip id1=0x83` (소리칩 인식)
2. `[cam] ... init OK` (카메라 인식)
3. `WiFi connected: 192.168...` (와이파이 붙음)
4. `[boot] ready — hold BOOT to talk`
5. **보드의 BOOT 버튼을 누른 채 말하고 손 떼기** → `[turn] you: 내가 한 말` / `[turn] bot: 로봇의 한국어 답` 이 뜨면 성공.
6. 카메라 앞에 물건 두고 BOOT 누르고 "이거 뭐야?" → 답에 그 물건이 나오면 카메라까지 성공.

> CLI가 편하면 VS Code 대신 터미널에서도 됨:
> `cd alien_robot\firmware` → `pio run -e waveshare-s3-touch-lcd-35b -t upload` → `pio device monitor -e waveshare-s3-touch-lcd-35b`

---

## 막히면 (자주 나오는 것)

| 화면/증상 | 조치 |
|---|---|
| `Activate.ps1` 빨간 보안 에러 | `Set-ExecutionPolicy -Scope CurrentUser RemoteSigned` (Y) 후 다시 |
| `git`/`python` "인식할 수 없는" | 미설치 → A-2대로 설치 후 창 다시 열기 |
| `[Errno 10048] ... bind ... 8787` | 8787 포트가 이미 점유됨(이전 uvicorn 창이 안 닫힘/중복 실행). 이전 창에서 Ctrl+C 하거나: `netstat -ano \| findstr :8787` 로 맨 끝 PID 확인 → `taskkill /PID <PID> /F` → 다시 uvicorn |
| 글자 테스트가 `Ollama ... 404` | `.env`의 `LLM_PROVIDER`가 아직 `ollama`임. **`brain180`으로 바꾸고 uvicorn 재시작**(.env 수정은 재시작해야 반영) |
| 글자 테스트가 `502` | A-5 주소/토큰이 B의 값과 똑같은지 |
| 답이 "이미지를 볼 수 없습니다" | Railway에 `OPENAI_API_KEY` 추가(B-5) |
| 업로드시 보드 안 잡힘 | BOOT 누른 채 RESET 후 다시 Upload. USB 케이블 교체 |
| 보드가 게이트웨이 못 붙음(시리얼에 타임아웃) | 같은 와이파이인지, A-8 방화벽 허용, IP 정확한지 |
| `[es8311] no I2C ACK` / 소리 깨짐 | 소리칩 초기화는 **미검증** 부분. 로그 캡처해서 공유 주면 코드 잡음(먼저 글자 답은 정상인지로 분리 확인) |

전체 한 줄 흐름: **BOOT+말+카메라 → 보드 → 게이트웨이(글자로 바꿈) → Railway 브레인 → 한국어 답 → 게이트웨이(음성으로) → 보드 스피커.**
