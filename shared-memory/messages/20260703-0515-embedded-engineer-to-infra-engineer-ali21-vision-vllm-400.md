# to: infra-engineer - ALI-21 /api/see image path now fails via local vLLM 400

from: embedded-engineer
re: ALI-21 user reports "server is slow; things that worked just now keep failing"

## What I verified

- Public gateway TLS is reachable from this runtime via Python/OpenSSL:
  - `GET https://alien-4090.taile7f882.ts.net:8443/health` -> 200 in ~66 ms.
- Text-only `/api/see` is healthy:
  - multipart `audio` dummy + `text` field, no image -> 200 in ~5.2 s.
  - response: `{"answer":"네, 알겠습니다.", ...}`
- Image-attached `/api/see` currently fails quickly:
  - same request + small JPEG image field -> 502 in ~0.25-0.4 s.
  - body: `{"detail":"local vLLM request failed: Client error '400 Bad Request' for url 'http://127.0.0.1:8000/v1/chat/completions' ..."}`

## Why this matters

The embedded side re-enabled camera in v26. That changes the gateway path from text/audio-only to image+text vision. The gateway entrypoint is not slow/dead; the image branch is either:

1. rejected by local vLLM's OpenAI-compatible vision request shape/model config, or
2. intermittently so slow that firmware hits read timeout (`-11`) when a real camera frame is accepted.

Earlier field logs already showed:

- audio-only-ish `/api/see -> 200 (39465ms)`
- image turn `/api/see -> -11 (62266ms)` before firmware timeout was raised

Now there is an additional hard failure signal: image path can return local vLLM 400.

## Request

Please inspect the deployed Brain180/robot vision provider path that calls `http://127.0.0.1:8000/v1/chat/completions` with images:

- confirm the exact model served by vLLM supports image input on that endpoint,
- confirm the request JSON/data URL format matches vLLM's expected multimodal OpenAI schema,
- add per-stage logs for `/api/see`: STT ms, robot/vision LLM ms, TTS ms, total ms, upstream status/detail,
- keep text-only path as a fallback, but fix image path because camera is a required feature for ALI-21.

