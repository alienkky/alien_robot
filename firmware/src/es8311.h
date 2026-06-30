#pragma once
// Minimal ES8311 mono codec driver (I2C control) for the Waveshare
// ESP32-S3-Touch-LCD-3.5B. Compiled only in the `waveshare-s3-touch-lcd-3.5b`
// PlatformIO env. I2S data transport is configured separately in main_waveshare.cpp.
//
// ⚠️ HARDWARE-UNVERIFIED: the register init below mirrors the widely used
// espressif es8311 component defaults for MCLK = 256 × fs (4.096 MHz @ 16 kHz).
// The clock-coefficient registers (0x02/0x03/0x04/0x16) depend on the exact
// MCLK/fs ratio and MUST be confirmed against the ES8311 datasheet coefficient
// table on real hardware (scope BCLK/WS, listen for clean playback). Build
// passing here means it compiles, NOT that audio is correct on the board.

#include <Arduino.h>

class ES8311 {
 public:
  // sdaPin/sclPin/addr per docs board profile; sampleRate fixed to mic/spk rate.
  bool begin(int sdaPin, int sclPin, uint8_t addr, uint32_t i2cFreq, uint32_t sampleRate);
  void setDacVolume(uint8_t vol);   // 0..255 (0xBF ≈ 0 dB)
  void setAdcGain(uint8_t gain);    // 0..7 PGA steps
  bool ok() const { return ok_; }

 private:
  bool write(uint8_t reg, uint8_t val);
  bool read(uint8_t reg, uint8_t &val);

  uint8_t addr_ = 0x18;
  bool ok_ = false;
};
