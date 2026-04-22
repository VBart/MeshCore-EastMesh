#include "TBeam1WBoard.h"

namespace {
constexpr float kFanTempOnC = 48.0f;
constexpr float kFanTempOffC = 42.0f;
constexpr uint32_t kFanControlCheckMs = 5000;
constexpr uint32_t kFanPostTxHoldMs = 30000;
constexpr float kNtcSeriesResistorOhms = 10000.0f;
constexpr float kNtcBeta = 3950.0f;
constexpr float kNtcRoomTempKelvin = 298.15f;
constexpr float kNtcRoomResistanceOhms = 10000.0f;
constexpr float kNtcVrefVolts = 3.3f;
constexpr uint32_t kMinFanPostTxHoldMs = 0;
constexpr uint32_t kMaxFanPostTxHoldMs = 600000;
}

void TBeam1WBoard::begin() {
  ESP32Board::begin();

  // Power on radio module (must be done before radio init)
  pinMode(SX126X_POWER_EN, OUTPUT);
  digitalWrite(SX126X_POWER_EN, HIGH);
  radio_powered = true;
  delay(10);  // Allow radio to power up

  // RF switch RXEN pin handled by RadioLib via setRfSwitchPins()

  // Initialize LED
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  // Initialize fan control (on by default - 1W PA can overheat)
  pinMode(FAN_CTRL_PIN, OUTPUT);
  setFanEnabled(true);
  fan_force_on_until = millis() + fan_post_tx_hold_ms;
  next_fan_control_check_at = millis() + kFanControlCheckMs;
}

void TBeam1WBoard::onBeforeTransmit() {
  // RF switching handled by RadioLib via SX126X_DIO2_AS_RF_SWITCH and setRfSwitchPins()
  digitalWrite(LED_PIN, HIGH);  // TX LED on
  if (fan_mode == FanMode::Auto) {
    fan_force_on_until = millis() + fan_post_tx_hold_ms;
    setFanEnabled(true);
  }
}

void TBeam1WBoard::onAfterTransmit() {
  digitalWrite(LED_PIN, LOW);   // TX LED off
  if (fan_mode == FanMode::Auto) {
    fan_force_on_until = millis() + fan_post_tx_hold_ms;
  }
}

uint16_t TBeam1WBoard::getBattMilliVolts() {
  // T-Beam 1W uses 7.4V battery with voltage divider
  // ADC reads through divider - adjust multiplier based on actual divider ratio
  analogReadResolution(12);
  uint32_t raw = 0;
  for (int i = 0; i < 8; i++) {
    raw += analogRead(BATTERY_PIN);
  }
  raw = raw / 8;
  // Assuming voltage divider ratio from ADC_MULTIPLIER
  // 3.3V reference, 12-bit ADC (4095 max)
  return static_cast<uint16_t>((raw * 3300 * ADC_MULTIPLIER) / 4095);
}

uint16_t TBeam1WBoard::getBatteryMinMilliVolts() const {
  return 6000;
}

uint16_t TBeam1WBoard::getBatteryMaxMilliVolts() const {
  return 8400;
}

const char* TBeam1WBoard::getManufacturerName() const {
  return "LilyGo T-Beam 1W";
}

void TBeam1WBoard::powerOff() {
  // Turn off radio LNA (CTRL pin must be LOW when not receiving)
  digitalWrite(SX126X_RXEN, LOW);

  // Turn off radio power
  digitalWrite(SX126X_POWER_EN, LOW);
  radio_powered = false;

  // Turn off LED and fan
  digitalWrite(LED_PIN, LOW);
  setFanEnabled(false);

  ESP32Board::powerOff();
}

void TBeam1WBoard::setFanEnabled(bool enabled) {
  fan_enabled = enabled;
  digitalWrite(FAN_CTRL_PIN, enabled ? HIGH : LOW);
}

bool TBeam1WBoard::isFanEnabled() const {
  return fan_enabled;
}

void TBeam1WBoard::setFanMode(FanMode mode) {
  fan_mode = mode;
  next_fan_control_check_at = 0;
  updateFanControl(true);
}

TBeam1WBoard::FanMode TBeam1WBoard::getFanMode() const {
  return fan_mode;
}

const char* TBeam1WBoard::getFanModeName() const {
  switch (fan_mode) {
    case FanMode::On:
      return "on";
    case FanMode::Off:
      return "off";
    case FanMode::Auto:
    default:
      return "auto";
  }
}

float TBeam1WBoard::getLastBoardTemperatureC() const {
  return last_board_temperature_c;
}

bool TBeam1WBoard::setFanPostTxHoldMs(uint32_t hold_ms) {
  if (hold_ms < kMinFanPostTxHoldMs || hold_ms > kMaxFanPostTxHoldMs) {
    return false;
  }
  fan_post_tx_hold_ms = hold_ms;
  return true;
}

uint32_t TBeam1WBoard::getFanPostTxHoldMs() const {
  return fan_post_tx_hold_ms;
}

float TBeam1WBoard::readBoardTemperatureC() const {
  const float millivolts = analogReadMilliVolts(NTC_PIN);
  if (millivolts <= 0.0f || millivolts >= (kNtcVrefVolts * 1000.0f)) {
    return NAN;
  }

  const float voltage = millivolts / 1000.0f;
  const float resistance = kNtcSeriesResistorOhms * ((kNtcVrefVolts / voltage) - 1.0f);
  if (!(resistance > 0.0f)) {
    return NAN;
  }

  const float kelvin = 1.0f / ((log(resistance / kNtcRoomResistanceOhms) / kNtcBeta) + (1.0f / kNtcRoomTempKelvin));
  return kelvin - 273.15f;
}

void TBeam1WBoard::updateFanControl(bool force) {
  const uint32_t now = millis();
  if (!force && now < next_fan_control_check_at) {
    return;
  }
  next_fan_control_check_at = now + kFanControlCheckMs;

  if (fan_mode == FanMode::On) {
    setFanEnabled(true);
    return;
  }
  if (fan_mode == FanMode::Off) {
    setFanEnabled(false);
    return;
  }

  if (now < fan_force_on_until) {
    setFanEnabled(true);
    return;
  }

  const float temperature = readBoardTemperatureC();
  last_board_temperature_c = temperature;
  if (isnan(temperature)) {
    // Keep the fan on if temperature sensing is unavailable.
    setFanEnabled(true);
    return;
  }

  if (temperature >= kFanTempOnC) {
    setFanEnabled(true);
  } else if (temperature <= kFanTempOffC) {
    setFanEnabled(false);
  }
}
