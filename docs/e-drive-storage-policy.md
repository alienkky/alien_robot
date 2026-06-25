# E Drive Storage Policy

All large Alien Robot runtime data must be stored on the E drive.

Default root:

```text
E:\AlienRobot
```

## Directory Layout

```text
E:\AlienRobot\
  hf_cache\      # HuggingFace model cache used by vLLM
  data\          # xiaozhi-server runtime data
  asr_models\    # ASR model files
  wsl-swap\      # WSL swap VHDX
  repos\         # local repo checkouts or mirrors
```

## Docker Compose

`docker-compose.yml` uses this data root by default:

```text
E:/AlienRobot
```

You can override it with:

```powershell
$env:ALIEN_ROBOT_DATA_ROOT="E:/AlienRobot"
docker compose up -d vllm
```

The important mounts are:

```text
E:/AlienRobot/hf_cache   -> /root/.cache/huggingface
E:/AlienRobot/data       -> /opt/xiaozhi-esp32-server/data
E:/AlienRobot/asr_models -> /opt/xiaozhi-esp32-server/models
```

## WSL Config

Windows requires `.wslconfig` to live under the user profile:

```text
C:\Users\kimto\.wslconfig
```

That file should point the swap file to E:

```ini
[wsl2]
memory=32GB
swap=64GB
swapFile=E:\\AlienRobot\\wsl-swap\\swap.vhdx
pageReporting=false
localhostForwarding=true
```

## Note

Do not store downloaded model weights under the repo or the C drive workspace.
The repo should contain code and docs only; runtime data belongs under `E:\AlienRobot`.
