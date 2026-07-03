## 2026-07-03 - ALI-21 post-422 touch freeze missed
- Earlier response treated the post-422 path as non-freezing based on health logs before the user's explicit report that touch froze after the "could not hear you" message.
- Missed code path: after `handleTurn()`, `while (touchPressed())` waited forever for release when touch state stayed stale-pressed after camera/I2C churn. The loop fed the watchdog, so it could look like a permanent touch freeze instead of rebooting.
- Correction in v28: local low-speech gate before camera/server, bounded post-turn release wait, stale-pressed ignore, and periodic I2C recovery until release is observed.
