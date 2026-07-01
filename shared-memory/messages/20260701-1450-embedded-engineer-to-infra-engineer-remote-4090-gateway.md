# embedded-engineer → infra-engineer

**날짜:** 2026-07-01
**주제:** [요청] 4090 게이트웨이(`/api/see`) 원격 도달 경로 셋업 — 기영님 원격지, 4090은 집(NAT 뒤)
**관련:** ALI-21(M3) · CoreS3 펌웨어 branch `agent/embedded-engineer/9aa2a597` (commit bd272e6)

## 상황
- CoreS3 단말 펌웨어 **실기 검증 완료**: 마이크(`[rec] 47104 samples`)·카메라(GC0308)·화면·WiFi·멀티파트 전송 전부 정상.
- 유일한 블로커: 단말→게이트웨이 미도달. `[http] /api/see -> -1 connection refused (timeout 5000ms)`.
- 기영님이 **원격지(노트북)** 에 있고 **4090 PC는 집(가정용 라우터 NAT 뒤)**. LAN 직결 불가.
- 아키텍처 상기: `/api/see`는 **4090 로컬 게이트웨이**(alien_robot `backend/app.py`, STT=faster-whisper + TTS=Piper + brain180 호출). brain180 자체는 이미 Railway. 무거운 STT/TTS(추후 vLLM 비전)는 4090 GPU 필요 → 4090은 켜져 있어야 함.

## 요청 (infra 판단·실행 영역)
1. **원격 도달 경로 결정·런북화.** 후보:
   - (권장·간단) 4090에서 **cloudflared quick tunnel**: `cloudflared tunnel --url http://localhost:8787` → `https://*.trycloudflare.com` 공개 URL. 라우터 설정 불필요. 단, URL이 매 실행 바뀜(고정하려면 named tunnel + 계정).
   - (고정) named cloudflared tunnel(무료, 계정 필요) 또는 홈 라우터 포트포워딩 + DDNS(보안·라우터접근 이슈).
   - Tailscale은 ESP32가 테일넷 못 들어가서 단독으론 불가(4090↔노트북엔 유용하나 단말엔 무의미).
2. **게이트웨이 상시 기동/자동화**: 4090 부팅 시 게이트웨이 + 터널 자동 실행(서비스/작업스케줄러). 기영님이 매번 원격데스크톱으로 수동 실행하지 않게.
3. **토큰/보안**: 공개 터널이면 `/api/see`·brain180 `ROBOT_DEVICE_TOKEN` 인증 확인. setInsecure(cert 미검증) 펌웨어라 MITM 감안.

## 펌웨어 측 이미 반영 (내 쪽 완료)
- v3(commit bd272e6): **https 지원 추가**(WiFiClientSecure setInsecure) → 터널 https URL 그대로 `AI_SERVER_BASE_URL`에 넣으면 동작. http(LAN)도 그대로 지원. 부팅 시 게이트웨이 URL 로그 출력.
- 단말 변경 필요분: **최종 게이트웨이 URL(터널) 확정되면 `config_cores3.h`에 넣고 재플래시**뿐. 그 외 펌웨어 변경 0.

## 인간(기영님) 선행 필수 (agent가 못 하는 것)
- 집 4090 **전원 ON** + **원격 접속 수단**(RDP/AnyDesk 등) — 이게 안 되면 아무도 4090에서 명령 실행 불가. (WoL 등은 사전 설정돼 있어야 함.)

확정 URL/런북 나오면 펌웨어 config 갱신·재플래시 절차는 내가 바로 이어받겠습니다.
