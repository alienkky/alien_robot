# HANDOFF.md — Alien Robot 프로젝트 인계 문서

**작성일:** 2026-06-23
**대상:** AA 멀티 에이전트 시스템 (`.claude/agents/`)
**저장소:** https://github.com/alienkky/alien_robot
**현재 브랜치:** `ali-112-esp32-ai-dialogue`

---

## TL;DR

Alien Robot은 **ESP32-S3 단말 + 로컬 4090 추론 서버**로 구성된 프라이빗 AI 대화 로봇이다.
현재 저장소는 **음성 전용 프로토타입(M0)** 상태이며, Ollama·whisper·piper 기반이다.
인계 목표는 이걸 **vLLM + Qwen3.6-27B(비전 통합) + StackChan 하드웨어**로 진화시키는 것.

**가장 중요한 결정 3가지:**
1. 추론 엔진은 **vLLM** (Ollama는 Qwen3.6 비전을 지원하지 않음)
2. LLM과 비전은 **Qwen3.6-27B 단일 모델**로 통합 (멀티모달)
3. TTS는 MVP 단계에서 **클라우드(EdgeTTS)**로 시작 → VRAM 안정화 후 로컬 CosyVoice 전환

**다음 액션:** M1(vLLM 전환)부터 시작. 4090에 vLLM 컨테이너 띄우고 비전 응답 검증.

---

## 1. 프로젝트 개요

### 무엇을 만드는가
ESP32를 저비용 음성·영상 단말로 쓰는 AI 로봇. 음성 인식·언어 모델 추론·비전·
음성 합성은 전부 로컬 PC/서버에서 돌아가므로, 클라우드 AI 서비스 없이도
프라이빗하게 작동한다.

### 왜 만드는가
AA의 로봇 페르소나에 **눈(카메라)과 몸(하드웨어)**을 부여. 기존에 완성된
xiaozhi 기반 한국어 페르소나를 물리적 디바이스로 구현하는 것이 목적.

### 레퍼런스 로봇
- **xiaozhi (小智):** AI 챗봇 펌웨어 + 서버 (뇌 역할). MIT 라이선스.
- **Stack-chan (스택첸):** 팬틸트 서보 데스크 로봇 (몸 역할). M5Stack CoreS3 기반.
- 목표 = "Stack-chan 몸체 위에서 xiaozhi 뇌가 도는, 비전 달린 로봇"

---

## 2. 현재 저장소 상태 (M0 — 음성 전용 프로토타입)

### 코드네임
ALI-112 reference prototype

### 현재 아키텍처
```
I2S mic -> ESP32-S3 -> HTTP PCM upload -> local AI server
                                      -> faster-whisper STT
                                      -> Ollama chat model
                                      -> optional Piper TTS WAV
                                      -> ESP32-S3 I2S speaker
```

### 현재 하드웨어 (DIY)
- ESP32-S3 (PSRAM 필수)
- INMP441 I2S 마이크
- MAX98357A I2S 앰프
- 4Ω / 3W 스피커
- Push-to-talk 버튼
- 5V USB 전원

### 현재 디렉토리
- `firmware/` — PlatformIO Arduino 펌웨어 (ESP32-S3 음성 단말)
- `backend/` — FastAPI 로컬 AI 서버
- `docs/hardware.md` — 배선·보드 노트
- `docs/protocol.md` — 펌웨어↔백엔드 오디오 계약

### 현재 마일스톤 정의
첫 안정 마일스톤 = **Serial로 텍스트 응답**. 오디오 응답은 Piper 음성 모델
설치 후 `backend/.env`에 `PIPER_BIN`/`PIPER_MODEL` 설정 시 활성화.

### 현재 제약
- ESP32는 녹음·재생만 담당. LLM은 못 돌림.
- PSRAM 있는 ESP32-S3 필수. 일반 ESP32는 RAM 부족으로 녹음 길이 제한.
- 16kHz 모노 PCM, 4초 발화로 시작. 안정화 후 streaming/WebSocket으로 확장.

---

## 3. 확정된 기술 결정 (목표 상태)

> 아래는 설계 검토를 거쳐 확정된 결정. 번복하려면 HANDOFF에 근거를 남길 것.

### 3-1. 추론 엔진: vLLM (Ollama 탈락)
- **이유:** Ollama는 Qwen3.6가 쓰는 별도 mmproj 비전 파일을 지원하지 않음
  (2026-04 기준). 비전이 핵심인 이 프로젝트에서 Ollama는 부적합.
- 비전 가능 대안: llama.cpp, LM Studio, vLLM ≥0.19.0, SGLang ≥0.5.10
- **vLLM 선택 근거:** 단일 24GB 카드에서 비전 활성화 작동이 커뮤니티
  레시피로 검증됨(85 TPS, 125K 컨텍스트, VRAM 21.3GB). SGLang은 단일카드
  INT4 경로에 버그 이력이 있어 변수가 큼 → 확장 단계에서 재검토.

### 3-2. LLM + 비전: Qwen3.6-27B INT4 (통합)
- Qwen3.6-27B은 네이티브 멀티모달(비전 인코더 내장) → 대화와 비전을
  **단일 모델·단일 엔드포인트**로 처리. 별도 VLLM 모델 불필요.
- 권장 양자화: INT4 (AutoRound 또는 Unsloth UD-Q4_K_XL), 가중치 ~17.5GB.
- 비전 타워(MoonViT)는 BF16 유지 (~0.9GB).

### 3-3. STT: SenseVoice (한국어)
- 현재 코드의 faster-whisper도 동작하지만, 한국어 품질·속도 위해 SenseVoice 권장.
- xiaozhi-esp32-server에 내장 지원.

### 3-4. TTS: EdgeTTS (MVP) → CosyVoice 2 (확장)
- **MVP는 EdgeTTS(클라우드)로 시작.** 이유: 27B 비전 모델이 VRAM ~21GB를
  먹어, 로컬 CosyVoice(~2GB)까지 올리면 ~23GB로 OOM 위험.
- 추천 보이스: `ko-KR-InJoonNeural`(차분한 남성) 또는 `ko-KR-SunHiNeural`(여성).
- VRAM 안정화 또는 2번째 GPU 확보 후 로컬 CosyVoice 2로 전환.

### 3-5. 오케스트레이션: xiaozhi-esp32-server
- 커스텀 FastAPI 백엔드 → xiaozhi-esp32-server로 이관.
- WebSocket 기반. ESP32 접속 주소: `ws://<4090-IP>:8003/xiaozhi/v1/`
- **포트 주의:** vLLM이 8000을 점유하므로 xiaozhi WebSocket은 8003으로 노출.

### 3-6. 하드웨어: ESP32-S3 → StackChan
- 현재 DIY(INMP441+MAX98357A)도 유효하나, 카메라·팬틸트·표정을 한 번에
  얻으려면 StackChan 공식 키트(CoreS3 기반, 약 $99) 권장.
- StackChan은 카메라(0.3MP)·듀얼 마이크·스피커·팬틸트 서보 2개·터치
  디스플레이 내장. 화질이 아쉬우면 카메라만 추후 업그레이드.

---

## 4. 갭 분석 (M0 현재 → 목표)

| 영역 | 현재 (M0) | 목표 | 작업량 |
|------|----------|------|--------|
| LLM 엔진 | Ollama/llama3.2:3b | vLLM/Qwen3.6-27B | 중 (컨테이너 신규) |
| 비전 | 없음 | Qwen3.6 멀티모달 | 대 (펌웨어+서버 양쪽) |
| STT | faster-whisper | SenseVoice | 소 (모듈 교체) |
| TTS | Piper(옵션) | EdgeTTS→CosyVoice | 소 (설정) |
| 통신 | HTTP PCM | WebSocket | 중 (펌웨어 개편) |
| 백엔드 | 커스텀 FastAPI | xiaozhi-server | 대 (이관) |
| 하드웨어 | DIY ESP32-S3 | StackChan | 소 (구매·플래싱) |
| 카메라 캡처 | 없음 | ESP32 카메라 프레임 전송 | 대 (펌웨어 신규) |

**핵심 갭:** 비전 파이프라인 전체가 신규. ESP32가 카메라 프레임을 캡처해
서버로 전송하고, 서버가 Qwen3.6 멀티모달로 처리하는 경로가 0부터 필요.

---

## 5. 마일스톤 로드맵

### M0 — 음성 전용 프로토타입 ✅ (현재)
- 상태: Ollama 기반 음성 대화, Serial 텍스트 응답까지.

### M1 — vLLM 전환 (다음 시작점)
- [ ] 4090에 vLLM + Qwen3.6-27B INT4 컨테이너 기동
- [ ] `curl`로 비전 단독 테스트 (이미지 URL → 한국어 응답 확인)
- [ ] 기존 FastAPI 백엔드의 LLM 호출을 vLLM OpenAI 엔드포인트로 교체
- **완료 기준:** 텍스트 대화가 vLLM 경로로 한국어 응답
- **예상:** 반나절 (CUDA·양자화 호환 삽질 포함)

### M2 — xiaozhi-server 이관 + 비전 활성화
- [ ] xiaozhi-esp32-server를 docker-compose에 추가 (포트 8003)
- [ ] config.yaml에 페르소나·모듈(SenseVoice/EdgeTTS/Qwen3.6) 설정
- [ ] LLM과 VLLM을 동일 Qwen3.6 엔드포인트로 연결
- **완료 기준:** 서버 단에서 이미지+텍스트 멀티모달 응답
- **예상:** 1~2일

### M3 — StackChan 하드웨어 + 카메라 캡처
- [ ] StackChan 공식 키트 주문·수령
- [ ] xiaozhi 호환 펌웨어 플래싱 (카메라 지원 빌드)
- [ ] ESP32 카메라 프레임 → 서버 전송 경로 구현
- [ ] WebSocket 통신으로 전환 (HTTP PCM 폐기)
- **완료 기준:** 로봇이 카메라로 본 것을 한국어로 설명
- **예상:** 3~5일 (배송 대기 별도)

### M4 — 페르소나 완성 + Multica 통합
- [ ] 팬틸트 서보 "관찰 후 시선 회피" 제스처 구현
- [ ] 로봇을 Multica 운영 레이어에 노드로 등록
- [ ] 27-에이전트 시스템과 vLLM 엔드포인트 공유
- **완료 기준:** 로봇이 AA 페르소나로 작동 + 내부 워크플로우 연동
- **예상:** 1주+

---

## 6. 에이전트 태스크 분배 (제안)

> AA 27-에이전트 시스템에 분배할 경우의 역할 매핑. 실제 에이전트명에 맞게 조정.

| 영역 | 담당 에이전트 역할 | M1 | M2 | M3 | M4 |
|------|------------------|----|----|----|----|
| 인프라/Docker | DevOps 에이전트 | ● | ● | | ● |
| LLM/추론 | AI 백엔드 에이전트 | ● | ● | | ● |
| 펌웨어/ESP32 | 임베디드 에이전트 | | | ● | ● |
| 페르소나/프롬프트 | 카피·페르소나 에이전트 | | ● | | ● |
| 하드웨어/배선 | 하드웨어 에이전트 | | | ● | |
| 통합/QA | 테스트 에이전트 | ● | ● | ● | ● |

**병렬 가능:** M1(인프라/AI 백엔드)과 M3 하드웨어 주문(배송 대기)은 동시 진행 가능.
**직렬 의존:** M2는 M1 완료 후, M4는 M3 완료 후.

---

## 7. 핵심 레퍼런스 링크

- xiaozhi 펌웨어: https://github.com/78/xiaozhi-esp32
- xiaozhi 서버: https://github.com/xinnan-tech/xiaozhi-esp32-server
- StackChan (원본): https://github.com/meganetaaan/m5stack-avatar
- StackChan (M5Stack 공식): https://github.com/m5stack/StackChan
- StackChan 키트 구매: https://shop.m5stack.com/products/stackchan-kawaii-co-created-open-source-ai-desktop-robot
- Qwen3.6-27B 모델카드: https://huggingface.co/Qwen/Qwen3.6-27B
- vLLM 단일 4090 레시피(참고): https://github.com/noonghunna/qwen36-27b-single-3090

---

## 8. 리스크 & 주의사항

1. **VRAM 한계 (최우선 리스크)**
   27B 비전 + ASR로 ~21GB. 로컬 TTS 추가 시 OOM 위험. 모델 추가 전 항상
   예산 재계산. OOM 시 `--gpu-memory-utilization` 또는 `--max-model-len` 하향.

2. **vLLM CUDA graph 크래시 이력**
   초기 Qwen3.6 지원에 그래프 캡처 버그 있었음. 불안정 시 `--enforce-eager`
   추가(속도 손해, 안정성 확보). 최신 vLLM(≥0.19.0)이면 대개 불필요.

3. **모델 파일 무결성**
   2GB 이상 다운로드 후 크기만 믿지 말고 sha256 검증. 헛소리 출력의 첫
   의심은 모델이 아니라 파일 손상.

4. **레거시 코드 병행 유지**
   Ollama/whisper/piper 경로를 목표 스택 검증 전에 삭제하지 말 것.
   작동하는 것을 먼저 깨면 디버깅 기준점을 잃는다.

5. **포트 충돌**
   vLLM(8000) vs xiaozhi WebSocket(8003) 분리. 펌웨어 서버 주소도 8003으로.

6. **페르소나 일관성**
   불교 용어 직접 사용 금지. 한국어 출력 락. 간결한 한두 문장 응답 유지.

---

## 9. 인계 체크리스트

작업 인계받은 에이전트는 아래를 순서대로 확인:

- [ ] `CLAUDE.md` 읽고 프로젝트 정체성·규칙 파악
- [ ] 이 문서의 갭 분석·마일스톤 확인 → 현재 위치 파악
- [ ] 4090 PC 상태 확인 (Docker, GPU, WSL2 가용성)
- [ ] M1 시작: vLLM 컨테이너 기동 → 비전 curl 테스트
- [ ] 진행 상황을 이 문서 마일스톤 체크박스에 반영하며 커밋
