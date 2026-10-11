#include "senseair_sunrise.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"
#include <cstring>
#include "esphome/core/application.h"
#include "esphome/core/preferences.h"

namespace esphome {
namespace senseair_sunrise {

static const char *const TAG = "senseair_sunrise";

// Register addresses (TDE5531 register map)
static constexpr uint8_t REG_ERROR_STATUS       = 0x00;
static constexpr uint8_t REG_MEASUREMENT_COUNT   = 0x0D;
static constexpr uint8_t REG_FIRMWARE_TYPE       = 0x2F;
static constexpr uint8_t REG_FIRMWARE_REV        = 0x38;
static constexpr uint8_t REG_SENSOR_ID           = 0x3A;
static constexpr uint8_t REG_PRODUCT_CODE        = 0x70;
static constexpr uint8_t REG_CAL_STATUS          = 0x81;
static constexpr uint8_t REG_CAL_COMMAND         = 0x82;
static constexpr uint8_t REG_MEASUREMENT_MODE    = 0x95;
static constexpr uint8_t REG_MEASUREMENT_PERIOD  = 0x96;
static constexpr uint8_t REG_NUM_SAMPLES         = 0x98;
static constexpr uint8_t REG_ABC_PERIOD          = 0x9A;
static constexpr uint8_t REG_CLEAR_ERROR_STATUS  = 0x9D;
static constexpr uint8_t REG_ABC_TARGET          = 0x9E;
static constexpr uint8_t REG_SENSOR_RESET        = 0xA3;
static constexpr uint8_t REG_METER_CONTROL       = 0xA5;
static constexpr uint8_t REG_START_SINGLE        = 0xC3;
static constexpr uint8_t REG_STATE_BLOCK         = 0xC4;
static constexpr uint8_t REG_BARO_PRESSURE       = 0xDC;

// TDE5531: EEPROM writes take <25 ms on Sunrise 006-0-0002/0007 and <107 ms on
// other models (e.g. 006-0-0008, Sunlight)
static constexpr uint32_t EEPROM_WRITE_DELAY_MS = 110;
static constexpr uint16_t ABC_PERIOD_DEFAULT_HOURS = 180;

void SenseairSunriseComponent::wake_up_() {
  // Wake sensor by sending its address on the bus. TDE5531 says any SDA
  // activity wakes the sensor from sleep; NACK is expected and harmless.
  // We send a dummy byte (0x00) because some ESP8266 I2C implementations
  // do not reliably generate bus activity for zero-length writes.
  uint8_t dummy = 0x00;
  this->write(&dummy, 1);
  delay(1);
}

bool SenseairSunriseComponent::read_register_(uint8_t reg, uint8_t *data, size_t len) {
  this->wake_up_();
  // Use separate write + read transactions (STOP between them) rather than
  // write_read() (repeated START).  The Sunrise sensor does not reliably
  // handle repeated START on all platforms; separate transactions work
  // because the 15 ms wake timeout keeps the sensor awake between them.
  i2c::ErrorCode err = this->write(&reg, 1);
  if (err != i2c::ERROR_OK) {
    ESP_LOGW(TAG, "Write register address 0x%02X failed: %d", reg, err);
    return false;
  }
  err = this->read(data, len);
  if (err != i2c::ERROR_OK) {
    ESP_LOGW(TAG, "Read data from register 0x%02X failed: %d", reg, err);
    return false;
  }
  return true;
}

bool SenseairSunriseComponent::write_register_(uint8_t reg, const uint8_t *data, size_t len) {
  if (len > 28) {
    ESP_LOGE(TAG, "write_register_: len %u exceeds buffer", len);
    return false;
  }
  this->wake_up_();
  uint8_t buf[29];  // max: 28-byte state block + 1 register address
  buf[0] = reg;
  std::memcpy(&buf[1], data, len);
  i2c::ErrorCode err = this->write(buf, len + 1);
  if (err != i2c::ERROR_OK) {
    ESP_LOGW(TAG, "Write register 0x%02X failed: %d", reg, err);
    return false;
  }
  return true;
}

uint32_t SenseairSunriseComponent::compute_config_hash_() const {
  // FNV-1a over the config fields that invalidate saved sensor state
  uint32_t h = 2166136261u;
  auto feed = [&](uint8_t byte) {
    h ^= byte;
    h *= 16777619u;
  };
  for (int i = 0; i < 4; i++)
    feed((this->sensor_id_ >> (i * 8)) & 0xFF);  // state belongs to one physical sensor
  feed(this->measurement_mode_);
  feed(this->iir_filter_ ? 1 : 0);
  feed(this->pressure_compensation_ ? 1 : 0);
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

bool SenseairSunriseComponent::sync_eeprom_u16_(uint8_t reg, uint16_t desired, bool &needs_reset, const char *name,
                                                uint16_t also_accept) {
  uint8_t data[2];
  if (!this->read_register_(reg, data, 2))
    return false;
  uint16_t current = (uint16_t(data[0]) << 8) | data[1];
  if (current == desired)
    return true;
  if (current == also_accept) {
    ESP_LOGD(TAG, "%s: sensor reports %u for configured %u (rounded by sensor), not rewriting", name, current,
             desired);
    return true;
  }
  ESP_LOGI(TAG, "Updating %s: %u -> %u", name, current, desired);
  data[0] = desired >> 8;
  data[1] = desired & 0xFF;
  if (!this->write_register_(reg, data, 2)) {
    ESP_LOGE(TAG, "Failed to set %s", name);
    return false;
  }
  needs_reset = true;
  delay(EEPROM_WRITE_DELAY_MS);  // wait for EEPROM write to complete
  return true;
}

void SenseairSunriseComponent::setup() {
  ESP_LOGCONFIG(TAG, "Setting up Senseair Sunrise...");
  bool needs_reset = false;

  if (this->nrdy_pin_ != nullptr) {
    this->nrdy_pin_->setup();
  }

  // Read firmware revision
  uint8_t fw_data[2];
  if (this->read_register_(REG_FIRMWARE_REV, fw_data, 2)) {
    ESP_LOGI(TAG, "Firmware version: %d.%d", fw_data[0], fw_data[1]);
  } else {
    ESP_LOGE(TAG, "Failed to communicate with Senseair Sunrise");
    this->mark_failed();
    return;
  }

  // Read firmware type
  uint8_t fw_type;
  if (this->read_register_(REG_FIRMWARE_TYPE, &fw_type, 1)) {
    ESP_LOGI(TAG, "Firmware type: %u", fw_type);
  }

  // Read sensor ID
  uint8_t id_data[4];
  if (this->read_register_(REG_SENSOR_ID, id_data, 4)) {
    this->sensor_id_ = ((uint32_t) id_data[0] << 24) | ((uint32_t) id_data[1] << 16) |
                       ((uint32_t) id_data[2] << 8) | id_data[3];
    ESP_LOGI(TAG, "Sensor ID: %u", this->sensor_id_);
  }

  // Load state saved for single-measurement mode. Saved state is tied to the
  // sensor ID, so a swapped sensor never receives another sensor's ABC/filter
  // state. On ESP8266 preferences live in RTC memory unless restore_from_flash
  // is set: they survive deep sleep but not a power loss.
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
      ESP_LOGD(TAG, "No valid saved sensor state (first cycle, config or sensor change, or corrupt)");
    }
  }

  // Read product code (16-byte ASCII string, requires firmware 4.08+)
  if (fw_data[0] > 4 || (fw_data[0] == 4 && fw_data[1] >= 8)) {
    uint8_t product_code[16];
    if (this->read_register_(REG_PRODUCT_CODE, product_code, 16)) {
      char product_str[17];
      memcpy(product_str, product_code, 16);
      product_str[16] = '\0';
      ESP_LOGI(TAG, "Product code: %s", product_str);
    }
  }

  // Check and sync measurement mode with config
  uint8_t meas_mode;
  if (this->read_register_(REG_MEASUREMENT_MODE, &meas_mode, 1)) {
    if (meas_mode != this->measurement_mode_) {
      ESP_LOGW(TAG, "Sensor measurement mode (%d) differs from configured mode (%d), updating sensor EEPROM",
               meas_mode, this->measurement_mode_);
      if (!this->write_register_(REG_MEASUREMENT_MODE, &this->measurement_mode_, 1)) {
        ESP_LOGE(TAG, "Failed to set measurement mode");
      } else {
        needs_reset = true;
      }
      delay(EEPROM_WRITE_DELAY_MS);  // wait for EEPROM write to complete
    }
  }

  // TDE5531: except on 006-0-0007, the sensor rounds odd sample counts down to
  // even and values below 2 up to 2. Accept that value so we don't rewrite
  // EEPROM on every boot.
  uint16_t rounded_samples = this->number_of_samples_ < 2 ? 2 : (this->number_of_samples_ & ~1u);
  this->sync_eeprom_u16_(REG_NUM_SAMPLES, this->number_of_samples_, needs_reset, "number of samples",
                         rounded_samples);

  if (this->measurement_mode_ == 0) {
    this->sync_eeprom_u16_(REG_MEASUREMENT_PERIOD, this->measurement_period_, needs_reset, "measurement period");
  }

  // Sync MeterControl (EEPROM)
  // Bit 2: static IIR filter (0=enabled, 1=disabled)
  // Bit 3: dynamic IIR filter (0=enabled, 1=disabled)
  // Bit 4: pressure compensation (0=enabled, 1=disabled)
  // Bit 5: nRDY polarity (1=active HIGH during measurement, 0=active LOW)
  uint8_t meter_control;
  if (this->read_register_(REG_METER_CONTROL, &meter_control, 1)) {
    this->nrdy_enabled_ = (meter_control & 0x01) == 0;
    this->nrdy_active_high_ = (meter_control & 0x20) != 0;
    this->abc_enabled_ = (meter_control & 0x02) == 0;
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
      if (!this->write_register_(REG_METER_CONTROL, &desired, 1)) {
        ESP_LOGE(TAG, "Failed to write MeterControl register");
      } else {
        needs_reset = true;
      }
      delay(EEPROM_WRITE_DELAY_MS);  // wait for EEPROM write to complete
    }
  }

  if (this->abc_period_ != 0) {
    this->sync_eeprom_u16_(REG_ABC_PERIOD, this->abc_period_, needs_reset, "ABC period");
  }

  // TDE5531: ABC only runs when MeterControl bit 1 is clear AND the period is
  // 1-65534 (0 and 65535 disable it).
  if (this->abc_enabled_) {
    uint8_t period_data[2];
    if (this->read_register_(REG_ABC_PERIOD, period_data, 2)) {
      uint16_t period = (uint16_t(period_data[0]) << 8) | period_data[1];
      if (period == 0 || period == 0xFFFF) {
        ESP_LOGW(TAG, "ABC enabled in MeterControl but ABC period is %u, which disables ABC", period);
        this->abc_enabled_ = false;
      }
    }
  }

  if (this->abc_target_ != 0) {
    this->sync_eeprom_u16_(REG_ABC_TARGET, this->abc_target_, needs_reset, "ABC target");
    // Warn if target < 400 ppm and ABC is enabled (datasheet: may cause incorrect ABC operation)
    if (this->abc_target_ < 400 && this->abc_enabled_) {
      ESP_LOGW(TAG, "ABC target (%u ppm) is below 400 ppm with ABC enabled — "
                    "this may cause incorrect ABC operation (see Senseair datasheet)",
               this->abc_target_);
    }
  }

  // Reset only if an EEPROM-backed setting changed. On deep-sleep wake the
  // sensor is often still powered, so avoiding an unnecessary reset reduces
  // startup churn and preserves the normal single-measurement timing path.
  if (needs_reset) {
    uint8_t reset_cmd = 0xFF;
    if (!this->write_register_(REG_SENSOR_RESET, &reset_cmd, 1)) {
      ESP_LOGW(TAG, "Failed to reset sensor");
    } else {
      delay(50);  // T_Start is 35 ms typical; give the sensor margin after reset
    }
  }
}

void SenseairSunriseComponent::update() {
  // Guard: skip if a single measurement is still in progress
  if (this->state_ != SunriseState::IDLE) {
    ESP_LOGW(TAG, "Previous measurement still in progress, skipping update");
    return;
  }

  // In single mode, restore saved ABC/filter state only if the sensor lost it.
  // ErrorStatus LSB bit 7 ("no measurement completed") is set at sensor
  // startup and cleared after the first measurement (TDE5531), so it tells us
  // whether the sensor restarted since its last measurement. If it did not
  // (e.g. EN tied high and only the ESP slept), the sensor's live state is
  // newer than our snapshot and must not be overwritten.
  // Must happen BEFORE pressure write and trigger (TDE5531 ordering).
  // Do NOT write state on the first cycle (no valid data yet).
  if (this->measurement_mode_ == 1 && this->state_valid_) {
    uint8_t status[2];
    if (!this->read_register_(REG_ERROR_STATUS, status, 2)) {
      ESP_LOGW(TAG, "Failed to read ErrorStatus, not restoring sensor state");
    } else if (status[1] & 0x80) {
      ESP_LOGD(TAG, "Sensor restarted since its last measurement, restoring saved state");
      if (!this->write_register_(REG_STATE_BLOCK, this->saved_state_.block, 28)) {
        ESP_LOGW(TAG, "Failed to restore sensor state");
      }
    } else {
      ESP_LOGD(TAG, "Sensor kept its state since the last measurement, not restoring");
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
      if (!this->write_register_(REG_BARO_PRESSURE, pressure_data, 2)) {
        ESP_LOGW(TAG, "Failed to write pressure compensation value");
      }
    }
  }

  if (this->measurement_mode_ == 1) {
    // Single measurement mode: clear error, trigger, and return immediately.
    // loop() handles waiting for completion and reading results.
    uint8_t clear_err = 0x00;
    this->write_register_(REG_CLEAR_ERROR_STATUS, &clear_err, 1);

    uint8_t cmd = 0x01;
    if (!this->write_register_(REG_START_SINGLE, &cmd, 1)) {
      ESP_LOGE(TAG, "Failed to trigger single measurement");
      this->status_set_warning("Single measurement failed");
      return;
    }

    this->measurement_timeout_ms_ = 35 + (static_cast<uint32_t>(this->number_of_samples_) * 300) + 1500;
    this->measurement_start_ = millis();
    this->saw_nrdy_active_ = false;
    this->state_ = SunriseState::WAIT_MEASUREMENT;
    ESP_LOGD(TAG, "Single measurement triggered, timeout=%ums", this->measurement_timeout_ms_);
    return;  // Non-blocking — loop() takes over from here
  }

  // Continuous mode: sensor measures autonomously, just read the latest result
  this->read_and_publish_();
}

void SenseairSunriseComponent::loop() {
  switch (this->state_) {
    case SunriseState::IDLE:
      return;

    case SunriseState::WAIT_MEASUREMENT: {
      // Check nRDY pin for measurement completion (active → inactive edge)
      if (this->nrdy_pin_ != nullptr && this->nrdy_enabled_) {
        bool pin_state = this->nrdy_pin_->digital_read();
        if (pin_state == this->nrdy_active_high_) {
          this->saw_nrdy_active_ = true;
        } else if (this->saw_nrdy_active_) {
          this->state_ = SunriseState::READ_RESULT;
          return;
        }
      }

      // Timeout — fall back to status register polling
      if (millis() - this->measurement_start_ > this->measurement_timeout_ms_) {
        if (this->nrdy_pin_ != nullptr && this->nrdy_enabled_) {
          ESP_LOGW(TAG, "nRDY timeout after %ums (saw_active=%s); falling back to status polling",
                   this->measurement_timeout_ms_,
                   this->saw_nrdy_active_ ? "yes" : "no");
        }
        this->state_ = SunriseState::POLL_STATUS;
        this->poll_start_ = millis();
      }
      return;
    }

    case SunriseState::POLL_STATUS: {
      if (millis() - this->poll_start_ > 1000) {
        ESP_LOGE(TAG, "Timeout waiting for single measurement to complete");
        this->status_set_warning("Single measurement timeout");
        this->state_ = SunriseState::IDLE;
        return;
      }

      // Rate-limit I2C polling to avoid bus contention (~50ms between reads)
      if (millis() - this->last_poll_attempt_ms_ < 50)
        return;
      this->last_poll_attempt_ms_ = millis();

      // Poll ErrorStatus register — bit 7 of LSB clears when measurement is ready
      uint8_t status[2];
      if (this->read_register_(REG_ERROR_STATUS, status, 2)) {
        if ((status[1] & 0x80) == 0) {
          this->state_ = SunriseState::READ_RESULT;
          return;
        }
      }
      return;
    }

    case SunriseState::READ_RESULT:
      this->read_and_publish_();
      this->state_ = SunriseState::IDLE;
      return;
  }
}

void SenseairSunriseComponent::read_and_publish_() {
  // Read registers 0x00 through 0x0D (14 bytes)
  // 0x00-0x01: ErrorStatus
  // 0x02-0x05: Reserved
  // 0x06-0x07: CO2 filtered + pressure compensated (signed 16-bit, ppm)
  // 0x08-0x09: Temperature (signed 16-bit, degC * 100)
  // 0x0A-0x0C: Reserved
  // 0x0D:      Measurement count
  uint8_t data[14];
  if (!this->read_register_(REG_ERROR_STATUS, data, 14)) {
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

  this->check_calibration_result_(data[13]);

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
  // In single mode this is cleared before triggering (see top of update()).
  if (this->measurement_mode_ == 0) {
    uint8_t clear_err = 0x00;
    this->write_register_(REG_CLEAR_ERROR_STATUS, &clear_err, 1);
  }

  if (skip_reading) {
    this->status_set_warning("Sensor error");
    return;
  }

  // Parse CO2 (signed 16-bit, registers 0x06-0x07)
  int16_t co2_raw = ((int16_t) data[6] << 8) | data[7];
  if (co2_raw < 0) {
    // Diagnostic: read unfiltered concentration (0x14-0x15), filtered no-pressure (0x12-0x13),
    // and measurement count (0x0D) to help distinguish calibration vs hardware issue.
    uint8_t diag[9];  // 0x0D through 0x15
    if (this->read_register_(REG_MEASUREMENT_COUNT, diag, 9)) {
      uint8_t meas_count = diag[0];
      int16_t filtered_no_pc = ((int16_t) diag[5] << 8) | diag[6];
      int16_t unfiltered = ((int16_t) diag[7] << 8) | diag[8];
      ESP_LOGW(TAG, "CO2 reading is negative (%d), skipping — diag: unfiltered=%d, filtered_no_pc=%d, meas_count=%u",
               co2_raw, unfiltered, filtered_no_pc, meas_count);
    } else {
      ESP_LOGW(TAG, "CO2 reading is negative (%d), skipping", co2_raw);
    }
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
    if (this->read_register_(REG_STATE_BLOCK, new_state.block, 28)) {
      new_state.config_hash = this->compute_config_hash_();
      new_state.crc = this->compute_state_crc_(new_state);
      // Only save if state actually changed (avoids flash wear with restore_from_flash)
      if (std::memcmp(&new_state, &this->saved_state_, sizeof(new_state)) != 0) {
        this->saved_state_ = new_state;
        this->state_valid_ = true;
        ESP_LOGD(TAG, "Saved updated sensor state");
        if (!this->state_pref_.save(&this->saved_state_)) {
          ESP_LOGW(TAG, "Failed to save sensor state to preferences");
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
  // Calibrates against the ABC target (fresh air, 400 ppm by default)
  this->start_calibration_(0x7C06, 0x20, "Background calibration");
}

void SenseairSunriseComponent::restore_factory_calibration() {
  this->start_calibration_(0x7C02, 0x04, "Factory calibration restore");
}

void SenseairSunriseComponent::start_calibration_(uint16_t command, uint8_t success_mask, const char *name) {
  // TDE5531: the sensor calibrates on the first measurement after it receives
  // the command, so the result can only be checked once a new measurement has
  // completed. In single mode the host must trigger that measurement.
  if (this->measurement_mode_ == 1 && this->state_ != SunriseState::IDLE) {
    ESP_LOGW(TAG, "%s not started: a measurement is in progress, try again", name);
    return;
  }

  uint8_t count;
  if (!this->read_register_(REG_MEASUREMENT_COUNT, &count, 1)) {
    ESP_LOGE(TAG, "%s not started: failed to read measurement count", name);
    return;
  }

  ESP_LOGI(TAG, "Starting %s...", name);
  // Clear CalibrationStatus before starting (TDE5531 recommendation)
  uint8_t clear = 0x00;
  if (!this->write_register_(REG_CAL_STATUS, &clear, 1)) {
    ESP_LOGW(TAG, "Failed to clear CalibrationStatus");
  }
  uint8_t cmd[2] = {static_cast<uint8_t>(command >> 8), static_cast<uint8_t>(command & 0xFF)};
  if (!this->write_register_(REG_CAL_COMMAND, cmd, 2)) {
    ESP_LOGE(TAG, "Failed to send %s command", name);
    return;
  }

  this->calibration_pending_ = true;
  this->calibration_mask_ = success_mask;
  this->calibration_count_ = count;
  this->calibration_name_ = name;

  if (this->measurement_mode_ == 1) {
    ESP_LOGI(TAG, "%s: triggering a measurement to apply it", name);
    this->update();
  } else {
    ESP_LOGI(TAG, "%s: result will be checked after the next measurement", name);
  }
}

void SenseairSunriseComponent::check_calibration_result_(uint8_t measurement_count) {
  if (!this->calibration_pending_ || measurement_count == this->calibration_count_)
    return;  // nothing pending, or no new measurement since the command
  this->calibration_pending_ = false;

  uint8_t status;
  if (!this->read_register_(REG_CAL_STATUS, &status, 1)) {
    ESP_LOGW(TAG, "%s: failed to read CalibrationStatus", this->calibration_name_);
    return;
  }
  if (status & this->calibration_mask_) {
    ESP_LOGI(TAG, "%s succeeded (CalibrationStatus=0x%02X)", this->calibration_name_, status);
  } else {
    ESP_LOGW(TAG, "%s failed (CalibrationStatus=0x%02X, expected 0x%02X); ensure a stable environment",
             this->calibration_name_, status, this->calibration_mask_);
  }
}

void SenseairSunriseComponent::dump_registers() {
  ESP_LOGI(TAG, "=== Register dump ===");
  // Read in blocks matching the TDE5531 register map
  struct { uint8_t start; uint8_t len; const char *label; } blocks[] = {
    {0x00, 16, "00-0F status+meas"},    // ErrorStatus, CO2, temp, count, cycle
    {0x10, 6,  "10-15 unfilt/filt"},     // Unfiltered/filtered concentrations
    {0x2F, 1,  "2F fw_type"},            // Firmware type
    {0x38, 6,  "38-3D fw_rev+id"},       // Firmware rev + sensor ID
    {0x81, 9,  "81-89 cal+abc_time"},    // CalStatus, CalCmd, CalTarget, MeasOverride, ABC time
    {0x8A, 8,  "8A-91 abc_params"},      // ABC parameter state
    {0x93, 1,  "93 SCsingle"},            // Single-meas config (trigger is 0xC3 in mirror block)
    {0x95, 5,  "95-99 mode+timing"},     // Mode, period, samples
    {0x9A, 2,  "9A-9B abc_period"},      // ABC period
    {0x9E, 2,  "9E-9F abc_target"},      // ABC target
    {0xA1, 1,  "A1 iir_param"},          // IIR filter parameter
    {0xA5, 1,  "A5 meter_ctrl"},         // MeterControl
    {0xA7, 5,  "A7-AB addr+scale"},      // Address + scale factors
    {0xC0, 32, "C0-DF mirror_block"},    // Full mirror/state block
  };
  for (auto &b : blocks) {
    uint8_t data[32];
    if (b.len > sizeof(data)) {
      ESP_LOGW(TAG, "  [%s]: block too large for buffer", b.label);
      continue;
    }
    if (this->read_register_(b.start, data, b.len)) {
      char hex[97];  // 32 bytes * 3 chars + null
      for (uint8_t i = 0; i < b.len; i++)
        snprintf(hex + i * 3, 4, "%02X ", data[i]);
      hex[b.len * 3 - 1] = '\0';  // trim trailing space
      ESP_LOGI(TAG, "  [%s]: %s", b.label, hex);
    } else {
      ESP_LOGW(TAG, "  [%s]: READ FAILED", b.label);
    }
  }
  ESP_LOGI(TAG, "=== End register dump ===");
}

bool SenseairSunriseComponent::abc_set_(bool enable) {
  const char *label = enable ? "enable" : "disable";
  ESP_LOGI(TAG, "%s ABC...", enable ? "Enabling" : "Disabling");
  bool changed = false;

  // TDE5531: ABC also needs a period of 1-65534 hours; 0 and 65535 disable it
  if (enable) {
    uint8_t period_data[2];
    if (!this->read_register_(REG_ABC_PERIOD, period_data, 2)) {
      ESP_LOGE(TAG, "Failed to read ABC period");
      return false;
    }
    uint16_t period = (uint16_t(period_data[0]) << 8) | period_data[1];
    if (period == 0 || period == 0xFFFF) {
      uint16_t desired = this->abc_period_ != 0 ? this->abc_period_ : ABC_PERIOD_DEFAULT_HOURS;
      ESP_LOGI(TAG, "ABC period is %u (disables ABC), setting it to %u hours", period, desired);
      period_data[0] = desired >> 8;
      period_data[1] = desired & 0xFF;
      if (!this->write_register_(REG_ABC_PERIOD, period_data, 2)) {
        ESP_LOGE(TAG, "Failed to write ABC period");
        return false;
      }
      delay(EEPROM_WRITE_DELAY_MS);
      changed = true;
    }
  }

  uint8_t meter_control;
  if (!this->read_register_(REG_METER_CONTROL, &meter_control, 1)) {
    ESP_LOGE(TAG, "Failed to read MeterControl register");
    return false;
  }

  bool currently_enabled = !(meter_control & 0x02);
  if (currently_enabled != enable) {
    if (enable) {
      meter_control &= ~0x02;
    } else {
      meter_control |= 0x02;
    }
    if (!this->write_register_(REG_METER_CONTROL, &meter_control, 1)) {
      ESP_LOGE(TAG, "Failed to write MeterControl register");
      return false;
    }
    delay(EEPROM_WRITE_DELAY_MS);
    changed = true;
  }

  this->abc_enabled_ = enable;
  if (!changed) {
    ESP_LOGI(TAG, "ABC is already %sd", label);
    return true;
  }

  // EEPROM-backed changes require a sensor reset (TDE5531)
  uint8_t reset_cmd = 0xFF;
  if (!this->write_register_(REG_SENSOR_RESET, &reset_cmd, 1)) {
    ESP_LOGW(TAG, "Failed to reset sensor after %s ABC", label);
  } else {
    delay(50);  // T_Start typ 35 ms
  }

  ESP_LOGI(TAG, "ABC %sd (sensor reset)", label);
  return true;
}

bool SenseairSunriseComponent::abc_enable() { return this->abc_set_(true); }
bool SenseairSunriseComponent::abc_disable() { return this->abc_set_(false); }

#ifdef USE_SWITCH
void SenseairSunriseABCSwitch::setup() {
  this->publish_state(this->parent_->is_abc_enabled());
}

void SenseairSunriseABCSwitch::write_state(bool state) {
  bool success = state ? this->parent_->abc_enable() : this->parent_->abc_disable();
  if (success) {
    this->publish_state(state);
  }
}

void SenseairSunriseABCSwitch::dump_config() {
  LOG_SWITCH("  ", "ABC", this);
}
#endif  // USE_SWITCH

}  // namespace senseair_sunrise
}  // namespace esphome
