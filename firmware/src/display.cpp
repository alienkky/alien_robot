// AXS15231B QSPI display bring-up — see display.h for the hardware-unverified
// caveat. Stage 1: panel init, backlight, boot screen, status line.
#include "display.h"

#include <Arduino_GFX_Library.h>

namespace {
// Panel pins (docs/m3-board-bringup.md — AXS15231B QSPI on this board).
constexpr int PIN_LCD_CS = 12;
constexpr int PIN_LCD_CLK = 5;
constexpr int PIN_LCD_D0 = 1;
constexpr int PIN_LCD_D1 = 2;
constexpr int PIN_LCD_D2 = 3;
constexpr int PIN_LCD_D3 = 4;
constexpr int PIN_LCD_BL = 6;   // backlight enable (active high)

// Native panel is 320(w) x 480(h). rotation 1 → 480x320 landscape.
constexpr int kRotation = 1;

Arduino_DataBus *bus = nullptr;
Arduino_GFX *gfx = nullptr;
bool ready = false;

// Redraw the bottom status bar with the given text.
void drawStatusBar(const char *text) {
  if (!ready) return;
  const int16_t w = gfx->width();
  const int16_t h = gfx->height();
  gfx->fillRect(0, h - 40, w, 40, BLACK);
  gfx->setTextColor(WHITE);
  gfx->setTextSize(2);
  gfx->setCursor(10, h - 30);
  gfx->print(text);
}
}  // namespace

bool display_begin() {
  // Backlight on first so a blank panel is at least visibly lit.
  pinMode(PIN_LCD_BL, OUTPUT);
  digitalWrite(PIN_LCD_BL, HIGH);

  bus = new Arduino_ESP32QSPI(PIN_LCD_CS, PIN_LCD_CLK, PIN_LCD_D0, PIN_LCD_D1,
                              PIN_LCD_D2, PIN_LCD_D3);
  // (bus, RST=none, rotation, IPS=true, width=320, height=480)
  gfx = new Arduino_AXS15231B(bus, GFX_NOT_DEFINED, kRotation, true, 320, 480);

  if (!gfx->begin()) {
    Serial.println("[lcd] begin failed (QSPI/panel)");
    ready = false;
    return false;
  }
  ready = true;
  gfx->fillScreen(BLACK);
  Serial.println("[lcd] init OK");
  return true;
}

void display_boot() {
  if (!ready) return;
  const int16_t w = gfx->width();
  gfx->fillScreen(BLACK);

  // Color bars — if these show, the panel + QSPI + backlight are all good.
  gfx->fillRect(0, 0, w, 24, RED);
  gfx->fillRect(0, 24, w, 24, GREEN);
  gfx->fillRect(0, 48, w, 24, BLUE);

  gfx->setTextColor(WHITE);
  gfx->setTextSize(3);
  gfx->setCursor(12, 100);
  gfx->print("ALIEN ROBOT");

  gfx->setTextColor(CYAN);
  gfx->setTextSize(2);
  gfx->setCursor(12, 145);
  gfx->print("display OK");

  drawStatusBar("booting...");
}

void display_status(const char *text) { drawStatusBar(text); }
