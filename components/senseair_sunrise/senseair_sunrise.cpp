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
  uint8_t id_data[4];
  if (this->read_register_(0x3A, id_data, 4)) {
    uint32_t sensor_id = ((uint32_t) id_data[0] << 24) | ((uint32_t) id_data[1] << 16) |
                         ((uint32_t) id_data[2] << 8) | id_data[3];
    ESP_LOGI(TAG, "Sensor ID: %u", sensor_id);
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

  // Parse error status
  uint16_t error_status = ((uint16_t) data[0] << 8) | data[1];
  if (error_status & 0x0080) {
    // Bit 7 of LSB: no measurement completed yet
    ESP_LOGD(TAG, "No measurement completed yet, skipping");
    return;
  }
  if (error_status != 0) {
    ESP_LOGW(TAG, "Sensor error status: 0x%04X", error_status);
  }

  // Parse CO2 (signed 16-bit, registers 0x06-0x07)
  int16_t co2_raw = ((int16_t) data[6] << 8) | data[7];
  if (co2_raw >= 0 && this->co2_sensor_ != nullptr) {
    this->co2_sensor_->publish_state(co2_raw);
  }

  this->status_clear_warning();
}

void SenseairSunriseComponent::dump_config() {
  ESP_LOGCONFIG(TAG, "Senseair Sunrise:");
  LOG_I2C_DEVICE(this);
  LOG_UPDATE_INTERVAL(this);
  LOG_SENSOR("  ", "CO2", this->co2_sensor_);
}

}  // namespace senseair_sunrise
}  // namespace esphome
