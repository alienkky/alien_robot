# M1 vLLM Runbook

이 문서는 M1 단계에서 vLLM/Qwen3.6 서버가 실제로 준비됐는지 확인하는 절차다.

## 현재 목표

M1의 목표는 ESP32 펌웨어가 아니라 4090 로컬 서버 쪽 검증이다.

순서는 다음과 같다.

1. vLLM 컨테이너가 Qwen3.6 모델을 끝까지 로딩한다.
2. `http://localhost:8000/v1/models`가 정상 JSON을 반환한다.
3. `/v1/chat/completions`로 텍스트 답변을 받는다.
4. 이미지 입력까지 한 번 curl로 확인한다.
5. 그 다음 FastAPI 백엔드의 `LLM_PROVIDER=vllm` 경로를 연결한다.

## 왜 `curl: (52) Empty reply from server`가 나는가

이 에러는 포트는 열려 있지만 vLLM API 서버가 아직 요청을 처리할 준비가 안 됐다는 뜻이다.

이번 확인에서 컨테이너 로그는 `Starting to load model Lorbus/Qwen3.6-27B-int4-AutoRound...` 이후 준비 완료 로그까지 가지 못했다. 즉, 모델 서버가 살아는 있지만 모델 로딩이 끝나지 않은 상태다.

## 확인 명령

```powershell
docker ps --format "table {{.Names}}\t{{.Status}}\t{{.Ports}}"
docker logs --tail 120 aa-vllm-qwen36
nvidia-smi
curl.exe -sS http://localhost:8000/v1/models
```

정상 준비 상태라면 로그에 API 서버 시작 완료 메시지가 나오고, `curl`은 `qwen36` 모델 목록을 JSON으로 반환해야 한다.

## 4090/WSL 권장 시작값

Qwen3.6-27B INT4는 24GB VRAM에서 여유가 크지 않다. 그래서 처음에는 다음처럼 보수적으로 시작한다.

```text
--max-model-len 8192
--gpu-memory-utilization 0.74
--cpu-offload-gb 4
```

의미는 간단하다.

- `max-model-len 8192`: 한 번에 처리할 문맥 길이를 줄여 메모리를 아낀다.
- `gpu-memory-utilization 0.74`: 4090 VRAM을 너무 꽉 채우지 않는다.
- `cpu-offload-gb 4`: 일부를 시스템 메모리로 넘겨 VRAM 부족을 피한다.

이 값으로 성공하면 그 다음에 문맥 길이와 GPU 사용률을 조금씩 올린다.

## 현재 관찰된 병목

`nvidia-smi`에서는 GPU 메모리가 거의 사용되지 않았다. 반면 Docker CLI에서 `페이징 파일이 너무 작습니다` 오류가 발생했다.

따라서 지금 1차 의심 지점은 GPU VRAM 부족이 아니라 Windows/WSL 쪽 시스템 메모리 또는 페이지파일 부족이다. 모델이 GPU로 올라가기 전에 CPU/디스크/가상 메모리 단계에서 멈추는 흐름으로 보인다.

## 다음 조치

1. Windows 페이지파일을 충분히 키운다.
2. Docker Desktop/WSL을 재시작한다.
3. `docker compose up -d vllm`로 vLLM만 먼저 띄운다.
4. `/v1/models`가 정상 응답할 때까지 xiaozhi-server는 붙이지 않는다.
5. vLLM 텍스트 응답이 확인된 뒤 FastAPI와 xiaozhi-server를 순서대로 연결한다.
