# embedded-engineer → infra-engineer

**날짜:** 2026-06-30
**주제:** [구현완료] Brain180↔로봇 연동 코어 빌드. 4090 배포/프라이빗 전환 조율.
**관련:** ALI-21(M3) · brain180 PR 브랜치 `feat/robot-bridge-ali21`

## 무엇을 만들었나 (기영님 "직접 판단해 구현" 지시)
1. **Brain180** (`feat/robot-bridge-ali21`): `POST /api/robot/chat` 신설 — 베어러 토큰
   (`ROBOT_DEVICE_TOKEN`) 인증, 레슨/세션 비종속·무상태, 로봇 페르소나, 텍스트+카메라 비전.
   기존 Kimi/Anthropic 시드 재사용. 추가 전용(토큰 미설정시 503). tsc/eslint 통과.
2. **게이트웨이**(alien_robot `backend/app.py`): `LLM_PROVIDER=brain180` 추가. `/api/turn`은
   M0 그대로(펌웨어 무수정). 신규 `/api/see`(카메라 멀티파트)·`/api/reset`. STT=faster-whisper,
   TTS=Piper 재사용. 스텁 종단 통합테스트 통과(전달·히스토리·이미지·리셋).

## infra 액션
1. **호스팅:** brain180 Railway 배포 env에 `ROBOT_DEVICE_TOKEN=<랜덤비밀>` 추가(또는 4090 로컬
   Docker 구동). 게이트웨이 `.env`의 `BRAIN180_BASE_URL`+`BRAIN180_DEVICE_TOKEN` 동일 토큰.
2. **프라이빗 전환(추후):** brain180 `AI_PROVIDER`를 Kimi→4090 vLLM/Qwen3.6(OpenAI 호환)로.
   게이트웨이/펌웨어 변경 0. vLLM(M1/M2) 작업은 이 스위치용으로 살려두면 됨.
3. 실행 절차 상세: `docs/m3-brain180-integration.md` §4.5.

## 펌웨어(나) 다음
실물 Waveshare 3.5B 확보 후 M0 흐름 포팅(ES8311+OV5640) + `/api/see` 전송. 그전엔 M0 DIY 보드가 음성 스탠드인.
