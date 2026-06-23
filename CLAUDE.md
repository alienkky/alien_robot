# CLAUDE.md — Alien Robot 프로젝트 컨텍스트

> 이 파일은 Claude Code 에이전트가 프로젝트 진입 시 자동으로 읽는 컨텍스트다.
> 작업 전 반드시 이 파일과 `docs/HANDOFF.md`를 먼저 확인할 것.

---

## 1. 프로젝트 정체성

**Alien Robot**은 Alien Agentic(AA)의 물리적 로봇 페르소나다.
ESP32-S3를 저비용 음성·영상 단말로 쓰고, 무거운 추론(STT/LLM/비전/TTS)은
로컬 4090 PC에서 돌린다. 클라우드 AI 의존 없이 프라이빗하게 작동하는 것이 핵심.

- **현재 코드네임:** ALI-112
- **현재 브랜치:** `ali-112-esp32-ai-dialogue`
- **목표:** 음성 전용 프로토타입 → 카메라 비전 + 페르소나 탑재 로봇

---

## 2. 기술 스택 (목표 상태)

| 레이어 | 선택 | 비고 |
|--------|------|------|
| 추론 엔진 | **vLLM ≥ 0.19.0** | Ollama는 Qwen3.6 비전 미지원 → 탈락 |
| LLM + 비전 | **Qwen3.6-27B INT4** | 단일 모델로 대화·비전 통합 (멀티모달) |
| STT | SenseVoice (한국어) | 현재 코드는 faster-whisper |
| TTS | EdgeTTS(MVP) → CosyVoice 2 | VRAM 여유 확보 위해 단계적 |
| 오케스트레이션 | xiaozhi-esp32-server | WebSocket 기반 |
| 단말 | ESP32-S3 / StackChan | 카메라·마이크·스피커·팬틸트 |
| 호스트 | 4090 PC / WSL2 / Docker | Multica 인프라 위에 얹음 |

**VRAM 예산 (4090 24GB):** 27B INT4(~17.5GB) + 비전 타워(~0.9GB) + KV(~1.5GB)
+ ASR(~1GB) ≈ **21GB**. TTS는 MVP 단계에선 클라우드(EdgeTTS)로 빼서 카드를 비운다.

---

## 3. 디렉토리 구조

```
alien_robot/
├── CLAUDE.md            # 이 파일 (에이전트 컨텍스트)
├── README.md            # 사용자용 빠른 시작
├── docs/
│   ├── HANDOFF.md       # 상세 인계 문서 (작업 전 필독)
│   ├── hardware.md      # 배선·보드 노트
│   └── protocol.md      # 펌웨어↔백엔드 오디오 계약
├── firmware/            # PlatformIO Arduino (ESP32-S3 단말)
└── backend/             # 로컬 AI 서버
```

---

## 4. 개발 규칙 (반드시 준수)

### 언어·페르소나
- 로봇 출력은 **항상 한국어**.
- 페르소나: 위트 있고 관찰력 날카로움, 한 발 물러서서 명료하게 봄, 지적으로 한 발 떨어진 거리감, 간결함.
- **불교 용어(禪/Zen/간화선 등) 직접 사용 금지.** "한 발 물러서서 명료하게 본다" 같은 평범한 표현으로 의미만 전달.
- 사과·면책·군더더기 표현 배제.

### 코드
- 풀 스크립트로 작성. 생략·플레이스홀더 금지.
- 주석으로 흐름 설명.
- 환경변수·시크릿은 `.env` / `config.h`로 분리, 절대 커밋 금지.

### 커밋
- 작업 단위로 커밋. 메시지는 명령형 현재시제.
- 한 PR = 한 마일스톤 단위 목표.

---

## 5. 빌드 / 실행

### 백엔드 (목표: vLLM 스택)
```bash
# vLLM + xiaozhi-server (docker-compose)
docker compose up -d
curl http://localhost:8000/v1/models       # vLLM 헬스체크
```

### 백엔드 (현재: FastAPI 레거시)
```bash
cd backend
python -m venv .venv && .\.venv\Scripts\Activate.ps1
pip install -r requirements.txt
Copy-Item config.example.env .env
uvicorn app:app --host 0.0.0.0 --port 8787
```

### 펌웨어
```bash
cd firmware
Copy-Item include\config.h.example include\config.h   # Wi-Fi·서버IP 입력
pio run -t upload
pio device monitor
```

---

## 6. 에이전트 작업 규칙

- 작업 시작 전 `docs/HANDOFF.md`의 **갭 분석**과 **마일스톤**을 확인하고,
  지금 어느 마일스톤(M0~M4)에 있는지 먼저 파악할 것.
- 레거시 코드(Ollama/whisper/piper)를 삭제하기 전에, 목표 스택이 동작 검증될
  때까지 **병행 유지**. 작동하는 것을 먼저 깨지 말 것.
- VRAM은 항상 빠듯하다. 모델 추가·교체 시 반드시 예산표를 갱신할 것.
- 막히면 추측하지 말고 `docs/HANDOFF.md`의 레퍼런스 링크를 확인.
