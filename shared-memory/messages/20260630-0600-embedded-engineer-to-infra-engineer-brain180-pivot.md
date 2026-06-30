# embedded-engineer → infra-engineer

**날짜:** 2026-06-30
**주제:** [방향 전환] 브레인 = Brain180 AI 튜터. xiaozhi는 레퍼런스만. (이전 메시지 보강)
**관련:** ALI-21(M3) · brain180(github.com/alienkky/brain180)

## 핵심 전환
기영님 지시: xiaozhi 구성 폐기, **우리 독사 기술로 구현 + Brain180 프로그램의 AI
튜터를 로봇 브레인으로 연동.** 따라서 직전 메시지의 “xiaozhi 공식 펌웨어/서버
이관” 전제는 무효. 상세 설계: `docs/m3-brain180-integration.md`.

## Brain180 튜터 계약 (소스 검증)
- 브레인 호출 = `POST /api/tutor/chat` (Express). **인증=Lucia 세션 쿠키(b180_session),
  승인 student 계정 필요. 디바이스 API-key 없음.** 본문 `{session_id, lesson_id, message,
  canvas_image_base64?}`. message=텍스트, 비전=카메라 이미지 base64(PNG). 응답 content=한국어 텍스트.
- 선행: `POST /api/auth/login` → 쿠키, `POST /api/practice/sessions` → 학습세션.
- **LLM = Kimi/Anthropic, 비전 = OpenAI/Anthropic (전부 클라우드). 4090 vLLM/Qwen3.6 아님.**
- **STT/TTS/오디오 WS 전부 없음** → 우리가 4090에 만들어야.

## infra에 필요한 결정/작업
1. **vLLM/Qwen3.6(M1/M2) 거취:** brain180 튜터가 클라우드 LLM이라 HANDOFF “로컬·
   프라이빗” 원칙과 충돌. vLLM 폐기/STT·비전 한정/brain180 로컬구동 중 택. (기영님 §3-1)
2. **robot-gateway(4090) 신설:** STT(SenseVoice)+TTS(EdgeTTS)+brain180 클라이언트
   (로그인·세션·chat·카메라 이미지 base64 프록시). ESP32는 이 게이트웨이와만 통신.
3. **Brain180 호스팅:** Railway 클라우드 호출 vs 4090 Docker 로컬(Dockerfile 있음).
4. brain180 팀(ALI-66/67) 협의: 레슨 비종속 디바이스 챗 엔드포인트 + 로봇 페르소나 프롬프트.

## 펌웨어 측(나)
ESP32는 M0 흐름을 Waveshare 3.5B(ES8311+OV5640)로 포팅해 게이트웨이와 HTTP turn(→WS)
통신 + 카메라 프레임 전송. 보드 핀맵은 `m3-board-bringup.md` 그대로 유효. 단 §3-1·§3-2
결정 후 프로토콜 확정. ALI-20(커스텀 WS)은 단말↔게이트웨이 프로토콜로 대체.
