#include "senseair_sunrise.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"
#include <cstring>
#include <vector>
#include "esphome/core/application.h"
#include "esphome/core/preferences.h"

namespace esphome {
namespace senseair_sunrise {

static const char *const TAG = "senseair_sunrise";

bool SenseairSunriseComponent::wake_up_() {
  // Address-only probe: START-address-STOP per TDE5531 wake-up spec.
  // NACK is expected and harmless if sensor was asleep.
  this->write(nullptr, 0);
  delay(1);
  return true;
}

bool SenseairSunriseComponent::read_register_(uint8_t reg, uint8_t *data, size_t len) {
  this->wake_up_();
  i2c::ErrorCode err = this->write_read(&reg, 1, data, len);
  if (err != i2c::ERROR_OK) {
    ESP_LOGW(TAG, "Read register 0x%02X failed: %d", reg, err);
    return false;
  }
  return true;
}

bool SenseairSunriseComponent::write_register_(uint8_t reg, const uint8_t *data, size_t len) {
  this->wake_up_();
  std::vector<uint8_t> buf(len + 1);
  buf[0] = reg;
  std::memcpy(&buf[1], data, len);
  i2c::ErrorCode err = this->write(buf.data(), buf.size());
  if (err != i2c::ERROR_OK) {
    ESP_LOGW(TAG, "Write register 0x%02X failed: %d", reg, err);
    return false;
  }
  return true;
}

bool SenseairSunriseComponent::trigger_single_measurement_() {
  // Write 0x01 to register 0xC3 to start a single measurement
  uint8_t cmd = 0x01;
  if (!this->write_register_(0xC3, &cmd, 1)) {
    ESP_LOGE(TAG, "Failed to trigger single measurement");
    return false;
  }

  // TDE7318 lists T_Start typ 35 ms and T_Sample max 300 ms per sample for
  // Sunrise 006-0-0007. Add a generous margin for communication and CO2
  // post-processing before the result registers are ready.
  uint32_t timeout_ms = 35 + (static_cast<uint32_t>(this->number_of_samples_) * 300) + 1500;

  if (this->nrdy_pin_ != nullptr && this->nrdy_enabled_) {
    bool active_level = this->nrdy_active_high_;
    bool saw_active = false;
    uint32_t start = millis();

    while (millis() - start <= timeout_ms) {
      bool pin_state = this->nrdy_pin_->digital_read();
      if (pin_state == active_level) {
        saw_active = true;
      } else if (saw_active) {
        return true;
      }
      delay(10);
    }

    ESP_LOGW(TAG, "nRDY timeout after %ums (enabled=%s, active=%s, saw_active=%s); falling back to status polling",
             timeout_ms, this->nrdy_enabled_ ? "yes" : "no", this->nrdy_active_high_ ? "HIGH" : "LOW",
             saw_active ? "yes" : "no");
  } else {
    // If nRDY is not wired or is disabled in the sensor, let the measurement
    // progress before starting I2C polling.
    delay(timeout_ms);
  }

  uint32_t start = millis();
  uint8_t last_status[2] = {0xFF, 0xFF};
  while (millis() - start <= 1000) {
    uint8_t status[2];
    if (this->read_register_(0x00, status, 2)) {
      last_status[0] = status[0];
      last_status[1] = status[1];
      // LSB bit 7: No measurement completed yet
      if ((status[1] & 0x80) == 0) {
        return true;
      }
    }
    delay(20);
  }

  ESP_LOGE(TAG, "Timeout waiting for single measurement to complete (last ErrorStatus=0x%02X%02X)",
           last_status[0], last_status[1]);
  return false;
}

uint32_t SenseairSunriseComponent::compute_config_hash_() const {
  uint32_t h = 0;
  h = (h * 31) + this->measurement_mode_;
  h = (h * 31) + (this->iir_filter_ ? 1 : 0);
  h = (h * 31) + (this->pressure_compensation_ ? 1 : 0);
  return h;
}

uint32_t SenseairSunriseComponent::compute_state_crc_(const SunriseSavedState &state) const {
  uint32_t crc = 0xFFFFFFFF;
  auto feed = [&](uint8_t byte) {
    crc ^= byte;
    for (int i = 0; i < 8; i++)
      crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1));
  };
  for (size_t i = 0; i < sizeof(state.block); i++)
    feed(state.block[i]);
  for (int i = 0; i < 4; i++)
    feed((state.config_hash >> (i * 8)) & 0xFF);
  return crc ^ 0xFFFFFFFF;
}

void SenseairSunriseComponent::setup() {
  ESP_LOGCONFIG(TAG, "Setting up Senseair Sunrise...");
  bool needs_reset = false;

  // Initialize preference storage for single-measurement state preservation
  if (this->measurement_mode_ == 1) {
    this->state_pref_ = global_preferences->make_preference<SunriseSavedState>(fnv1_hash("senseair_sunrise_state"));
    uint32_t expected_config = this->compute_config_hash_();
    if (this->state_pref_.load(&this->saved_state_) &&
        this->saved_state_.config_hash == expected_config &&
        this->saved_state_.crc == this->compute_state_crc_(this->saved_state_)) {
      this->state_valid_ = true;
      ESP_LOGD(TAG, "Loaded saved sensor state from preferences");
    } else {
      this->state_valid_ = false;
      ESP_LOGD(TAG, "No valid saved sensor state (first cycle, config change, or corrupt)");
    }
  }

  if (this->nrdy_pin_ != nullptr) {
    this->nrdy_pin_->setup();
  }

  // Read firmware revision (registers 0x38-0x39)
  uint8_t fw_data[2];
  if (this->read_register_(0x38, fw_data, 2)) {
    ESP_LOGI(TAG, "Firmware version: %d.%d", fw_data[0], fw_data[1]);
  } else {
    ESP_LOGE(TAG, "Failed to communicate with Senseair Sunrise");
    this->mark_failed();
    return;
  }

  // Read firmware type (register 0x2F, uint8)
  uint8_t fw_type;
  if (this->read_register_(0x2F, &fw_type, 1)) {
    ESP_LOGI(TAG, "Firmware type: %u", fw_type);
  }

  // Read sensor ID (registers 0x3A-0x3D)
  uint8_t id_data[4];
  if (this->read_register_(0x3A, id_data, 4)) {
    uint32_t sensor_id = ((uint32_t) id_data[0] << 24) | ((uint32_t) id_data[1] << 16) |
                         ((uint32_t) id_data[2] << 8) | id_data[3];
    ESP_LOGI(TAG, "Sensor ID: %u", sensor_id);
  }

  // Read product code (registers 0x70-0x7F, 16-byte ASCII string, requires firmware 4.08+)
  if (fw_data[0] > 4 || (fw_data[0] == 4 && fw_data[1] >= 8)) {
    uint8_t product_code[16];
    if (this->read_register_(0x70, product_code, 16)) {
      char product_str[17];
      memcpy(product_str, product_code, 16);
      product_str[16] = '\0';
      ESP_LOGI(TAG, "Product code: %s", product_str);
    }
  }

  // Check and sync measurement mode with config
  uint8_t meas_mode;
  if (this->read_register_(0x95, &meas_mode, 1)) {
    if (meas_mode != this->measurement_mode_) {
      ESP_LOGW(TAG, "Sensor measurement mode (%d) differs from configured mode (%d), updating sensor EEPROM",
               meas_mode, this->measurement_mode_);
      if (!this->write_register_(0x95, &this->measurement_mode_, 1)) {
        ESP_LOGE(TAG, "Failed to set measurement mode");
      } else {
        needs_reset = true;
      }
      delay(25);  // wait for EEPROM write to complete
    }
  }

  // Sync number of samples (register 0x98-0x99, uint16, EEPROM)
  uint8_t samples_data[2];
  if (this->read_register_(0x98, samples_data, 2)) {
    uint16_t current = (uint16_t(samples_data[0]) << 8) | samples_data[1];
    if (current != this->number_of_samples_) {
      ESP_LOGI(TAG, "Updating number of samples: %u -> %u", current, this->number_of_samples_);
      samples_data[0] = this->number_of_samples_ >> 8;
      samples_data[1] = this->number_of_samples_ & 0xFF;
      if (!this->write_register_(0x98, samples_data, 2)) {
        ESP_LOGE(TAG, "Failed to set number of samples");
      } else {
        needs_reset = true;
      }
      delay(25);  // wait for EEPROM write to complete
    }
  }

  if (this->measurement_mode_ == 0) {
    // Sync measurement period (register 0x96-0x97, uint16 seconds) — continuous mode only
    uint8_t period_data[2];
    if (this->read_register_(0x96, period_data, 2)) {
      uint16_t current = (uint16_t(period_data[0]) << 8) | period_data[1];
      if (current != this->measurement_period_) {
        ESP_LOGI(TAG, "Updating measurement period: %us -> %us", current, this->measurement_period_);
        period_data[0] = this->measurement_period_ >> 8;
        period_data[1] = this->measurement_period_ & 0xFF;
        if (!this->write_register_(0x96, period_data, 2)) {
          ESP_LOGE(TAG, "Failed to set measurement period");
        } else {
          needs_reset = true;
        }
        delay(25);  // wait for EEPROM write to complete
      }
    }
  }

  // Sync MeterControl register 0xA5 (EEPROM)
  // Bit 2: static IIR filter (0=enabled, 1=disabled)
  // Bit 3: dynamic IIR filter (0=enabled, 1=disabled)
  // Bit 4: pressure compensation (0=enabled, 1=disabled)
  // Bit 5: nRDY polarity (1=active HIGH during measurement, 0=active LOW)
  uint8_t meter_control;
  if (this->read_register_(0xA5, &meter_control, 1)) {
    this->nrdy_enabled_ = (meter_control & 0x01) == 0;
    this->nrdy_active_high_ = (meter_control & 0x20) != 0;
    uint8_t desired = meter_control;

    // IIR filter bits 2-3
    if (this->iir_filter_) {
      desired &= ~0x0C;  // clear bits 2+3 (enable both filters)
    } else {
      desired |= 0x0C;   // set bits 2+3 (disable both filters)
    }

    // Pressure compensation bit 4
    if (this->pressure_compensation_) {
      desired &= ~0x10;  // clear bit 4 (enable pressure compensation)
    } else {
      desired |= 0x10;   // set bit 4 (disable pressure compensation)
    }

    if (desired != meter_control) {
      ESP_LOGI(TAG, "Updating MeterControl: 0x%02X -> 0x%02X", meter_control, desired);
      if (!this->write_register_(0xA5, &desired, 1)) {
        ESP_LOGE(TAG, "Failed to write MeterControl register");
      } else {
        needs_reset = true;
      }
      delay(25);  // wait for EEPROM write to complete
    }
  }

  // Sync ABC period (register 0x9A-0x9B, uint16 hours, EEPROM)
  if (this->abc_period_ != 0) {
    uint8_t abc_data[2];
    if (this->read_register_(0x9A, abc_data, 2)) {
      uint16_t current = (uint16_t(abc_data[0]) << 8) | abc_data[1];
      if (current != this->abc_period_) {
        ESP_LOGI(TAG, "Updating ABC period: %uh -> %uh", current, this->abc_period_);
        abc_data[0] = this->abc_period_ >> 8;
        abc_data[1] = this->abc_period_ & 0xFF;
        if (!this->write_register_(0x9A, abc_data, 2)) {
          ESP_LOGE(TAG, "Failed to set ABC period");
        } else {
          needs_reset = true;
        }
        delay(25);  // wait for EEPROM write to complete
      }
    }
  }

  // Sync ABC target (register 0x9E-0x9F, uint16 ppm, EEPROM)
  if (this->abc_target_ != 0) {
    uint8_t cal_data[2];
    if (this->read_register_(0x9E, cal_data, 2)) {
      uint16_t current = (uint16_t(cal_data[0]) << 8) | cal_data[1];
      if (current != this->abc_target_) {
        ESP_LOGI(TAG, "Updating ABC target: %u ppm -> %u ppm", current, this->abc_target_);
        cal_data[0] = this->abc_target_ >> 8;
        cal_data[1] = this->abc_target_ & 0xFF;
        if (!this->write_register_(0x9E, cal_data, 2)) {
          ESP_LOGE(TAG, "Failed to set ABC target");
        } else {
          needs_reset = true;
        }
        delay(25);  // wait for EEPROM write to complete
      }
    }
    // Warn if target < 400 ppm and ABC is enabled (datasheet: may cause incorrect ABC operation)
    if (this->abc_target_ < 400) {
      uint8_t mc;
      if (this->read_register_(0xA5, &mc, 1)) {
        if (!(mc & 0x02)) {  // bit 1 clear = ABC enabled
          ESP_LOGW(TAG, "ABC target (%u ppm) is below 400 ppm with ABC enabled — "
                        "this may cause incorrect ABC operation (see Senseair datasheet)",
                   this->abc_target_);
        }
      }
    }
  }

  // Reset only if an EEPROM-backed setting changed. On deep-sleep wake the
  // sensor is often still powered, so avoiding an unnecessary reset reduces
  // startup churn and preserves the normal single-measurement timing path.
  if (needs_reset) {
    uint8_t reset_cmd = 0xFF;
    if (!this->write_register_(0xA3, &reset_cmd, 1)) {
      ESP_LOGW(TAG, "Failed to reset sensor");
    } else {
      delay(50);  // T_Start is 35 ms typical; give the sensor margin after reset
    }
  }
}

void SenseairSunriseComponent::update() {
  // In single mode, restore saved ABC/filter state from previous cycle.
  // This must happen BEFORE the pressure write (which overwrites the stale
  // saved pressure at 0xDC-0xDD with the current value) and BEFORE the
  // trigger command (TDE5531 ordering requirement).
  // Do NOT write state on the first cycle (no valid data yet, per TDE5531).
  if (this->measurement_mode_ == 1 && this->state_valid_) {
    if (!this->write_register_(0xC4, this->saved_state_.block, 28)) {
      ESP_LOGW(TAG, "Failed to restore sensor state");
    }
  }

  // Write barometric pressure for compensation (RAM register, every cycle)
  if (this->pressure_compensation_) {
    int16_t pressure = this->pressure_value_;  // static default
    if (this->pressure_source_ != nullptr) {
      if (this->pressure_source_->has_state()) {
        // Convert hPa to 0.1 hPa units
        pressure = static_cast<int16_t>(this->pressure_source_->state * 10.0f);
      } else {
        ESP_LOGD(TAG, "Pressure source not ready, skipping pressure write");
        pressure = 0;  // skip write
      }
    }
    if (pressure != 0) {
      // Clamp to valid range 3000-13000 (300-1300 hPa)
      if (pressure < 3000) pressure = 3000;
      if (pressure > 13000) pressure = 13000;
      uint8_t pressure_data[2] = {
        static_cast<uint8_t>((pressure >> 8) & 0xFF),
        static_cast<uint8_t>(pressure & 0xFF)
      };
      if (!this->write_register_(0xDC, pressure_data, 2)) {
        ESP_LOGW(TAG, "Failed to write pressure compensation value");
      }
    }
  }

  if (this->measurement_mode_ == 1) {
    // Clear stale ErrorStatus before triggering fresh measurement
    uint8_t clear_err = 0x00;
    this->write_register_(0x9D, &clear_err, 1);

    // Single measurement mode: trigger and wait for result
    if (!this->trigger_single_measurement_()) {
      this->status_set_warning("Single measurement failed");
      return;
    }
  }

  // Read registers 0x00 through 0x09 (10 bytes)
  // 0x00-0x01: ErrorStatus
  // 0x02-0x05: Reserved
  // 0x06-0x07: CO2 filtered + pressure compensated (signed 16-bit, ppm)
  // 0x08-0x09: Temperature (signed 16-bit, degC * 100)
  uint8_t data[10];
  if (!this->read_register_(0x00, data, 10)) {
    ESP_LOGW(TAG, "Failed to read sensor data");
    this->status_set_warning("Communication failed");
    return;
  }

  // Parse error status (MSB = data[0], LSB = data[1])
  uint8_t error_msb = data[0];
  uint8_t error_lsb = data[1];
  bool skip_reading = false;

  // LSB bit 7: No measurement completed (normal at startup)
  if (error_lsb & 0x80) {
    ESP_LOGD(TAG, "No measurement completed yet, skipping");
    return;
  }

  // MSB bit 0: Low internal regulated voltage
  if (error_msb & 0x01) {
    ESP_LOGE(TAG, "Low internal regulated voltage - check power supply");
    skip_reading = true;
  }
  // MSB bit 1: Measurement timeout
  if (error_msb & 0x02) {
    ESP_LOGW(TAG, "Measurement timeout");
    skip_reading = true;
  }
  // MSB bit 2: Abnormal signal level
  if (error_msb & 0x04) {
    ESP_LOGW(TAG, "Abnormal signal level detected");
    skip_reading = true;
  }

  // LSB bit 0: Fatal error
  if (error_lsb & 0x01) {
    ESP_LOGE(TAG, "Fatal error - sensor initialization failed");
    skip_reading = true;
  }
  // LSB bit 1: I2C error
  if (error_lsb & 0x02) {
    ESP_LOGE(TAG, "I2C communication error reported by sensor");
  }
  // LSB bit 2: Algorithm error
  if (error_lsb & 0x04) {
    ESP_LOGE(TAG, "Algorithm error - corrupt parameters");
  }
  // LSB bit 3: Calibration error
  if (error_lsb & 0x08) {
    ESP_LOGW(TAG, "Calibration error");
  }
  // LSB bit 4: Self-diagnostics error
  if (error_lsb & 0x10) {
    ESP_LOGE(TAG, "Self-diagnostics error");
  }
  // LSB bit 5: Out of range
  if (error_lsb & 0x20) {
    ESP_LOGW(TAG, "Measurement out of range");
    // Still publish -- reading may be approximate
  }
  // LSB bit 6: Memory error
  if (error_lsb & 0x40) {
    ESP_LOGE(TAG, "Memory error");
  }

  // Clear ErrorStatus now that we've read and logged it (register 0x9D, TDE5531).
  // In single mode this will be cleared before triggering (see Task 4).
  if (this->measurement_mode_ == 0) {
    uint8_t clear_err = 0x00;
    this->write_register_(0x9D, &clear_err, 1);
  }

  if (skip_reading) {
    this->status_set_warning("Sensor error");
    return;
  }

  // Parse CO2 (signed 16-bit, registers 0x06-0x07)
  int16_t co2_raw = ((int16_t) data[6] << 8) | data[7];
  if (co2_raw < 0) {
    ESP_LOGW(TAG, "CO2 reading is negative (%d), skipping", co2_raw);
    return;
  }
  if (this->co2_sensor_ != nullptr) {
    this->co2_sensor_->publish_state(co2_raw);
  }

  // Parse temperature (signed 16-bit, registers 0x08-0x09, unit = degC * 100)
  if (this->temperature_sensor_ != nullptr) {
    int16_t temp_raw = ((int16_t) data[8] << 8) | data[9];
    float temperature = temp_raw / 100.0f;
    this->temperature_sensor_->publish_state(temperature);
  }

  // Save updated sensor state for next power cycle (single mode only).
  // TDE5531: read state AFTER measurement completes, save before power-down.
  if (this->measurement_mode_ == 1) {
    SunriseSavedState new_state{};
    if (this->read_register_(0xC4, new_state.block, 28)) {
      new_state.config_hash = this->compute_config_hash_();
      new_state.crc = this->compute_state_crc_(new_state);
      // Only write to flash if state actually changed (reduces flash wear)
      if (std::memcmp(&new_state, &this->saved_state_, sizeof(new_state)) != 0) {
        this->saved_state_ = new_state;
        this->state_valid_ = true;
        if (!this->state_pref_.save(&this->saved_state_)) {
          ESP_LOGW(TAG, "Failed to persist sensor state to flash");
        }
      }
    } else {
      ESP_LOGW(TAG, "Failed to read sensor state after measurement");
    }
  }

  this->status_clear_warning();
}

void SenseairSunriseComponent::dump_config() {
  ESP_LOGCONFIG(TAG, "Senseair Sunrise:");
  LOG_I2C_DEVICE(this);
  LOG_UPDATE_INTERVAL(this);
  ESP_LOGCONFIG(TAG, "  Measurement Mode: %s", this->measurement_mode_ == 0 ? "continuous" : "single");
  if (this->nrdy_pin_ != nullptr) {
    LOG_PIN("  nRDY Pin: ", this->nrdy_pin_);
    ESP_LOGCONFIG(TAG, "  nRDY Enabled: %s", this->nrdy_enabled_ ? "yes" : "no");
    ESP_LOGCONFIG(TAG, "  nRDY Active During Measurement: %s", this->nrdy_active_high_ ? "HIGH" : "LOW");
  }
  ESP_LOGCONFIG(TAG, "  Number of Samples: %u", this->number_of_samples_);
  if (this->measurement_mode_ == 0) {
    ESP_LOGCONFIG(TAG, "  Measurement Period: %us", this->measurement_period_);
  }
  ESP_LOGCONFIG(TAG, "  IIR Filter: %s", this->iir_filter_ ? "enabled" : "disabled");
  if (this->pressure_compensation_) {
    if (this->pressure_source_ != nullptr) {
      ESP_LOGCONFIG(TAG, "  Pressure Compensation: enabled (dynamic sensor)");
    } else {
      ESP_LOGCONFIG(TAG, "  Pressure Compensation: enabled (%.1f hPa)",
                    this->pressure_value_ / 10.0f);
    }
  } else {
    ESP_LOGCONFIG(TAG, "  Pressure Compensation: disabled");
  }
  if (this->abc_period_ != 0) {
    ESP_LOGCONFIG(TAG, "  ABC Period: %uh", this->abc_period_);
  }
  if (this->abc_target_ != 0) {
    ESP_LOGCONFIG(TAG, "  ABC Target: %u ppm", this->abc_target_);
  }
  LOG_SENSOR("  ", "CO2", this->co2_sensor_);
  LOG_SENSOR("  ", "Temperature", this->temperature_sensor_);
}

void SenseairSunriseComponent::background_calibration() {
  ESP_LOGI(TAG, "Initiating background calibration...");
  // Step 1: Clear CalibrationStatus before starting (TDE5531 recommendation)
  uint8_t clear = 0x00;
  if (!this->write_register_(0x81, &clear, 1)) {
    ESP_LOGW(TAG, "Failed to clear CalibrationStatus");
  }
  // Step 2: Write calibration command 0x7C06 to registers 0x82-0x83
  uint8_t cmd[2] = {0x7C, 0x06};
  if (!this->write_register_(0x82, cmd, 2)) {
    ESP_LOGE(TAG, "Failed to send background calibration command");
    return;
  }
  // Step 3: Read back CalibrationStatus to verify (TDE5531 example)
  delay(50);
  uint8_t status;
  if (this->read_register_(0x81, &status, 1)) {
    if (status & 0x20) {
      ESP_LOGI(TAG, "Background calibration successful (status=0x%02X)", status);
    } else {
      ESP_LOGW(TAG, "Background calibration status: 0x%02X (expected bit 5 set)", status);
    }
  } else {
    ESP_LOGW(TAG, "Failed to read CalibrationStatus after calibration");
  }
}

void SenseairSunriseComponent::abc_enable() {
  ESP_LOGI(TAG, "Enabling ABC...");
  uint8_t meter_control;
  if (!this->read_register_(0xA5, &meter_control, 1)) {
    ESP_LOGE(TAG, "Failed to read MeterControl register");
    return;
  }
  if (!(meter_control & 0x02)) {
    ESP_LOGI(TAG, "ABC is already enabled");
    return;
  }
  meter_control &= ~0x02;
  if (!this->write_register_(0xA5, &meter_control, 1)) {
    ESP_LOGE(TAG, "Failed to write MeterControl register");
    return;
  }
  delay(25);  // EEPROM write time for 006-0-0007
  // EEPROM-backed changes require a sensor reset (TDE7318)
  uint8_t reset_cmd = 0xFF;
  if (!this->write_register_(0xA3, &reset_cmd, 1)) {
    ESP_LOGW(TAG, "Failed to reset sensor after enabling ABC");
  } else {
    delay(50);  // T_Start typ 35 ms
  }
  ESP_LOGI(TAG, "ABC enabled (sensor reset)");
}

void SenseairSunriseComponent::abc_disable() {
  ESP_LOGI(TAG, "Disabling ABC...");
  uint8_t meter_control;
  if (!this->read_register_(0xA5, &meter_control, 1)) {
    ESP_LOGE(TAG, "Failed to read MeterControl register");
    return;
  }
  if (meter_control & 0x02) {
    ESP_LOGI(TAG, "ABC is already disabled");
    return;
  }
  meter_control |= 0x02;
  if (!this->write_register_(0xA5, &meter_control, 1)) {
    ESP_LOGE(TAG, "Failed to write MeterControl register");
    return;
  }
  delay(25);  // EEPROM write time for 006-0-0007
  // EEPROM-backed changes require a sensor reset (TDE7318)
  uint8_t reset_cmd = 0xFF;
  if (!this->write_register_(0xA3, &reset_cmd, 1)) {
    ESP_LOGW(TAG, "Failed to reset sensor after disabling ABC");
  } else {
    delay(50);  // T_Start typ 35 ms
  }
  ESP_LOGI(TAG, "ABC disabled (sensor reset)");
}

}  // namespace senseair_sunrise
}  // namespace esphome
