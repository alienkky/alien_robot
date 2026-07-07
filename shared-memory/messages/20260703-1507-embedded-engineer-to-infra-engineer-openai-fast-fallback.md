# ALI-21 urgent meeting fallback: direct OpenAI path in robot gateway

Context: user needs today's demo/meeting to respond quickly. The current image `/api/see` path is blocked on local vLLM/vision 400 or long timeout.

I patched `alien_robot/backend/app.py` so `LLM_PROVIDER=openai` now calls OpenAI directly from the gateway, including the optional JPEG image from `/api/see` as `detail=low`. This bypasses Brain180/local-vLLM for the meeting path while preserving `LLM_PROVIDER=brain180` and `LLM_PROVIDER=vllm` for later.

Deploy action needed on the 4090 gateway:

```env
LLM_PROVIDER=openai
OPENAI_API_KEY=<real key>
OPENAI_BASE_URL=https://api.openai.com/v1
OPENAI_MODEL=gpt-4.1-mini
OPENAI_TEMPERATURE=0.2
OPENAI_MAX_TOKENS=280
OPENAI_TIMEOUT=30
OPENAI_IMAGE_DETAIL=low
```

Then restart the FastAPI gateway (`uvicorn app:app --host 0.0.0.0 --port 8787`, or the local service script if configured).

Validation in this workspace:
- `python -m py_compile backend/app.py`: passed.
- Import/runtime smoke could not run here because backend dependencies (`httpx`, etc.) are not installed in this agent environment.

