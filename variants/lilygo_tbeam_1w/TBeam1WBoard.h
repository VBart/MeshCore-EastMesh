#pragma once

#include <Arduino.h>
#include <helpers/ESP32Board.h>
#include "variant.h"

// LilyGo T-Beam 1W with SX1262 + external PA (XY16P35 module)
//
// Power architecture (LDO is separate chip on T-Beam board, not inside XY16P35):
//
//   VCC (+4.0~+8.0V) ──┬──────────────────► XY16P35 VCC pin 5 (PA direct)
//   (USB or Battery)   │
//                      │   ┌───────────┐
//                      └──►│ LDO Chip  │──► +3.3V ──► XY16P35 (SX1262 + LNA)
//                          │ EN=GPIO40 │
//                          └───────────┘
//                      LDO_EN (GPIO 40): H @ +1.2V~VIN, active high, not floating
//
// Control signals:
//   - LDO_EN (GPIO 40): HIGH enables LDO → powers SX1262 + LNA
//   - TCXO_EN (DIO3):   HIGH enables TCXO (set to 1.8V per Meshtastic)
//   - CTL (GPIO 21):    HIGH=RX (LNA on), LOW=TX (LNA off)
//   - DIO2:             AUTO via SX126X_DIO2_AS_RF_SWITCH (TX path)
//
// Power notes:
//   - PA needs VCC 4.0-8.0V for full 32dBm output
//   - USB-C (3.9-6V) marginal; 7.4V battery recommended
//   - Battery must support 2A+ discharge for high-power TX

class TBeam1WBoard : public ESP32Board {
public:
  enum class FanMode : uint8_t {
    Auto = 0,
    On,
    Off
  };

private:
  bool radio_powered = false;
  bool fan_enabled = true;
  uint32_t fan_force_on_until = 0;
  uint32_t next_fan_control_check_at = 0;
  uint32_t fan_post_tx_hold_ms = 30000;
  FanMode fan_mode = FanMode::Auto;
  float last_board_temperature_c = NAN;
  float readBoardTemperatureC() const;

public:
  void begin();
  void onBeforeTransmit() override;
  void onAfterTransmit() override;
  uint16_t getBattMilliVolts() override;
  uint16_t getBatteryMinMilliVolts() const override;
  uint16_t getBatteryMaxMilliVolts() const override;
  const char* getManufacturerName() const override;
  void powerOff() override;

  // Fan control methods
  void setFanEnabled(bool enabled);
  bool isFanEnabled() const;
  void setFanMode(FanMode mode);
  FanMode getFanMode() const;
  const char* getFanModeName() const;
  float getLastBoardTemperatureC() const;
  bool setFanPostTxHoldMs(uint32_t hold_ms);
  uint32_t getFanPostTxHoldMs() const;
  void updateFanControl(bool force = false);
};
