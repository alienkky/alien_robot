# embedded-engineer → infra-engineer

**날짜:** 2026-07-02
**주제:** [알림] 게이트웨이 이미지 분석을 로컬 4090 Qwen(vLLM)로 전환함 + 브랜치 파편화 주의

기영님 "로컬 모델로 돌려서 확인" 요청 처리:
- 로컬 vLLM Qwen3.6가 **비전 지원 확인**(멀티모달). `localhost:8000` `qwen36`(Qwen3.6-27B-int4).
- 게이트웨이(`C:\Users\kimto\Desktop\alien_robot\backend\app.py`)에 **`ask_vllm_vision()` 추가** + `/api/see`가 `LLM_PROVIDER=vllm`이면 로컬 Qwen으로 라우팅. Qwen3.6 추론모델이라 `chat_template_kwargs.enable_thinking=false` + max_tokens 768로 content 비지 않게.
- `.env`: **LLM_PROVIDER=brain180 → vllm** 전환(gitignored). 게이트웨이 재시작(detached), 로컬+펀넬 /health OK.
- **실측:** `/api/see`(이미지) → Qwen "화면에는 테스트 패턴이 보입니다." 정답 + audio, **~10.3s**(펀넬). 클라우드 미경유.

**⚠️ 브랜치 파편화 — 정리 필요:**
- 실행 게이트웨이 = **Desktop\alien_robot** = 브랜치 `agent/embedded-engineer/4d2c69d9`(app.py vLLM 커밋 `8f29aaf` push함). launcher = `start-gateway.cmd`.
- 펌웨어 정본 = 브랜치 `9aa2a597`(내 작업). E:\alien_robot도 9aa2a597.
- app.py의 edge_tts/TTS_ENGINE/see는 4d2c69d9에만 있음. **9aa2a597과 4d2c69d9 백엔드 통합 필요.**

**되돌리기:** `.env` LLM_PROVIDER=vllm→brain180 + 재시작이면 클라우드 복귀. 튜터/노드는 brain180 경로(ALI-23).
**재부팅 자동기동:** 아직 없음(detached). durable화 그쪽에서.
