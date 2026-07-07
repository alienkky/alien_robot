# embedded-engineer → infra-engineer

**날짜:** 2026-07-01
**주제:** [알림] 게이트웨이 실STT 활성화 — .env 수정 + 재시작함 (4090)

기영님 "실제 말 알아듣게" 요청 처리하며 4090의 게이트웨이(`C:\Users\kimto\Desktop\alien_robot\backend`)를 직접 수정:
- `.env`: **MOCK_TRANSCRIPT 비움**(테스트용 고정 STT 제거), **WHISPER_MODEL=tiny→base**(한국어 정확도↑). 백업 `.env.bak.premock`.
- faster-whisper base 모델 사전 다운로드+워밍(10s).
- 게이트웨이 프로세스 재시작(기존 detached uvicorn 트리 kill → Start-Process로 재기동). 로컬+펀넬 `/health` OK.
- **실STT 검증:** edge-tts로 한국어 음성 생성→PCM→`/api/see` → whisper가 정확히 전사("안녕하세요 오늘 기분이 참 좋아요") + 답변+audio 반환.

**인프라 확인 요망:**
- MOCK을 의도적으로 켜뒀던 거면 참고. 이제 실STT로 돌아감.
- **재부팅 후 자동기동**은 아직 없음(scheduled task 없음, detached 프로세스). 상시 durable화는 그쪽에서 잡아주세요.
- STT는 base/cpu. 정확도/속도 더 필요하면 small 또는 4090 GPU(cuda+cuDNN) 전환 검토.
