# embedded-engineer → infra-engineer

**날짜:** 2026-06-30
**주제:** M3 실보드 = xiaozhi 공식 펌웨어 → ALI-20 재검토 + 서버 비전 설정 요청
**관련:** ALI-21 (M3), ALI-19 (xiaozhi-server), ALI-20 (커스텀 WebSocket)

## 결론 (소스 검증됨)
기영님 보유 올인원 보드 = Waveshare **ESP32-S3-Touch-LCD-3.5B** (Spotpear 리브랜드).
xiaozhi-esp32 공식 펌웨어에 **이 보드 프로파일(`esp32-s3-touch-lcd-3.5b`)이 이미
존재**하고, 카메라(OV5640/OV2640)·디스플레이·ES8311 오디오·터치 핀이 전부 정의됨.
→ 펌웨어 핀포팅 불필요, 카메라 캡처 코드 신규 작성 불필요. 상세: `docs/m3-board-bringup.md`.

## 요청 1 — ALI-20 재검토
xiaozhi 공식 펌웨어가 **WebSocket(오디오) + MCP + 카메라 비전**을 전부 제공.
우리가 커스텀 WebSocket 프로토콜을 새로 구현할 이유가 사라짐.
→ **ALI-20 은 cancel 또는 scope 축소 후보.** 인프라 관점 판단 부탁.

## 요청 2 — xiaozhi-server(ALI-19) 비전 설정
펌웨어 비전 경로: 음성 의도 → LLM이 MCP `self.camera.take_photo` 호출 → ESP32가
JPEG 캡처 → 서버 비전(Explain) 엔드포인트 → 한국어 응답. 서버 측에서 필요:
- `config.yaml` 의 **VLLM(비전) 프로바이더 = 우리 Qwen3.6-27B vLLM** (OpenAI 호환,
  4090:8000). LLM·VLLM 동일 멀티모달 엔드포인트.
- **WebSocket = `ws://<4090-IP>:8003/xiaozhi/v1/`** 노출 (펌웨어 기본 경로와 일치 확인).
- 페르소나/한국어 락 = 서버 시스템 프롬프트.

## 펌웨어 측 상태
보드 프로파일·핀·카메라 컨피그까지 소스 검증 완료. 실제 `idf.py build`(ESP-IDF) 와
USB 플래싱·시리얼 검증은 장비/툴체인 확보 후 기영님. 빌드 통과 ≠ 실기 동작으로 구분 보고.
