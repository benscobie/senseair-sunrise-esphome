#pragma once

#include "esphome/core/component.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/i2c/i2c.h"

namespace esphome {
namespace senseair_sunrise {

class SenseairSunriseComponent : public PollingComponent, public i2c::I2CDevice {
 public:
  void setup() override;
  void update() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  void set_co2_sensor(sensor::Sensor *co2_sensor) { this->co2_sensor_ = co2_sensor; }

 protected:
  bool wake_up_();
  bool read_register_(uint8_t reg, uint8_t *data, size_t len);
  bool write_register_(uint8_t reg, const uint8_t *data, size_t len);

  sensor::Sensor *co2_sensor_{nullptr};
};

}  // namespace senseair_sunrise
}  // namespace esphome
