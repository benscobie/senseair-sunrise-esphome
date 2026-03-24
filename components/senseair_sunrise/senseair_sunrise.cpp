#include "senseair_sunrise.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"
#include <cstring>
#include <vector>

namespace esphome {
namespace senseair_sunrise {

static const char *const TAG = "senseair_sunrise";

bool SenseairSunriseComponent::wake_up_() {
  // Write register address 0x00 to wake sensor.
  // If sensor was asleep, it will NACK -- this is expected behavior.
  // Any SDA activity wakes it up.
  uint8_t reg = 0x00;
  this->write(&reg, 1);
  // ignore error -- NACK is expected if sensor was asleep
  delay(1);
  return true;
}

bool SenseairSunriseComponent::read_register_(uint8_t reg, uint8_t *data, size_t len) {
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

  // Wait for measurement to complete
  if (this->nrdy_pin_ != nullptr) {
    // Poll nRDY pin (active LOW) with timeout
    uint32_t start = millis();
    while (this->nrdy_pin_->digital_read()) {
      if (millis() - start > 5000) {
        ESP_LOGE(TAG, "Timeout waiting for nRDY pin");
        return false;
      }
      delay(50);
    }
  } else {
    // No nRDY pin -- busy-wait for measurement to complete
    delay(3000);
  }

  return true;
}

void SenseairSunriseComponent::setup() {
  ESP_LOGCONFIG(TAG, "Setting up Senseair Sunrise...");

  this->wake_up_();

  // Read firmware revision (registers 0x38-0x39)
  uint8_t fw_data[2];
  if (this->read_register_(0x38, fw_data, 2)) {
    ESP_LOGI(TAG, "Firmware version: %d.%d", fw_data[0], fw_data[1]);
  } else {
    ESP_LOGE(TAG, "Failed to communicate with Senseair Sunrise");
    this->mark_failed();
    return;
  }

  // Read sensor ID (registers 0x3A-0x3D)
  this->wake_up_();
  uint8_t id_data[4];
  if (this->read_register_(0x3A, id_data, 4)) {
    uint32_t sensor_id = ((uint32_t) id_data[0] << 24) | ((uint32_t) id_data[1] << 16) |
                         ((uint32_t) id_data[2] << 8) | id_data[3];
    ESP_LOGI(TAG, "Sensor ID: %u", sensor_id);
  }

  // Check and sync measurement mode with config
  this->wake_up_();
  uint8_t meas_mode;
  if (this->read_register_(0x95, &meas_mode, 1)) {
    if (meas_mode != this->measurement_mode_) {
      ESP_LOGW(TAG, "Sensor measurement mode (%d) differs from configured mode (%d), updating sensor EEPROM",
               meas_mode, this->measurement_mode_);
      this->wake_up_();
      if (!this->write_register_(0x95, &this->measurement_mode_, 1)) {
        ESP_LOGE(TAG, "Failed to set measurement mode");
      }
    }
  }

  // Sync number of samples (register 0x98-0x99, uint16, EEPROM)
  this->wake_up_();
  uint8_t samples_data[2];
  if (this->read_register_(0x98, samples_data, 2)) {
    uint16_t current = (uint16_t(samples_data[0]) << 8) | samples_data[1];
    if (current != this->number_of_samples_) {
      ESP_LOGI(TAG, "Updating number of samples: %u -> %u", current, this->number_of_samples_);
      samples_data[0] = this->number_of_samples_ >> 8;
      samples_data[1] = this->number_of_samples_ & 0xFF;
      this->wake_up_();
      if (!this->write_register_(0x98, samples_data, 2)) {
        ESP_LOGE(TAG, "Failed to set number of samples");
      }
    }
  }

  // Sync measurement period (register 0x96-0x97, uint16 seconds)
  this->wake_up_();
  uint8_t period_data[2];
  if (this->read_register_(0x96, period_data, 2)) {
    uint16_t current = (uint16_t(period_data[0]) << 8) | period_data[1];
    if (current != this->measurement_period_) {
      ESP_LOGI(TAG, "Updating measurement period: %us -> %us", current, this->measurement_period_);
      period_data[0] = this->measurement_period_ >> 8;
      period_data[1] = this->measurement_period_ & 0xFF;
      this->wake_up_();
      if (!this->write_register_(0x96, period_data, 2)) {
        ESP_LOGE(TAG, "Failed to set measurement period");
      }
    }
  }

  // Sync IIR filter (MeterControl register 0xA5, EEPROM)
  // Bit 2: static IIR filter (0=enabled, 1=disabled)
  // Bit 3: dynamic IIR filter (0=enabled, 1=disabled)
  this->wake_up_();
  uint8_t meter_control;
  if (this->read_register_(0xA5, &meter_control, 1)) {
    bool static_disabled = meter_control & 0x04;
    bool dynamic_disabled = meter_control & 0x08;
    bool currently_enabled = !static_disabled && !dynamic_disabled;
    if (currently_enabled != this->iir_filter_) {
      if (this->iir_filter_) {
        meter_control &= ~0x0C;  // clear bits 2+3 (enable both filters)
      } else {
        meter_control |= 0x0C;   // set bits 2+3 (disable both filters)
      }
      ESP_LOGI(TAG, "Updating IIR filter: %s", this->iir_filter_ ? "enabled" : "disabled");
      this->wake_up_();
      if (!this->write_register_(0xA5, &meter_control, 1)) {
        ESP_LOGE(TAG, "Failed to set IIR filter");
      }
    }
  }

}

void SenseairSunriseComponent::update() {
  this->wake_up_();

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

  this->status_clear_warning();
}

void SenseairSunriseComponent::dump_config() {
  ESP_LOGCONFIG(TAG, "Senseair Sunrise:");
  LOG_I2C_DEVICE(this);
  LOG_UPDATE_INTERVAL(this);
  ESP_LOGCONFIG(TAG, "  Number of Samples: %u", this->number_of_samples_);
  ESP_LOGCONFIG(TAG, "  Measurement Period: %us", this->measurement_period_);
  ESP_LOGCONFIG(TAG, "  IIR Filter: %s", this->iir_filter_ ? "enabled" : "disabled");
  LOG_SENSOR("  ", "CO2", this->co2_sensor_);
  LOG_SENSOR("  ", "Temperature", this->temperature_sensor_);
}

void SenseairSunriseComponent::background_calibration() {
  ESP_LOGI(TAG, "Initiating background calibration...");
  this->wake_up_();
  uint8_t cmd[2] = {0x7C, 0x06};
  if (!this->write_register_(0x82, cmd, 2)) {
    ESP_LOGE(TAG, "Failed to send background calibration command");
    return;
  }
  ESP_LOGI(TAG, "Background calibration command sent");
}

void SenseairSunriseComponent::abc_enable() {
  ESP_LOGI(TAG, "Enabling ABC...");
  this->wake_up_();
  uint8_t meter_control;
  if (!this->read_register_(0xA5, &meter_control, 1)) {
    ESP_LOGE(TAG, "Failed to read MeterControl register");
    return;
  }
  meter_control &= ~0x02;
  if (!this->write_register_(0xA5, &meter_control, 1)) {
    ESP_LOGE(TAG, "Failed to write MeterControl register");
    return;
  }
  ESP_LOGI(TAG, "ABC enabled");
}

void SenseairSunriseComponent::abc_disable() {
  ESP_LOGI(TAG, "Disabling ABC...");
  this->wake_up_();
  uint8_t meter_control;
  if (!this->read_register_(0xA5, &meter_control, 1)) {
    ESP_LOGE(TAG, "Failed to read MeterControl register");
    return;
  }
  meter_control |= 0x02;
  if (!this->write_register_(0xA5, &meter_control, 1)) {
    ESP_LOGE(TAG, "Failed to write MeterControl register");
    return;
  }
  ESP_LOGI(TAG, "ABC disabled");
}

}  // namespace senseair_sunrise
}  // namespace esphome
