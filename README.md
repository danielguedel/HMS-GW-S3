# HMS-GW-S3

**ESP32-S3 Gateway for Hoymiles HMS-800W-2T Solar Inverter**

[![PlatformIO](https://img.shields.io/badge/PlatformIO-ESP32--S3-orange)](https://platformio.org)
[![Framework](https://img.shields.io/badge/Framework-Arduino%20%2B%20FreeRTOS-blue)](https://docs.espressif.com/projects/esp-idf)
[![License](https://img.shields.io/badge/License-Apache%202.0-green)](LICENSE)

Reads live data from the Hoymiles HMS-800W-2T solar inverter, visualises it on an integrated web server, forwards it via MQTT, and is fully configurable through a web GUI — running on the **ESP32-S3-DEVKITC-1U-N8R8**.

Based on [dtuGateway](https://github.com/ohAnd/dtuGateway) by ohAnd.

---

## Features

- **Solar Data** — Real-time PV power, voltage, current, daily & total energy, temperature
- **Web Dashboard** — Live visualisation at `http://<device-ip>`
- **MQTT** — Full publish/subscribe, Home Assistant auto-discovery, OpenDTU-compatible mode
- **REST API** — JSON endpoints for all data and control
- **NeoPixel Status LED** — Onboard WS2812B (GPIO48) shows all operating states via colour & animation
- **Relay + 4 GPIO** — Controllable via Web GUI, REST API and MQTT
- **Web GUI Config** — All settings configurable in browser, no re-flash needed
- **OTA Updates** — Firmware update via web file upload or HTTP pull
- **Serial Console** — Structured logging + full command set at 115200 baud
- **FreeRTOS** — All functions as independent tasks, dual-core optimised

---

## Hardware

| Component | Details |
|---|---|
| Board | ESP32-S3-DEVKITC-1U-N8R8 (Espressif) |
| SoC | ESP32-S3, Xtensa LX7 Dual-Core @ 240 MHz |
| Flash | 8 MB Quad-SPI |
| PSRAM | 8 MB Octal-SPI |
| NeoPixel | GPIO48 (onboard WS2812B) |
| Relay | GPIO1 (external driver required) |
| Digital I/O | GPIO2–5 (configurable in/out) |

### GPIO Pinout

| GPIO | Function |
|---|---|
| 48 | NeoPixel LED (onboard) |
| 1 | Relay output |
| 2–5 | Digital I/O 1–4 |
| 43/44 | Serial TX/RX (console) |
| 0 | BOOT button (factory reset > 5 s) |

---

## Quick Start

### 1. Flash firmware

```bash
# First-time flash (ESP32-S3: bootloader at 0x0, not 0x1000!)
esptool.py --chip esp32s3 --baud 921600 write_flash \
  0x0      bootloader.bin \
  0x8000   partitions.bin \
  0xe000   boot_app0.bin \
  0x10000  firmware.bin
```

### 2. Initial setup

1. Connect to Wi-Fi: **`HMS-GW-S3_XXXXXX`**
2. Captive portal opens automatically → configure your Wi-Fi and DTU IP
3. Device connects and starts serving data

### 3. Access

- **Dashboard:** `http://<device-ip>` or `http://hmsgws3.local`
- **Config:** `http://<device-ip>/config`
- **API:** `http://<device-ip>/api/data.json`

---

## NeoPixel LED States

| State | Colour | Animation |
|---|---|---|
| Boot | White | Blinking 2 Hz |
| Wi-Fi connecting | Blue | Fast blink 4 Hz |
| AP mode | Yellow | Slow blink 0.5 Hz |
| Wi-Fi ok, DTU offline | Orange | Double blink 1 Hz |
| Wi-Fi + DTU ok, no MQTT | Cyan | Pulsing 0.5 Hz |
| Fully operational | Green | Heartbeat 0.2 Hz |
| Data received | Bright green | 100 ms flash |
| OTA update | Magenta | Fast blink 5 Hz |
| Critical error | Red | SOS pattern |
| Factory reset | Red | 10× fast blink |
| Standby (0 W) | Dark blue | Steady |

---

## Serial Console

**115200 baud, 8N1**

```
[HH:MM:SS.mmm] [INF] [DTU   ] Connected. Firmware: 01.00.08, Model: HMS-800W-2T
[HH:MM:SS.mmm] [INF] [DATA  ] PV1: 35.8V / 8.2A / 293W   PV2: 36.1V / 7.9A / 285W
[HH:MM:SS.mmm] [INF] [DATA  ] Grid: 230.2V / 2.45A / 564W  Temp: 24.5°C  Limit: 80%
```

Key commands: `status`, `data`, `setPower <W>`, `setRelay <0|1>`, `setGPIO <1-4> <0|1>`, `reboot`, `resetToFactory 1`, `ledTest`, `sysinfo`

---

## MQTT Topics

All topics under `<MainTopic>/` (default: `hmsgws3_XXXXXX`):

**Publish:** `grid/P`, `grid/U`, `grid/I`, `grid/dailyEnergy`, `pv0/P`, `pv1/P`, `inverter/Temp`, `inverter/PowerLimit`, `relay/state`, `gpio1/state` … `gpio4/state`

**Subscribe (control):** `inverter/PowerLimitSet/set`, `relay/set`, `gpio1/set` … `gpio4/set`, `inverter/RebootDtu/set`, `inverter/RebootGw/set`

---

## Build (PlatformIO)

```bash
# Clone
git clone https://github.com/danielguedel/HMS-GW-S3.git
cd HMS-GW-S3

# Build
pio run -e esp32-s3-devkitc-1

# Flash firmware
pio run -e esp32-s3-devkitc-1 -t upload

# Flash filesystem (web assets)
pio run -e esp32-s3-devkitc-1 -t uploadfs

# Serial monitor
pio device monitor
```

---

## Project Structure

```
HMS-GW-S3/
├── src/
│   ├── main.cpp              # Setup, FreeRTOS task launch
│   ├── taskDTU.cpp/.h        # Hoymiles DTU communication (Protobuf/HTTP)
│   ├── taskMQTT.cpp/.h       # MQTT publish/subscribe
│   ├── taskWebServer.cpp/.h  # HTTP server, REST API, OTA
│   ├── taskNeoPixel.cpp/.h   # LED state machine
│   ├── taskGPIO.cpp/.h       # Relay & GPIO control
│   ├── taskSerial.cpp/.h     # Serial console
│   └── taskSysMonitor.cpp/.h # Watchdog, heap monitor
├── include/
│   ├── config.h              # Pin definitions, defaults, task config
│   ├── systemState.h         # Shared data structures, queues
│   └── proto/                # NanoPB .proto definitions
├── data/                     # LittleFS (web assets, config)
├── platformio.ini
├── custom_partitions.csv
└── version_inc.py
```

---

## Compatibility

Requires a Hoymiles HMS-xxxW-2T inverter with **integrated Wi-Fi DTU** (built into the inverter unit). External DTU sticks (DTU-Lite, DTU-Pro) are **not supported**.

---

## Based On

- [dtuGateway](https://github.com/ohAnd/dtuGateway) by ohAnd — Apache 2.0
- Protocol Buffers implementation adapted from dtuGateway

## License

Apache License 2.0 — see [LICENSE](LICENSE)
