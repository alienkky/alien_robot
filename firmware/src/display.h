#pragma once
// ─────────────────────────────────────────────────────────────────────
// Display bring-up for Waveshare ESP32-S3-Touch-LCD-3.5B (AXS15231B, QSPI).
// Compiled only in the `waveshare-s3-touch-lcd-35b` PlatformIO env.
//
// STAGE 1 (this file): initialize the panel + backlight and draw a boot
// screen / status text. ASCII only — a Korean speech bubble (Hangul font)
// and JPEG photo preview come in later stages.
//
// ⚠️ HARDWARE-UNVERIFIED: panel pins are from docs/m3-board-bringup.md
// (CS12/CLK5/D0-3=1,2,3,4, backlight GPIO6). Uses the Arduino_GFX
// AXS15231B QSPI driver. Build-verify with `pio run`; the panel actually
// lighting up must be confirmed on the real board (first flash-test).
// ─────────────────────────────────────────────────────────────────────

#include <Arduino.h>

// Init QSPI bus + AXS15231B panel + backlight. Returns false on begin() fail.
// Non-fatal: the caller continues the voice loop even if this returns false.
bool display_begin();

// Draw the boot screen (color bars prove the panel; title + hint text).
void display_boot();

// Replace the bottom status line (e.g. Wi-Fi IP, "recording...", errors).
// Safe to call before display_begin(); it just no-ops if the panel is absent.
void display_status(const char *text);
