# M2 — xiaozhi-server 기동 런북

M1(vLLM `/v1/models` 정상) 완료 후 진행. xiaozhi-server를 vLLM과 동일 compose 프로젝트
(`alien_robot`) 네트워크에 띄워 `vllm:8000` 서비스 DNS로 Qwen3.6 엔드포인트를 재사용한다.

## 1. 포트 설계 (검증값)

| 역할 | 컨테이너 내부 | host 노출 |
|------|---------------|-----------|
| vLLM OpenAI API | 8000 | **8000** |
| xiaozhi WebSocket | 8000 | **8003** (`ws://<4090-IP>:8003/xiaozhi/v1/`) |
| xiaozhi OTA/비전 HTTP | 8003 | **8002** (`http://<4090-IP>:8002/xiaozhi/ota/`, `/mcp/vision/explain`) |

- vLLM이 host 8000을 점유하므로 xiaozhi WS는 컨테이너 8000 → host 8003.
- xiaozhi의 HTTP(OTA·비전 MCP)는 컨테이너 **8003**에서 돈다. 따라서 host 8002 노출은
  `"8002:8003"` 매핑이어야 한다(초기 `"8002:8002"`는 listener 없어 죽은 포트였음 → 수정).

## 2. 모델 파일 준비 (필수, 1회)

이미지의 `/opt/xiaozhi-esp32-server/models`에는 `silero_vad.onnx`와 SenseVoiceSmall
스캐폴딩이 번들돼 있지만, **빈 host 볼륨을 그 경로에 마운트하면 번들이 가려진다.**
또한 SenseVoice ASR 가중치 `model.pt`(~936MB)는 이미지에 미포함이라 별도 다운로드.

```bash
# 2-1. 번들 모델을 host 볼륨으로 복사 (silero_vad.onnx + SenseVoiceSmall 스캐폴딩)
docker run --rm -v E:/AlienRobot/asr_models:/host \
  --entrypoint sh ghcr.io/xinnan-tech/xiaozhi-esp32-server:server_latest \
  -c "cp -rn /opt/xiaozhi-esp32-server/models/. /host/"

# 2-2. SenseVoiceSmall ASR 가중치 다운로드 (~936MB)
curl -L -o E:/AlienRobot/asr_models/SenseVoiceSmall/model.pt \
  "https://huggingface.co/FunAudioLLM/SenseVoiceSmall/resolve/main/model.pt"
```

## 3. config.yaml 모듈 type 주의

- ASR 로컬 SenseVoice의 xiaozhi `type`은 **`fun_local`** (FunASR). `sense_voice`는
  sherpa_onnx_local의 `model_type` 값이지 ASR `type`이 아님 → 잘못 쓰면
  `不支持的ASR类型: sense_voice` 로 부팅 실패.
- VLLM `type: openai`는 `base_url` 또는 `url` 둘 다 인식(`base_url` 우선).

## 4. 기동 / 재기동

```bash
cd <repo>            # ./config.yaml 이 .config.yaml 로 마운트됨
# 최초 기동
docker compose -p alien_robot up -d xiaozhi-server
# config/compose 수정 후 재생성 — vLLM 재생성을 막으려면 반드시 --no-deps
docker compose -p alien_robot up -d --no-deps xiaozhi-server
```

> **함정:** `--no-deps` 없이 `up` 하면 depends_on 로 인해 vLLM 컨테이너가 recreate 되어
> 27B 모델을 처음부터 재로딩(~8분)한다. xiaozhi만 건드릴 땐 항상 `--no-deps`.

## 5. 검증

```bash
curl http://localhost:8000/v1/models                 # vLLM → qwen36 (200)
curl -o /dev/null -w "%{http_code}\n" http://localhost:8003/xiaozhi/ota/   # WS host (200)
curl -o /dev/null -w "%{http_code}\n" http://localhost:8002/xiaozhi/ota/   # OTA host (200)
curl -o /dev/null -w "%{http_code}\n" http://localhost:8002/mcp/vision/explain  # 비전 MCP (200)
# 컨테이너 → vLLM DNS
docker exec aa-xiaozhi-server python -c \
  "import urllib.request as u,json;print([m['id'] for m in json.load(u.urlopen('http://vllm:8000/v1/models'))['data']])"
```

부팅 로그 정상 신호: `초기화 컴포넌트 llm/intent/memory/vad/asr 成功`, `Websocket地址是 ...`.

## 6. 비전 한국어 quirk

이미지의 `core/providers/vllm/openai.py` `VLLMProvider.response()`가 질문에
`(请使用中文回复)`를 하드코딩 → `/mcp/vision/explain` raw 출력은 **중국어**. 최종 한국어는
대화 LLM(페르소나 프롬프트)이 비전 결과를 대화로 엮을 때 산출된다. 한국어 직출력이
필요하면 이미지 패치/오버라이드 필요(페르소나·통합 단계 검토 대상).
