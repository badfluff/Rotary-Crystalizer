# Rotary Crystalizer

ESP32-based hot-plate temperature controller for a small crystallization system.

The project contains firmware, a browser dashboard, and optional logging tools for capturing temperature, target, heating power, and PID-term telemetry.

## Features

- ESP32 firmware with PID temperature control
- Rolling-window derivative smoothing for stable D-term behavior
- Asymmetric derivative gains for rising/falling temperature response
- Banded integrator so the integral term only learns near the setpoint
- Web UI served from LittleFS for live monitoring and setpoint control
- WebSocket streaming of temperature, target, heating power, and PID terms
- Browser logging to IndexedDB with export to CSV
- Optional Python logger for remote WebSocket capture into SQLite

## Repository Layout

- `platformio.ini` - PlatformIO configuration for `esp32doit-devkit-v1`
- `src/` - Firmware source code
  - `main.cpp` - ESP32 app entry point, network + web server, control loop
  - `PIDController.hpp` - PID implementation with derivative window and integrator band
  - `TemperatureController.hpp` - target wrapper for the PID controller
  - `Secrets.hpp` - excluded credentials and network secrets
- `data/` - Web dashboard assets
  - `index.html` - dashboard UI
  - `style.css` - dashboard styling
  - `script.js` - live telemetry, logging, and IndexedDB storage
- `tools/esp32_logger.py` - optional Python WebSocket logger for remote recording
- `analyze/` - analysis tools and logs

## Setup

### 1. Fill in secrets

Create or edit `src/Secrets.hpp` with your network credentials:

```cpp
#pragma once

static const char *WIFI_SSID_STR = "your-ssid";
static const char *WIFI_PASSWORD_STR = "your-password";
```

`src/Secrets.hpp` is excluded from Git in `.gitignore`, so your credentials stay local.

### 2. Build and upload firmware

Use PlatformIO from the project root:

```bash
pio run -e esp32doit-devkit-v1 --target upload
pio run -e esp32doit-devkit-v1 --target uploadfs
```

The second command uploads the web UI files to LittleFS.

### 3. Access the dashboard

Once the ESP32 is on the network, find its IP from the serial console and open it in a browser.

The dashboard provides:

- current temperature
- target temperature
- heating power
- P-term, D-term, and I-term telemetry
- browser-based logging sessions

## Logging

### Browser logging

The dashboard can save telemetry into IndexedDB. Use the built-in "Begin Logging" button, then export sessions as CSV.

### Python logger

Use the optional logger to record directly from the ESP32 WebSocket stream:

```bash
python3 tools/esp32_logger.py [host]
```

Once running, use commands such as `start`, `stop`, `list`, and `export`.

## Tuning notes

The PID controller is tuned for a hot-plate/water system with a relatively slow thermal response. Important design decisions include:

- `PID_KP` is sized so the P term drops below the steady-state hold power before the setpoint is reached.
- `PID_KI` only integrates inside a band around the setpoint to avoid windup during warm-up.
- `PID_KD_RISING` and `PID_KD_FALLING` are separate to reflect the asymmetry between heating and cooling.
- The derivative term is estimated over a rolling 30-second window.

## Notes

- `src/Secrets.hpp` is intentionally ignored by Git; do not commit your Wi-Fi credentials.
- The current implementation uses a direct target error model with no setpoint ramp.

## Development

- Add new web UI features in `data/script.js` and `data/index.html`
- Tune PID behavior in `src/main.cpp` and `src/PIDController.hpp`
- Use `tools/esp32_logger.py` to capture and inspect runtime behavior remotely

## License

This repository does not include a license file. Add one if you want to make the project reusable by others.
