# M1 WSL Memory Checklist

This checklist is for the current Alien Robot M1 machine profile:

- System RAM: 48GB
- GPU: RTX 4090 24GB VRAM
- Runtime: Windows + WSL2 + Docker Desktop
- vLLM model: `Lorbus/Qwen3.6-27B-int4-AutoRound`

## Current Finding

The machine has 48GB RAM, but Docker currently reports this container limit:

```text
aa-vllm-qwen36 MEM LIMIT: 23.43GiB
```

That means vLLM is not allowed to use the full host RAM. For this model, that can block startup before the server becomes ready.

Observed state:

```text
curl http://localhost:8000/v1/models
=> curl: (52) Empty reply from server
```

GPU usage did increase to about 16GB, so the model started loading. The API is still not ready because vLLM has not finished initialization.

## Target WSL/Docker Memory

For 48GB host RAM, start with:

```text
WSL memory: 40GB
WSL swap: 64GB
Windows pagefile: system managed, or 64GB minimum if manually configured
```

Why:

- 40GB leaves about 8GB for Windows.
- 64GB swap gives vLLM room during model load and compilation.
- The RTX 4090 24GB VRAM is tight but usable with conservative vLLM settings.

## Suggested `.wslconfig`

Create or update this file on Windows:

```text
C:\Users\<your-user>\.wslconfig
```

Suggested content:

```ini
[wsl2]
memory=40GB
swap=64GB
pageReporting=false
localhostForwarding=true
```

After changing it, restart WSL and Docker:

```powershell
wsl --shutdown
```

Then reopen Docker Desktop.

## Retest Order

Run vLLM only first:

```powershell
docker compose up -d vllm
docker logs -f aa-vllm-qwen36
```

In another terminal:

```powershell
nvidia-smi
docker stats aa-vllm-qwen36 --no-stream
curl.exe -sS http://localhost:8000/v1/models
```

Do not start `xiaozhi-server` until `/v1/models` returns JSON.

## Expected Ready Signal

The first success condition is:

```text
curl http://localhost:8000/v1/models
```

returns a JSON response containing `qwen36`.

Only after that should M1 continue to:

1. text chat completion smoke test
2. vision/image smoke test
3. FastAPI `LLM_PROVIDER=vllm` validation
