# M3 — 독사(자체) 스택 + Brain180 AI 튜터 연동 설계

**작성:** embedded-engineer (도하람) · 2026-06-30
**지시(기영님):** xiaozhi 구성은 **참조만**. 우리 독사 기술로 만들고, **Brain180
프로그램의 AI 튜터**를 로봇의 브레인으로 붙인다.
**Brain180 저장소:** https://github.com/alienkky/brain180
**관련 이슈:** ALI-21 (M3) · brain180 측 ALI-66(프롬프트)/ALI-67(API·인증)

> 방향 전환: 이전 결론(“xiaozhi 공식 펌웨어를 그대로 빌드/플래시”)은 **폐기**.
> 보드 하드웨어 분석(`m3-board-bringup.md` §1·§2 핀맵)은 여전히 유효 — 재사용한다.
> 단 펌웨어·서버 스택은 xiaozhi를 **레퍼런스로만** 보고 자체 구현.

---

## 1. Brain180 AI 튜터 — 검증된 실제 계약 (소스 확인)

Brain180은 React(Vite) + **Express/Node** + Postgres(Drizzle) + Lucia 인증의
웹 학습 앱이다. AI 튜터는 서버 라우트로 노출돼 있다.

### 로봇이 브레인을 호출하는 3-스텝 경로
| 단계 | 엔드포인트 | 비고 |
|---|---|---|
| 1. 로그인 | `POST /api/auth/login` (email/pw) | Lucia 세션 **쿠키 `b180_session`** 발급 (TTL 720h) |
| 2. 세션 생성 | `POST /api/practice/sessions` | 학습 세션 row 1개 생성 (lesson에 묶임) |
| 3. 대화 | `POST /api/tutor/chat` | 튜터 응답 텍스트 반환 |

### `POST /api/tutor/chat` 상세 (핵심)
- **인증 필수:** `requireAuth → requirePasswordFresh → requireApprovedUser`.
  즉 **승인된 student 계정의 세션 쿠키**가 있어야 한다. (디바이스용 API-key 경로 **없음**.)
- **요청 본문:** `{ session_id, lesson_id, message, canvas_mode?, canvas_snapshot?, canvas_image_base64? }`
  - `session_id` = 1단계의 learningSession, `lesson_id` = 그 세션의 레슨.
  - `message` = **텍스트**(=STT 결과를 우리가 넣어야 함).
  - **`canvas_image_base64` (PNG)** = 비전 입력. → **카메라 프레임을 여기에 base64로 넣으면
    튜터가 이미지를 본다.** (별도 비전 모델 경로 불필요.)
- **응답:** `{ id, session_id, role:"assistant", content, model, input_tokens, output_tokens, ... }`
  → `content` 가 **튜터의 한국어 텍스트 답**. (=TTS로 우리가 음성화해야 함.)
- **LLM 제공자:** env `AI_PROVIDER` = **kimi(Moonshot, 기본) / anthropic**, 비전은
  **OpenAI(gpt-4.1-mini) / Anthropic**. → **전부 클라우드 LLM. 4090 vLLM/Qwen3.6 아님.**

### Brain180이 **제공하지 않는 것** (로봇에 필요한데 없는 것)
- ❌ STT(음성→텍스트), ❌ TTS(텍스트→음성), ❌ 오디오 WebSocket/스트리밍.
- ❌ 디바이스(비브라우저) 인증 토큰. (쿠키 기반뿐.)
- 튜터는 **레슨에 묶인** 코칭 봇이다 — 무상태 범용 어시스턴트가 아님.

---

## 2. 독사 아키텍처 (제안) — xiaozhi 레퍼런스, brain180 브레인

핵심 아이디어: **ESP32는 “귀·눈·입”만 하는 단순 단말**로 두고, **4090의 자체
게이트웨이**가 STT/TTS와 brain180 호출(인증·세션·비전)을 전담한다. ESP32가
직접 쿠키 인증·레슨 관리를 하지 않으므로 펌웨어가 가벼워지고 xiaozhi 의존 0.

```text
[ESP32-S3 Waveshare 3.5B · 자체 펌웨어]
  I2S mic(ES8311) ─ 음성 ──┐         ┌── 음성 응답 → I2S spk(ES8311)
  OV5640 ─ JPEG 프레임 ─────┤         │
                           ▼         │
                 [robot-gateway · 4090 · 자체 서비스]
                   1) STT (SenseVoice/whisper)  : 음성 → 텍스트
                   2) brain180 클라이언트:
                        - 부팅 시 /api/auth/login → b180_session 쿠키 보관
                        - 고정 디바이스 learningSession 확보
                        - POST /api/tutor/chat { message:STT텍스트,
                                                 canvas_image_base64:카메라JPEG→PNG }
                   3) 응답 content(한국어) → TTS (EdgeTTS) → 오디오
                           │
                           ▼
                 [Brain180 (Node/Express) = 브레인 / AI 튜터]
                   LLM: Kimi/Anthropic · 비전: OpenAI/Anthropic (클라우드)
```

- **단말↔게이트웨이 프로토콜:** 우리가 정의(독사). M0의 `POST /api/turn`(HTTP PCM)
  계약을 그대로 재사용해 시작 → 안정화 후 WebSocket 스트리밍. (xiaozhi WS 설계를
  참조하되 우리 코드.) ESP32 펌웨어는 M0 `main.cpp` 흐름을 이 보드(ES8311/카메라)로
  포팅 — 이게 “2순위(자체 포팅)” 경로이고, 이제 이게 정식 경로가 된다.
- **게이트웨이가 쿠키·레슨·비전 base64 변환을 흡수** → ESP32는 인증/레슨 무지(無知).
- **비전:** 카메라 JPEG → (게이트웨이에서 PNG로) → `canvas_image_base64` → 튜터 비전.

---

## 3. 미해결 결정 (기영님 확인 필요) — 이게 막혀서 펌웨어 확정 못 함

1. **브레인이 brain180 클라우드 튜터면 → 4090 vLLM/Qwen3.6(M1/M2)은 어떻게?**
   brain180 튜터는 Kimi/Anthropic/OpenAI(클라우드)로 돈다. HANDOFF의 “로컬 vLLM
   Qwen3.6 · **클라우드 의존 없이 프라이빗**” 원칙과 정면 충돌.
   → (a) vLLM 폐기하고 brain180 클라우드 LLM 채택? (b) vLLM은 STT/비전만? (c) brain180을
   4090 로컬 인스턴스로 띄우되 LLM만 클라우드? **프라이버시 트레이드오프 결정 필요.**
2. **로봇을 어느 “레슨”에 붙이나?** 튜터는 `lesson_id`+학습세션이 필수다. 로봇 대화
   전용 레슨/페르소나를 brain180에 하나 만들까, 아니면 brain180에 **레슨 비종속
   디바이스 엔드포인트**(예: `/api/tutor/device-chat`)를 신설할까? (후자가 깔끔, brain180 작업 필요.)
3. **디바이스 인증:** 게이트웨이가 service-account 쿠키로 프록시(권장, brain180 변경 0)
   vs brain180에 디바이스 API-key 엔드포인트 신설(ALI-67 작업)?
4. **Brain180 호스팅:** Railway(클라우드) 그대로 호출 vs 4090에 Docker로 로컬 구동
   (Dockerfile 있음 → 가능, 프라이빗 유지에 유리)?
5. **STT/TTS:** 게이트웨이에 SenseVoice(STT) + EdgeTTS(TTS) 자체 구현 확정?
   (brain180엔 없음. infra/AI-backend 협업 영역.)

---

## 4. 작업 분배 (제안)

- **embedded (나):** ESP32 자체 펌웨어 — M0 흐름을 Waveshare 3.5B(ES8311 코덱 +
  OV5640 카메라)로 포팅, 게이트웨이와 HTTP turn(→WS) 통신, 카메라 프레임 캡처·전송.
  **단, §3-1·§3-2 결정 후 핀/프로토콜 확정.**
- **infra (ALI-19 등):** robot-gateway(4090) — STT/TTS + brain180 클라이언트(로그인·
  세션·chat·비전 base64). brain180 호스팅(로컬 vs Railway) 결정·구동.
- **brain180 팀(ALI-66/67):** (필요 시) 레슨 비종속 디바이스 챗 엔드포인트 + 로봇
  페르소나 시스템 프롬프트 + 디바이스 인증.

---

## 4.5 구현 상태 (2026-06-30 — 1차 연동 완료, Railway/Kimi 기준)

기영님 결정(직접 판단해 구현, 우선순위=연동, Railway 우선, STT/TTS는 효율적으로)에
따라 **brain180↔로봇 연동의 코어를 구현**했다. 결정 처리:
- **vLLM/Qwen3.6:** 지금은 brain180 기본값(Kimi)로 둔다. 프라이빗 전환은 brain180
  `AI_PROVIDER` env 교체만으로 됨(게이트웨이/펌웨어 변경 0). → **나중에 스위치.**
- **레슨 종속:** brain180에 **레슨 비종속 디바이스 엔드포인트 `/api/robot/chat`** 신설로 제거.
- **STT/TTS(효율 우선):** 기존 M0 백엔드의 **faster-whisper(STT)+Piper(TTS, 장치용 WAV
  정규화)** 를 그대로 재사용. (EdgeTTS 전환은 추후 옵션. 새 의존성·트랜스코드 회피.)

### 무엇을 만들었나
**A. Brain180 (브랜치 `feat/robot-bridge-ali21`)**
- `POST /api/robot/chat` — 베어러 토큰(`ROBOT_DEVICE_TOKEN`) 인증, 무상태(게이트웨이가
  `history` 전달), **Alien Robot 페르소나**(간결 한국어), 텍스트 + 카메라 이미지 비전.
  기존 LLM 시드(Kimi/Anthropic)·비전 래퍼 재사용 → vLLM 전환은 env 교체.
- `GET /api/robot/health` 준비성 프로브. `RobotChatBody` 검증, `/api/robot`만 8MB 바디(카메라).
- `npm run smoke:robot` 스모크. **추가 전용** — 토큰 미설정 시 503(기존 배포 무영향). `tsc`/`eslint` 통과.

**B. Alien Robot 게이트웨이 (`backend/app.py` 확장)**
- `LLM_PROVIDER=brain180` 추가 → `ask_brain180()` 가 `/api/robot/chat` 호출(히스토리·이미지).
- **`/api/turn`(M0 프로토콜) 그대로** → **기존 M0 펌웨어는 서버 IP만 게이트웨이로 바꾸면 무수정 동작.**
- 신규 `POST /api/see`(멀티파트: PCM/텍스트 + JPEG) → 카메라 비전 턴. `POST /api/reset`(대화 메모리 초기화).
- 롤링 대화 메모리(최근 N턴), `config.example.env`에 brain180 변수.
- **검증:** brain180 스텁으로 종단 통합 테스트 통과(턴 전달·베어러·히스토리 누적·이미지 첨부·리셋).

### 실행 절차 (4090, 기영님/infra)
```bash
# 1) Brain180 (Railway 또는 4090 로컬). 로컬이면:
cd brain180 && npm ci
#   .env: DATABASE_URL(Postgres), KIMI_API_KEY 등 + ROBOT_DEVICE_TOKEN=<랜덤 비밀>
npm run dev:server                       # :3001
ROBOT_DEVICE_TOKEN=<...> npm run smoke:robot   # 연동 확인

# 2) 게이트웨이(M0 백엔드)
cd alien_robot/backend && pip install -r requirements.txt
cp config.example.env .env
#   .env: LLM_PROVIDER=brain180, BRAIN180_BASE_URL=<railway-url 또는 http://127.0.0.1:3001>,
#         BRAIN180_DEVICE_TOKEN=<위와 동일 토큰>, WHISPER_*; (음성 출력 원하면) PIPER_BIN/PIPER_MODEL
uvicorn app:app --host 0.0.0.0 --port 8787

# 3) ESP32(M0 펌웨어): config.h 의 서버 IP를 게이트웨이(4090:8787)로. POST /api/turn 그대로.
```

### 남은 일
- **펌웨어 카메라 경로:** Waveshare 3.5B(ES8311+OV5640)로 M0 흐름 포팅 + `/api/see` 멀티파트
  전송 = **실물 보드 확보 후** (핀맵 `m3-board-bringup.md` 유효). 그전엔 M0 DIY 보드가 음성 단말 스탠드인.
- **프라이빗 전환:** brain180 `AI_PROVIDER`를 4090 vLLM/Qwen3.6로(인프라).
- 라이브 STT/LLM/TTS 실호출은 4090(모델·키·DB 필요). 본 작업은 빌드·종단 스텁 통합까지 검증.

## 5. 재사용/폐기 정리
- ✅ 재사용: 보드 핀맵·하드웨어 분석(`m3-board-bringup.md` §1·§2), M0 `firmware/main.cpp` 흐름.
- ✅ 레퍼런스만: xiaozhi-esp32 펌웨어/서버(프로토콜·비전 MCP 아이디어 참고).
- ⛔ 폐기 후보: xiaozhi 공식 펌웨어 직접 빌드/플래시, xiaozhi-esp32-server 이관(ALI 관련),
  ALI-20 커스텀 WS(우리 단말↔게이트웨이 프로토콜로 대체).
