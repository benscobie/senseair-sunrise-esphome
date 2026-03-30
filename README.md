# Senseair Sunrise CO2 Sensor — ESPHome Component

Custom [ESPHome](https://esphome.io/) component for the [Senseair Sunrise](https://senseair.com/product/sunrise/) NDIR CO2 sensor over I2C.

Supports continuous and single (battery-optimized) measurement modes, automatic baseline correction (ABC), pressure compensation, and deep sleep.

> **Tested on Sunrise 006-0-0007.** The sensor comes in three revisions (006-0-0002, 006-0-0007, 006-0-0008) — other revisions may work but are untested.

## Installation

Add this to your ESPHome YAML:

```yaml
external_components:
  - source:
      type: git
      url: https://github.com/benscobie/senseair-sunrise-esphome
    components: [senseair_sunrise]
```

## Wiring

ESP8266 D1 Mini to Sunrise pinout:

| D1 Mini | Sunrise | Function |
|---------|---------|----------|
| 3V3 | Pin 2 (VBB), Pin 3 (VDDIO), Pin 9 (EN) | Power |
| GND | Pin 1 (GND), Pin 6 (COMSEL) | Ground + I2C mode select |
| D2 | Pin 4 (SDA) | I2C data |
| D1 | Pin 5 (SCL) | I2C clock |
| D5 | Pin 7 (nRDY) | Measurement ready (optional) |
| — | Pin 8 (DVCC) | Leave floating |

For ESP32, use the appropriate I2C pins (e.g. GPIO21/GPIO22).

## Examples

Three example configs are included:

- **[example.yaml](example.yaml)** — Continuous mode, ESPHome native API
- **[example_api_battery.yaml](example_api_battery.yaml)** — Single mode + deep sleep, ESPHome native API
- **[example_mqtt_battery.yaml](example_mqtt_battery.yaml)** — Single mode + deep sleep, MQTT (for Home Assistant MQTT integration)

Copy `secrets.yaml.example` to `secrets.yaml` and fill in your values before compiling.

## Configuration

### Sensor

```yaml
sensor:
  - platform: senseair_sunrise
    id: my_sunrise
    co2:
      name: "CO2"
    temperature:
      name: "Sensor Temperature"
```

### Measurement mode

| Option | Default | Description |
|--------|---------|-------------|
| `measurement_mode` | `continuous` | `continuous` or `single`. Single mode is for battery/deep-sleep use. |
| `number_of_samples` | `8` | Samples per measurement (1–1024). More = better accuracy, longer measurement. |
| `measurement_period` | `16s` | Continuous mode only. Minimum = ceil(samples x 300ms), must be even seconds. |
| `iir_filter` | auto | Auto-disabled when measurement_period > 60s. Disable manually for single mode with long intervals. |
| `nrdy_pin` | — | Optional GPIO for measurement-ready signal. Useful in single mode. |

### ABC (Automatic Baseline Correction)

| Option | Default | Description |
|--------|---------|-------------|
| `abc_period` | sensor default | Recalibration interval (1h–65535h). |
| `abc_target` | sensor default | Reference CO2 level in ppm (350–2000). |

### Pressure compensation

Pick one (mutually exclusive):

| Option | Description |
|--------|-------------|
| `pressure_source` | ID of a pressure sensor providing hPa readings. |
| `pressure` | Static pressure in hPa (300–1300). |
| `altitude` | Altitude in meters, auto-converted to pressure. |

### Switch

Toggle ABC on/off at runtime:

```yaml
switch:
  - platform: senseair_sunrise
    senseair_sunrise_id: my_sunrise
    abc:
      name: "ABC"
```

### Actions

Available for use in automations:

- `senseair_sunrise.background_calibration` — Zero-point calibration (requires clean air)
- `senseair_sunrise.abc_enable` / `senseair_sunrise.abc_disable` — Toggle ABC
- `senseair_sunrise.dump_registers` — Log all sensor registers (diagnostics)

## Vendor Documentation

- [PSP11704 — I2C communication description](https://rmtplusstoragesenseair.blob.core.windows.net/docs/Dev/publicerat/PSP11704.pdf)
- [TDE5531 — I2C register description](https://rmtplusstoragesenseair.blob.core.windows.net/docs/Dev/publicerat/TDE5531.pdf)
- [TDE7318 — Sensor module description](https://rmtplusstoragesenseair.blob.core.windows.net/docs/Market/publicerat/TDE7318.pdf)

Source: [senseair.com/product/sunrise](https://senseair.com/product/sunrise/)

## License

[MIT](LICENSE)
