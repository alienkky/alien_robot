from __future__ import annotations

import base64
import hashlib
import os
import subprocess
import tempfile
import wave
from collections import deque
from pathlib import Path
from typing import Any

import httpx
import numpy as np
from dotenv import load_dotenv
from fastapi import FastAPI, File, Form, HTTPException, Request, UploadFile
from fastapi.responses import FileResponse
from scipy import signal
from scipy.io import wavfile

load_dotenv()

APP_DIR = Path(__file__).resolve().parent
ARTIFACT_DIR = APP_DIR / "artifacts"
ARTIFACT_DIR.mkdir(exist_ok=True)

SAMPLE_RATE = 16_000
CHANNELS = 1
SAMPLE_WIDTH_BYTES = 2

app = FastAPI(title="ESP32 Local AI Dialogue Server")
_whisper_model: Any | None = None


def env(name: str, default: str = "") -> str:
    return os.getenv(name, default).strip()


def write_pcm_as_wav(pcm: bytes, path: Path) -> None:
    with wave.open(str(path), "wb") as wav:
        wav.setnchannels(CHANNELS)
        wav.setsampwidth(SAMPLE_WIDTH_BYTES)
        wav.setframerate(SAMPLE_RATE)
        wav.writeframes(pcm)


def get_whisper_model() -> Any:
    global _whisper_model
    if _whisper_model is None:
        from faster_whisper import WhisperModel

        _whisper_model = WhisperModel(
            env("WHISPER_MODEL", "tiny"),
            device=env("WHISPER_DEVICE", "cpu"),
            compute_type=env("WHISPER_COMPUTE_TYPE", "int8"),
        )
    return _whisper_model


def transcribe_pcm(pcm: bytes) -> str:
    mock_transcript = env("MOCK_TRANSCRIPT")
    if mock_transcript:
        return mock_transcript

    with tempfile.NamedTemporaryFile(suffix=".wav", delete=False) as tmp:
        tmp_path = Path(tmp.name)
    try:
        write_pcm_as_wav(pcm, tmp_path)
        segments, _info = get_whisper_model().transcribe(
            str(tmp_path),
            vad_filter=True,
            language="ko",
        )
        transcript = " ".join(segment.text.strip() for segment in segments).strip()
        if not transcript:
            raise HTTPException(status_code=422, detail="No speech detected")
        return transcript
    finally:
        tmp_path.unlink(missing_ok=True)


async def ask_ollama(transcript: str) -> str:
    payload = {
        "model": env("OLLAMA_MODEL", "llama3.2:3b"),
        "stream": False,
        "messages": [
            {"role": "system", "content": env("SYSTEM_PROMPT")},
            {"role": "user", "content": transcript},
        ],
    }
    url = f"{env('OLLAMA_URL', 'http://127.0.0.1:11434').rstrip('/')}/api/chat"
    try:
        async with httpx.AsyncClient(timeout=120) as client:
            response = await client.post(url, json=payload)
            response.raise_for_status()
    except httpx.HTTPError as exc:
        raise HTTPException(status_code=502, detail=f"Ollama request failed: {exc}") from exc

    data = response.json()
    answer = data.get("message", {}).get("content", "").strip()
    if not answer:
        raise HTTPException(status_code=502, detail="Ollama returned an empty answer")
    return answer


async def ask_openai_compatible(transcript: str) -> str:
    payload = {
        "model": env("VLLM_MODEL", "qwen36"),
        "messages": [
            {"role": "system", "content": env("SYSTEM_PROMPT")},
            {"role": "user", "content": transcript},
        ],
        "temperature": float(env("LLM_TEMPERATURE", "0.7")),
        "max_tokens": int(env("LLM_MAX_TOKENS", "256")),
    }
    base_url = env("VLLM_BASE_URL", "http://127.0.0.1:8000/v1").rstrip("/")
    headers = {"Authorization": f"Bearer {env('VLLM_API_KEY', 'EMPTY')}"}
    try:
        async with httpx.AsyncClient(timeout=120) as client:
            response = await client.post(
                f"{base_url}/chat/completions",
                json=payload,
                headers=headers,
            )
            response.raise_for_status()
    except httpx.HTTPError as exc:
        raise HTTPException(status_code=502, detail=f"vLLM request failed: {exc}") from exc

    data = response.json()
    choices = data.get("choices", [])
    answer = choices[0].get("message", {}).get("content", "").strip() if choices else ""
    if not answer:
        raise HTTPException(status_code=502, detail="vLLM returned an empty answer")
    return answer


# ── Brain180 robot bridge (ALI-21) ──────────────────────────────────
# The brain is the Brain180 program's AI tutor, reached over its device route
# POST /api/robot/chat (bearer-token, stateless). This gateway owns the short
# conversation memory and forwards it as `history` each turn, plus an optional
# camera frame (base64 JPEG) for vision. Swapping Brain180's own LLM from Kimi
# to the local 4090 vLLM (Qwen3.6) is a Brain180-side env change — invisible here.

# Rolling dialogue memory for the single desk robot. Each entry is a
# {"role": "user"|"assistant", "content": str} dict in Brain180's history shape.
_robot_history: deque[dict[str, str]] = deque(
    maxlen=max(2, int(env("ROBOT_HISTORY_TURNS", "6")) * 2)
)


def reset_robot_history() -> None:
    _robot_history.clear()


async def ask_brain180(transcript: str, image_b64: str | None = None) -> str:
    base_url = env("BRAIN180_BASE_URL", "http://127.0.0.1:3001").rstrip("/")
    token = env("BRAIN180_DEVICE_TOKEN")
    if not token:
        raise HTTPException(
            status_code=500,
            detail="BRAIN180_DEVICE_TOKEN not configured (set it to the server's ROBOT_DEVICE_TOKEN)",
        )

    payload: dict[str, Any] = {
        "message": transcript,
        "history": list(_robot_history),
    }
    if image_b64:
        payload["image_base64"] = image_b64

    headers = {"Authorization": f"Bearer {token}"}
    timeout = float(env("BRAIN180_TIMEOUT", "120"))
    try:
        async with httpx.AsyncClient(timeout=timeout) as client:
            response = await client.post(
                f"{base_url}/api/robot/chat", json=payload, headers=headers
            )
            response.raise_for_status()
    except httpx.HTTPStatusError as exc:
        # Surface Brain180's structured error code (e.g. upstream_error) to logs.
        detail = exc.response.text[:300] if exc.response is not None else str(exc)
        raise HTTPException(status_code=502, detail=f"Brain180 request failed: {detail}") from exc
    except httpx.HTTPError as exc:
        raise HTTPException(status_code=502, detail=f"Brain180 unreachable: {exc}") from exc

    data = response.json()
    answer = (data.get("data", {}) or {}).get("text", "").strip()
    if not answer:
        raise HTTPException(status_code=502, detail="Brain180 returned an empty answer")

    # Commit this turn to memory only after a successful answer.
    _robot_history.append({"role": "user", "content": transcript})
    _robot_history.append({"role": "assistant", "content": answer})
    return answer


async def ask_llm(transcript: str, image_b64: str | None = None) -> str:
    provider = env("LLM_PROVIDER", "ollama").lower()
    if provider == "brain180":
        return await ask_brain180(transcript, image_b64)
    if provider == "ollama":
        return await ask_ollama(transcript)
    if provider in {"vllm", "openai"}:
        return await ask_openai_compatible(transcript)
    raise HTTPException(status_code=500, detail=f"Unsupported LLM_PROVIDER: {provider}")


def synthesize_with_piper(text: str, key: str) -> str | None:
    piper_bin = env("PIPER_BIN")
    piper_model = env("PIPER_MODEL")
    if not piper_bin or not piper_model:
        return None

    output_path = ARTIFACT_DIR / f"{key}.wav"
    command = [piper_bin, "--model", piper_model, "--output_file", str(output_path)]
    speaker = env("PIPER_SPEAKER")
    if speaker:
        command.extend(["--speaker", speaker])

    try:
        subprocess.run(
            command,
            input=text,
            text=True,
            check=True,
            capture_output=True,
            timeout=60,
        )
    except (subprocess.SubprocessError, OSError) as exc:
        raise HTTPException(status_code=500, detail=f"Piper TTS failed: {exc}") from exc

    normalize_wav_for_device(output_path)
    return f"/audio/{output_path.name}"


def normalize_wav_for_device(path: Path) -> None:
    source_rate, data = wavfile.read(path)
    if data.ndim > 1:
        data = data.mean(axis=1)

    if source_rate != SAMPLE_RATE:
        gcd = np.gcd(source_rate, SAMPLE_RATE)
        data = signal.resample_poly(data, SAMPLE_RATE // gcd, source_rate // gcd)

    if data.dtype != np.int16:
        data = data.astype(np.float32)
        peak = float(np.max(np.abs(data))) if data.size else 0.0
        if peak > 0:
            data = data / peak
        data = (data * 0.9 * np.iinfo(np.int16).max).astype(np.int16)

    wavfile.write(path, SAMPLE_RATE, data)


@app.get("/health")
async def health() -> dict[str, str]:
    return {"status": "ok"}


@app.post("/api/turn")
async def turn(request: Request) -> dict[str, str | None]:
    pcm = await request.body()
    if len(pcm) < SAMPLE_RATE * SAMPLE_WIDTH_BYTES // 2:
        raise HTTPException(status_code=400, detail="PCM body is too short")
    if len(pcm) % SAMPLE_WIDTH_BYTES != 0:
        raise HTTPException(status_code=400, detail="PCM body must be 16-bit aligned")

    transcript = transcribe_pcm(pcm)
    answer = await ask_llm(transcript)
    key = hashlib.sha256(f"{transcript}\n{answer}".encode("utf-8")).hexdigest()[:16]
    audio_url = synthesize_with_piper(answer, key)
    return {"transcript": transcript, "answer": answer, "audio_url": audio_url}


@app.post("/api/see")
async def see(
    audio: UploadFile = File(..., description="raw 16kHz mono s16le PCM"),
    image: UploadFile | None = File(None, description="JPEG camera frame"),
    text: str | None = Form(None, description="optional text instead of audio STT"),
) -> dict[str, str | None]:
    """Vision turn for the camera firmware: PCM (or text) + a JPEG frame.

    Mirrors /api/turn but multipart, so the ESP32-S3 (Waveshare 3.5B + OV5640)
    can attach what the camera sees. Requires LLM_PROVIDER=brain180 (only the
    Brain180 bridge forwards images to a vision model).
    """
    if env("LLM_PROVIDER", "ollama").lower() != "brain180":
        raise HTTPException(status_code=400, detail="/api/see requires LLM_PROVIDER=brain180")

    if text and text.strip():
        transcript = text.strip()
    else:
        pcm = await audio.read()
        if len(pcm) < SAMPLE_RATE * SAMPLE_WIDTH_BYTES // 2:
            raise HTTPException(status_code=400, detail="PCM body is too short")
        if len(pcm) % SAMPLE_WIDTH_BYTES != 0:
            raise HTTPException(status_code=400, detail="PCM body must be 16-bit aligned")
        transcript = transcribe_pcm(pcm)

    image_b64: str | None = None
    if image is not None:
        raw = await image.read()
        if raw:
            image_b64 = base64.b64encode(raw).decode("ascii")

    answer = await ask_brain180(transcript, image_b64)
    key = hashlib.sha256(f"{transcript}\n{answer}".encode("utf-8")).hexdigest()[:16]
    audio_url = synthesize_with_piper(answer, key)
    return {"transcript": transcript, "answer": answer, "audio_url": audio_url}


@app.post("/api/reset")
async def reset() -> dict[str, str]:
    """Clear the robot's rolling conversation memory (new dialogue)."""
    reset_robot_history()
    return {"status": "ok"}


@app.get("/audio/{filename}")
async def audio(filename: str) -> FileResponse:
    path = ARTIFACT_DIR / filename
    if not path.exists() or path.suffix.lower() != ".wav":
        raise HTTPException(status_code=404, detail="Audio not found")
    return FileResponse(path, media_type="audio/wav")


@app.post("/api/debug/sine")
async def debug_sine() -> dict[str, str]:
    seconds = 1.0
    t = np.linspace(0, seconds, int(SAMPLE_RATE * seconds), endpoint=False)
    samples = (np.sin(2 * np.pi * 440 * t) * 0.2 * np.iinfo(np.int16).max).astype(np.int16)
    path = ARTIFACT_DIR / "debug-440hz.wav"
    wavfile.write(path, SAMPLE_RATE, samples)
    return {"audio_url": f"/audio/{path.name}"}
