#pragma once

#include "esphome/core/component.h"
#include "esphome/core/automation.h"
#include "esphome/core/hal.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/i2c/i2c.h"
#include "esphome/core/preferences.h"

namespace esphome {
namespace senseair_sunrise {

// Sensor algorithm state preserved across power cycles in single-measurement
// mode.  Covers registers 0xC4-0xDF:
//   0xC4-0xC5  ABC time
//   0xC6-0xCD  ABC parameters
//   0xCE-0xDB  IIR filter parameters
//   0xDC-0xDD  barometric pressure (overwritten by explicit write each cycle)
//   0xDE-0xDF  ABC barometric pressure
struct SunriseSavedState {
  uint8_t block[28];      // registers 0xC4-0xDF
  uint32_t config_hash;   // fingerprint of settings that invalidate state
  uint32_t crc;           // CRC32 over block[] + config_hash
} __attribute__((packed));

class SenseairSunriseComponent : public PollingComponent, public i2c::I2CDevice {
 public:
  void setup() override;
  void update() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  void set_co2_sensor(sensor::Sensor *co2_sensor) { this->co2_sensor_ = co2_sensor; }
  void set_temperature_sensor(sensor::Sensor *temperature_sensor) { this->temperature_sensor_ = temperature_sensor; }

  void set_measurement_mode(uint8_t mode) { this->measurement_mode_ = mode; }
  void set_nrdy_pin(GPIOPin *pin) { this->nrdy_pin_ = pin; }
  void set_number_of_samples(uint16_t samples) { this->number_of_samples_ = samples; }
  void set_measurement_period(uint16_t period) { this->measurement_period_ = period; }
  void set_iir_filter(bool enabled) { this->iir_filter_ = enabled; }
  void set_abc_period(uint16_t hours) { this->abc_period_ = hours; }
  void set_abc_target(uint16_t target) { this->abc_target_ = target; }
  void set_pressure_compensation(bool enabled) { this->pressure_compensation_ = enabled; }
  void set_pressure_source(sensor::Sensor *source) { this->pressure_source_ = source; }
  void set_pressure_value(int16_t value) { this->pressure_value_ = value; }

  void background_calibration();
  void dump_registers();
  void abc_enable();
  void abc_disable();

 protected:
  bool wake_up_();
  bool read_register_(uint8_t reg, uint8_t *data, size_t len);
  bool write_register_(uint8_t reg, const uint8_t *data, size_t len);
  bool trigger_single_measurement_();

  sensor::Sensor *co2_sensor_{nullptr};
  sensor::Sensor *temperature_sensor_{nullptr};
  GPIOPin *nrdy_pin_{nullptr};
  bool nrdy_enabled_{true};
  bool nrdy_active_high_{true};
  uint8_t measurement_mode_{0};
  uint16_t number_of_samples_{8};
  uint16_t measurement_period_{16};
  bool iir_filter_{true};
  uint16_t abc_period_{0};  // 0 = not configured (use sensor default)
  uint16_t abc_target_{0};  // 0 = not configured (use sensor default)
  bool pressure_compensation_{false};
  sensor::Sensor *pressure_source_{nullptr};
  int16_t pressure_value_{0};  // static pressure in 0.1 hPa units, 0 = unused

  ESPPreferenceObject state_pref_;
  bool state_valid_{false};
  SunriseSavedState saved_state_{};

  uint32_t compute_config_hash_() const;
  uint32_t compute_state_crc_(const SunriseSavedState &state) const;
};

template<typename... Ts> class BackgroundCalibrationAction : public Action<Ts...> {
 public:
  BackgroundCalibrationAction(SenseairSunriseComponent *parent) : parent_(parent) {}
  void play(const Ts &...x) override { this->parent_->background_calibration(); }

 protected:
  SenseairSunriseComponent *parent_;
};

template<typename... Ts> class DumpRegistersAction : public Action<Ts...> {
 public:
  DumpRegistersAction(SenseairSunriseComponent *parent) : parent_(parent) {}
  void play(const Ts &...x) override { this->parent_->dump_registers(); }

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
