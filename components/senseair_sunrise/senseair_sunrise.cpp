#include "senseair_sunrise.h"
#include "esphome/core/log.h"

namespace esphome {
namespace senseair_sunrise {

static const char *const TAG = "senseair_sunrise";

void SenseairSunriseComponent::setup() {
  ESP_LOGCONFIG(TAG, "Setting up Senseair Sunrise...");
}

void SenseairSunriseComponent::update() {
  ESP_LOGD(TAG, "Update called (stub)");
}

void SenseairSunriseComponent::dump_config() {
  ESP_LOGCONFIG(TAG, "Senseair Sunrise:");
  LOG_I2C_DEVICE(this);
  LOG_UPDATE_INTERVAL(this);
  LOG_SENSOR("  ", "CO2", this->co2_sensor_);
}

}  // namespace senseair_sunrise
}  // namespace esphome
