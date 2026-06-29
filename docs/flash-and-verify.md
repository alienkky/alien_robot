# 플래싱·시리얼 검증 절차

ESP32-S3 보드 실물 검증용. PC는 4090 호스트(WSL2 아님) 또는 별도 Windows/Mac.

---

## 사전 준비

```powershell
# PlatformIO CLI 없으면 설치
pip install platformio

# 저장소 클론 (또는 이미 있으면 최신 pull)
git clone https://github.com/alienkky/alien_robot
cd alien_robot/firmware

# config.h 생성
copy include\config.h.example include\config.h
notepad include\config.h   # ← WIFI_SSID / WIFI_PASSWORD / 서버 IP 입력
```

`config.h`에서 반드시 채울 값:

| 항목 | 설명 |
|------|------|
| `WIFI_SSID` | ESP32와 4090 PC가 같은 공유기에 연결될 SSID |
| `WIFI_PASSWORD` | 위 AP 비밀번호 |
| `AI_SERVER_BASE_URL` | Phase 0: `http://4090-PC-IP:8787` |
| `XIAOZHI_SERVER_HOST` | Phase 1: 4090 PC LAN IP (예: `192.168.0.20`) |

`config.h`는 `.gitignore`에 포함됨 — 커밋되지 않음.

---

## 빌드 & 플래시

### Phase 0 (HTTP, FastAPI 백엔드)

```bash
# ESP32-S3를 USB로 연결한 뒤
pio run -e http -t upload
pio device monitor   # 115200 baud
```

### Phase 1 (WebSocket, xiaozhi-esp32-server 필요)

```bash
pio run -e ws -t upload
pio device monitor
```

---

## 시리얼 검증 체크리스트

ESP32를 USB로 꽂고 `pio device monitor` 실행. 아래 순서로 확인.

### Step 1 — 부팅

```
WiFi connecting......
WiFi connected: 192.168.0.xxx
```

- `WiFi connecting` 뒤 점이 끝없이 찍히면 → SSID/비밀번호 또는 AP 채널 문제.
- IP가 출력되면 같은 PC에서 `ping 192.168.0.xxx` 확인.

### Step 2 — PSRAM

정상 부팅 시 PSRAM 에러 없이 바로 "Ready" 출력.

```
Ready (HTTP mode). Hold button to talk.
```

다음 메시지가 보이면 PSRAM 미활성화:

```
PSRAM alloc failed. Use ESP32-S3 board with PSRAM and OPI memory type.
```

→ 보드가 OPI PSRAM 탑재 모델인지 확인. QPI 보드면 `platformio.ini`의
`board_build.arduino.memory_type = qio_opi` 를 `qio_qspi`로 변경 후 재플래시.

### Step 3 — Phase 0: HTTP 루프 검증

1. **마이크 미배선 상태로 테스트할 경우:** 백엔드를 `MOCK_TRANSCRIPT=true`로 실행.
   FastAPI가 실제 STT 없이 고정 텍스트로 LLM까지 돌린다.

2. 버튼(GPIO 0) 누르고 2초 후 떼기.
   ```
   Recording...
   Recorded 64000 bytes
   Server status: 200
   You: (mock transcript 또는 실제 인식 텍스트)
   AI: (LLM 응답)
   Ready. Hold button to talk.
   ```

3. 오디오 재생이 없으면(TTS 미설정) Serial 텍스트만으로도 루프 확인 완료.

### Step 4 — Phase 1: WebSocket 루프 검증

1. xiaozhi-esp32-server가 4090에서 실행 중인지 확인:
   ```bash
   curl http://4090-PC-IP:8003/health   # {"status":"ok"} 기대
   ```

2. 플래시 후 시리얼 확인:
   ```
   WS: connecting to 192.168.0.20:8003/xiaozhi/v1/
   WS: connected to /xiaozhi/v1/
   WS: sent hello
   WS: server hello ok (TTS rate=24000)
   Ready (WebSocket mode). Hold button to talk.
   ```

3. 버튼 누르고 2초 후 떼기:
   ```
   Recording...
   Recorded 64000 bytes
   WS: sent 64000 bytes PCM
   You: (STT 결과)
   AI: (LLM 응답)
   Ready. Hold button to talk.
   ```

4. TTS 오디오가 스피커에서 나와야 완료.

---

## 백엔드 빠른 시작 (Phase 0 검증용)

```bash
# 4090 PC WSL2 또는 로컬 Python
cd alien_robot/backend
cp config.example.env .env
# .env: LLM_PROVIDER=vllm, MOCK_TRANSCRIPT=true (마이크 없을 때)
pip install -r requirements.txt
uvicorn app:app --host 0.0.0.0 --port 8787

# 헬스 체크
curl http://localhost:8787/health
```

---

## 배선 재확인 (hardware.md 요약)

| 기능 | GPIO |
|------|------|
| PTT 버튼 | 0 (한쪽 → GND) |
| 상태 LED | 2 |
| Mic BCLK | 4 |
| Mic WS | 5 |
| Mic SD/DIN | 6 |
| Spk BCLK | 15 |
| Spk WS/LRC | 16 |
| Spk DIN | 17 |

INMP441의 L/R 핀 → GND (Left 채널 선택).
MAX98357A의 SD 핀 → 3.3V 또는 VIN (항상 출력 활성).
