# HMS-GW-S3

**ESP32-S3 Gateway for Hoymiles HMS-800W-2T Solar Inverter**

[![PlatformIO](https://img.shields.io/badge/PlatformIO-ESP32--S3-orange)](https://platformio.org)
[![Framework](https://img.shields.io/badge/Framework-Arduino%20%2B%20FreeRTOS-blue)](https://docs.espressif.com/projects/esp-idf)
[![License](https://img.shields.io/badge/License-Apache%202.0-green)](LICENSE)

Reads live data from the Hoymiles HMS-800W-2T solar inverter, visualises it on an integrated web dashboard, forwards it via MQTT, and is fully configurable through a web GUI — running on the **ESP32-S3-DEVKITC-1U-N8R8**.

Based on [dtuGateway](https://github.com/ohAnd/dtuGateway) by ohAnd (Apache 2.0).

---

## Features

| Feature | Details |
|---|---|
| ⚡ Solar data | PV1/PV2 power, voltage, current · Grid feed-in · Daily & total energy · Temperature |
| 🌐 Web dashboard | Live charts, GPIO controls, config tabs — responsive SPA at `http://<ip>` |
| 📡 MQTT | Full publish/subscribe · Home Assistant auto-discovery · OpenDTU-compatible mode |
| 🔌 REST API | JSON endpoints: `/api/data.json`, `/api/info.json`, `/api/gpio`, `/api/control` |
| 🌈 NeoPixel LED | Onboard WS2812B (GPIO48) — 11 states via colour & animation |
| 🔀 Relay + 4 GPIO | Switchable via Web GUI, REST API and MQTT |
| 🔧 Web config | All settings in browser — WiFi, DTU, MQTT, GPIO, System |
| 🔄 OTA updates | Firmware update via web file upload or HTTP pull |
| 🖥️ Serial console | Structured log output + 16 commands at 115200 baud |
| 🧵 FreeRTOS | 7 independent tasks, dual-core optimised |

---

## Hardware

**Board:** ESP32-S3-DEVKITC-1U-N8R8 (Espressif) — 8 MB Flash, 8 MB PSRAM

### GPIO Pinout

| GPIO | Function | Direction |
|---|---|---|
| 48 | NeoPixel LED (onboard WS2812B) | Output |
| 1 | Relay output | Output (ext. driver required) |
| 2 | Digital I/O 1 | In/Out (configurable) |
| 3 | Digital I/O 2 | In/Out (configurable) |
| 4 | Digital I/O 3 | In/Out (configurable) |
| 5 | Digital I/O 4 | In/Out (configurable) |
| 0 | BOOT button | Factory reset (hold > 5 s) |
| 43/44 | Serial TX/RX | Console 115200 baud |

> All GPIO assignments are configurable via `build_flags` in `platformio.ini`.

---

## Quick Start

### 1. Prerequisites

- [PlatformIO](https://platformio.org) (VSCode extension or CLI)
- Node.js (for `version_inc.py` — optional)

### 2. Clone and build

```bash
git clone https://github.com/danielguedel/HMS-GW-S3.git
cd HMS-GW-S3

# Build firmware
pio run -e esp32-s3-devkitc-1

# Flash firmware (after initial factory flash)
pio run -e esp32-s3-devkitc-1 -t upload

# Flash web assets (LittleFS)
pio run -e esp32-s3-devkitc-1 -t uploadfs

# Open serial monitor
pio device monitor
```

### 3. First-time factory flash

> ⚠️ **ESP32-S3: Bootloader is at address `0x0` — not `0x1000` like classic ESP32!**

```bash
esptool.py --chip esp32s3 --baud 921600 \
  --before default_reset --after hard_reset write_flash \
  0x0      bootloader.bin \
  0x8000   partitions.bin \
  0xe000   boot_app0.bin \
  0x10000  firmware.bin
```

### 4. Initial setup

1. Connect to Wi-Fi: **`HMS-GW-S3_XXXXXX`** (no password)
2. Captive portal opens automatically → configure your Wi-Fi and DTU IP
3. Device connects and data starts flowing within 31 seconds

### 5. Access

| URL | Description |
|---|---|
| `http://<device-ip>` | Dashboard |
| `http://hmsgws3.local` | mDNS (if supported by your OS) |
| `http://<ip>/api/data.json` | Live inverter data (JSON) |
| `http://<ip>/api/info.json` | System info (JSON) |
| `http://<ip>/api/gpio` | GPIO state (GET/POST JSON) |
| `http://<ip>/update` | OTA firmware upload |

---

## NeoPixel LED States

| State | Colour | Animation | Freq |
|---|---|---|---|
| Boot | White | Blink | 2 Hz |
| WiFi connecting | Blue | Fast blink | 4 Hz |
| AP mode | Yellow | Slow blink | 0.5 Hz |
| WiFi ok, DTU offline | Orange | Double blink | 1 Hz |
| WiFi + DTU ok, no MQTT | Cyan | Pulse | 0.5 Hz |
| Fully operational | Green | Heartbeat | 0.2 Hz |
| Data received | Bright green | 100 ms flash | on data |
| OTA update | Magenta | Fast blink | 5 Hz |
| Critical error | Red | SOS (···−−−···) | 1×/5 s |
| Factory reset | Red | 10× fast blink | 10 Hz |
| Standby (0 W) | Dark blue | Steady | – |

---

## Serial Console

**115200 baud, 8N1** — connect via USB or 3.3 V UART adapter.

```
[12:34:56.001] [INF] [DTU   ] Connected. Firmware: 01.00.08, Model: HMS-800W-2T
[12:34:56.032] [INF] [DATA  ] PV1: 35.8V / 8.2A / 293W   PV2: 36.1V / 7.9A / 285W
[12:34:56.033] [INF] [DATA  ] Grid: 230.2V / 2.45A / 564W  Temp: 24.5°C  Limit: 80%
```

### Commands

| Command | Description |
|---|---|
| `help` | List all commands |
| `status` | System status (WiFi, DTU, MQTT, GPIO) |
| `data` | Current inverter data |
| `sysinfo` | Heap, tasks, uptime |
| `setPower <W>` | Set power limit |
| `setRelay <0\|1>` | Switch relay |
| `setGPIO <1-4> <0\|1>` | Set GPIO output |
| `getGPIO <1-4>` | Read GPIO |
| `reboot` | Restart gateway |
| `resetToFactory 1` | Factory reset (deletes config) |
| `rebootDTU 1` | Request DTU reboot |
| `rebootInverter 1` | Request inverter reboot |
| `setInterval <s>` | Set poll interval (min 31) |
| `setLogLevel <0-3>` | Log level (0=ERR … 3=DBG) |
| `ledTest` | Cycle all LED states |
| `protectSettings <0\|1>` | Lock/unlock web config |

---

## MQTT Topics

All topics under `<MainTopic>/` (default: `hmsgws3_XXXXXX`):

### Publish

| Topic | Value | Trigger |
|---|---|---|
| `grid/P`, `grid/U`, `grid/I` | float | Every DTU update |
| `grid/dailyEnergy`, `grid/totalEnergy` | float kWh | Every DTU update |
| `pv0/P`, `pv0/U`, `pv0/I`, `pv0/dailyEnergy` | float | Every DTU update |
| `pv1/P`, `pv1/U`, `pv1/I`, `pv1/dailyEnergy` | float | Every DTU update |
| `inverter/Temp` | float °C | Every DTU update |
| `inverter/PowerLimit` | int % | Every DTU update |
| `inverter/dtuConnState` | 0 or 1 | On change |
| `relay/state` | 0 or 1 | On change |
| `gpio1/state` … `gpio4/state` | 0 or 1 | On change |
| `system/uptime`, `system/rssi`, `system/heap` | int | Every 60 s |

### Subscribe (control)

| Topic | Value | Action |
|---|---|---|
| `inverter/PowerLimitSet/set` | 2–100 | Set power limit |
| `inverter/RebootDtu/set` | 1 | Reboot DTU |
| `inverter/RebootGw/set` | 1 | Reboot gateway |
| `inverter/On/set` | 0 or 1 | Inverter on/off |
| `relay/set` | 0 or 1 | Switch relay |
| `gpio1/set` … `gpio4/set` | 0 or 1 | Set GPIO output |

---

## Project Structure

```
HMS-GW-S3/
├── src/
│   ├── main.cpp              # FreeRTOS setup, task launch
│   ├── appConfig.cpp/.h      # JSON config (LittleFS load/save)
│   ├── logger.cpp/.h         # Structured serial logger
│   ├── taskDTU.cpp/.h        # Hoymiles DTU communication (Protobuf/HTTP)
│   ├── taskMQTT.cpp/.h       # MQTT publish/subscribe + HA discovery
│   ├── taskWebServer.cpp/.h  # Async HTTP server, REST API, OTA, captive portal
│   ├── taskNeoPixel.cpp/.h   # WS2812B LED state machine
│   ├── taskGPIO.cpp/.h       # Relay & GPIO, debounce, factory reset
│   ├── taskSerial.cpp/.h     # Serial console commands
│   └── taskSysMonitor.cpp/.h # Watchdog, heap monitor, LED state manager
├── include/
│   ├── config.h              # Pin definitions, task config, defaults
│   ├── systemState.h         # Shared structs, FreeRTOS handles, event bits
│   ├── appConfig.h           # AppConfig struct
│   ├── logger.h              # LOG_I/W/E/D macros
│   └── proto/
│       ├── hoymiles.proto    # DTU protocol definition
│       └── hoymiles.pb.h     # NanoPB generated header
├── data/
│   └── www/
│       └── index.html        # Web dashboard SPA (LittleFS)
├── platformio.ini
├── custom_partitions.csv
├── version_inc.py
└── LICENSE
```

---

## FreeRTOS Task Overview

| Task | Core | Priority | Stack | Function |
|---|---|---|---|---|
| WebServer | 1 | 3 | 6144 | HTTP server, WiFi init, config load |
| NeoPixel | 1 | 2 | 2048 | LED state machine |
| GPIO | 1 | 3 | 2048 | Relay, GPIO, factory reset |
| Serial | 1 | 2 | 3072 | Console I/O |
| SysMonitor | 1 | 1 | 2048 | Watchdog, heap, LED state manager |
| DTU | 0 | 5 | 6144 | Hoymiles polling (Protobuf/HTTP) |
| MQTT | 0 | 4 | 4096 | Publish/subscribe, HA discovery |

---

## Compatibility

Requires a Hoymiles **HMS-xxxW-2T** inverter with **integrated Wi-Fi DTU** (built into the inverter). External DTU sticks (DTU-Lite, DTU-Pro) are **not supported**.

---

## Based On

- [dtuGateway](https://github.com/ohAnd/dtuGateway) by ohAnd — Apache 2.0
- Hoymiles protobuf protocol from dtuGateway community research

## License

Apache License 2.0 — see [LICENSE](LICENSE)
