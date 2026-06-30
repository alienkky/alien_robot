// ES8311 mono codec init — see es8311.h for the hardware-unverified caveat.
#include "es8311.h"

#include <Wire.h>

namespace {
// ES8311 register addresses (subset used by this driver).
constexpr uint8_t REG_RESET      = 0x00;
constexpr uint8_t REG_CLK_MANAGER = 0x01;  // 0x02..0x06 = clock dividers/coeff
constexpr uint8_t REG_SDPIN      = 0x09;    // serial data port: ADC→I2S in
constexpr uint8_t REG_SDPOUT     = 0x0A;    // serial data port: DAC←I2S out
constexpr uint8_t REG_SYSTEM_0B  = 0x0B;
constexpr uint8_t REG_SYSTEM_0C  = 0x0C;
constexpr uint8_t REG_SYSTEM_10  = 0x10;
constexpr uint8_t REG_SYSTEM_11  = 0x11;
constexpr uint8_t REG_ADC_17     = 0x17;    // ADC volume
constexpr uint8_t REG_DAC_32     = 0x32;    // DAC volume
constexpr uint8_t REG_ADC_16     = 0x16;    // ADC gain / PGA
constexpr uint8_t REG_GPIO_44    = 0x44;
constexpr uint8_t REG_CHIP_ID1   = 0xFD;    // expect 0x83
}  // namespace

bool ES8311::write(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(addr_);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}

bool ES8311::read(uint8_t reg, uint8_t &val) {
  Wire.beginTransmission(addr_);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(static_cast<int>(addr_), 1) != 1) return false;
  val = Wire.read();
  return true;
}

bool ES8311::begin(int sdaPin, int sclPin, uint8_t addr, uint32_t i2cFreq,
                   uint32_t sampleRate) {
  addr_ = addr;
  // The codec shares this I2C bus with the camera SCCB; init it once here.
  Wire.begin(sdaPin, sclPin, i2cFreq);

  // Sanity: probe chip id. Non-fatal (some clones differ) but logged.
  uint8_t id1 = 0;
  if (read(REG_CHIP_ID1, id1)) {
    Serial.printf("[es8311] chip id1=0x%02X (expect 0x83)\n", id1);
  } else {
    Serial.println("[es8311] WARN: no I2C ACK from codec");
  }

  // ── Init sequence (MCLK = 256×fs provided by ESP I2S) ───────────────
  // Mirrors espressif es8311 component defaults. Coeff regs (0x02-0x06,0x16)
  // assume the 256×fs ratio; verify against datasheet table on hardware.
  bool okv = true;
  okv &= write(REG_RESET, 0x1F);        // reset
  delay(20);
  okv &= write(REG_RESET, 0x00);        // release reset → CSM normal
  okv &= write(REG_CLK_MANAGER, 0x30);  // MCLK + BCLK enabled, from MCLK pin
  okv &= write(0x02, 0x00);             // clk div (256×fs path)
  okv &= write(0x03, 0x10);             // ADC osr
  okv &= write(0x04, 0x10);             // DAC osr
  okv &= write(0x05, 0x00);             // clk div (LRCK)
  okv &= write(REG_SYSTEM_0B, 0x00);
  okv &= write(REG_SYSTEM_0C, 0x00);
  okv &= write(REG_SYSTEM_10, 0x1F);    // power up vmid/ibias
  okv &= write(REG_SYSTEM_11, 0x7F);
  okv &= write(0x00, 0x80);             // power up chip state machine
  okv &= write(0x0D, 0x01);             // power up analog
  okv &= write(0x0E, 0x02);
  okv &= write(0x12, 0x00);
  okv &= write(0x13, 0x10);
  okv &= write(0x1C, 0x6A);             // ADC config
  okv &= write(0x37, 0x08);
  okv &= write(REG_SDPIN, 0x00);        // 16-bit I2S, ADC → I2S
  okv &= write(REG_SDPOUT, 0x00);       // 16-bit I2S, I2S → DAC
  okv &= write(REG_GPIO_44, 0x08);      // ADC→DAC loopback off
  (void)sampleRate;                     // fs encoded by I2S MCLK (256×fs)

  setAdcGain(6);                        // sensible default mic PGA
  setDacVolume(0xBF);                   // ≈ 0 dB

  ok_ = okv;
  Serial.printf("[es8311] init %s\n", ok_ ? "OK (compile-level)" : "FAILED (I2C)");
  return ok_;
}

void ES8311::setDacVolume(uint8_t vol) { write(REG_DAC_32, vol); }

void ES8311::setAdcGain(uint8_t gain) { write(REG_ADC_16, gain & 0x07); }
