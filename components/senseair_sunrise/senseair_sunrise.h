#pragma once

#include "esphome/core/component.h"
#include "esphome/core/automation.h"
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
  void set_temperature_sensor(sensor::Sensor *temperature_sensor) { this->temperature_sensor_ = temperature_sensor; }

  void set_measurement_mode(uint8_t mode) { this->measurement_mode_ = mode; }
  void set_number_of_samples(uint16_t samples) { this->number_of_samples_ = samples; }
  void set_measurement_period(uint16_t period) { this->measurement_period_ = period; }
  void set_iir_filter(bool enabled) { this->iir_filter_ = enabled; }

  void background_calibration();
  void abc_enable();
  void abc_disable();

 protected:
  bool wake_up_();
  bool read_register_(uint8_t reg, uint8_t *data, size_t len);
  bool write_register_(uint8_t reg, const uint8_t *data, size_t len);

  sensor::Sensor *co2_sensor_{nullptr};
  sensor::Sensor *temperature_sensor_{nullptr};
  uint8_t measurement_mode_{0};
  uint16_t number_of_samples_{8};
  uint16_t measurement_period_{4};
  bool iir_filter_{true};
};

template<typename... Ts> class BackgroundCalibrationAction : public Action<Ts...> {
 public:
  BackgroundCalibrationAction(SenseairSunriseComponent *parent) : parent_(parent) {}
  void play(const Ts &...x) override { this->parent_->background_calibration(); }

 protected:
  SenseairSunriseComponent *parent_;
};

template<typename... Ts> class ABCEnableAction : public Action<Ts...> {
 public:
  ABCEnableAction(SenseairSunriseComponent *parent) : parent_(parent) {}
  void play(const Ts &...x) override { this->parent_->abc_enable(); }

 protected:
  SenseairSunriseComponent *parent_;
};

template<typename... Ts> class ABCDisableAction : public Action<Ts...> {
 public:
  ABCDisableAction(SenseairSunriseComponent *parent) : parent_(parent) {}
  void play(const Ts &...x) override { this->parent_->abc_disable(); }

 protected:
  SenseairSunriseComponent *parent_;
};

}  // namespace senseair_sunrise
}  // namespace esphome
