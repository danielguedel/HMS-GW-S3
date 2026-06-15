# HMS-GW-S3

**ESP32-S3 Gateway for Hoymiles HMS-800W-2T Solar Inverter**

[![PlatformIO](https://img.shields.io/badge/PlatformIO-ESP32--S3-orange)](https://platformio.org)
[![Framework](https://img.shields.io/badge/Framework-Arduino%20%2B%20FreeRTOS-blue)](https://docs.espressif.com/projects/esp-idf)
[![License](https://img.shields.io/badge/License-Apache%202.0-green)](LICENSE)

Reads live data from the Hoymiles HMS-800W-2T solar inverter, visualises it on a web dashboard, forwards it via MQTT to Home Assistant, and is fully configurable through a web GUI — running on the **ESP32-S3-DEVKITC-1-N8R8**.

Based on [dtuGateway](https://github.com/ohAnd/dtuGateway) by ohAnd (Apache 2.0).

---

## Features

| Feature | Details |
|---|---|
| ⚡ Solar data | PV1/PV2 power, voltage, current · Grid feed-in · Daily & total energy · Temperature |
| 🌐 Web dashboard | Live data, GPIO controls, config tabs — responsive Dark Mode SPA at `http://<ip>` |
| 📡 MQTT | Full publish/subscribe · Home Assistant auto-discovery · OpenDTU-compatible mode |
| 🔌 REST API | JSON endpoints: `/api/data.json`, `/api/info.json`, `/api/gpio`, `/api/config` |
| 🌈 NeoPixel LED | Onboard WS2812B (GPIO38) — 11 states via colour & animation |
| 🔀 Relay + 4 GPIO | Switchable via Web GUI, REST API and MQTT · GPIO2/3 reserved for future I2C sensors |
| 🔧 Web config | All settings in browser — WiFi, DTU, MQTT, GPIO, System |
| 🔄 OTA updates | Firmware and filesystem update via web file upload or HTTP URL |
| 🖥️ Serial console | Structured log output `[HH:MM:SS.mmm] [LVL] [MODULE]` + 18 commands at 115200 baud |
| 🧵 FreeRTOS | 8 independent tasks on Core 1 — Core 0 reserved for WiFi stack |
| 🗄️ DataStore | Central in-memory data store — no direct task-to-task dependencies |

---

## Hardware

**Board:** ESP32-S3-DEVKITC-1-N8R8 (Espressif) — 8 MB Flash, 8 MB PSRAM

### GPIO Default Pinout

| GPIO | Function | Direction | Configurable |
|---|---|---|---|
| 38 | NeoPixel LED (onboard WS2812B) | Output | ✅ |
| 1 | Relay output | Output (ext. driver required) | ✅ |
| 0 | GP1 / BOOT button | Input (factory reset: hold > 5 s) | ✅ |
| 2 | GP2 — I2C SDA (reserved) | Reserved for future I2C sensors | ✅ |
| 3 | GP3 — I2C SCL (reserved) | Reserved for future I2C sensors | ✅ |
| — | GP4 | Unassigned | ✅ |
| 43/44 | Serial TX/RX | Console 115200 baud | — |

> All GPIO assignments (except Serial TX/RX) are configurable via the web GUI and saved to `config.json`.

> GPIO2 and GPIO3 are reserved for future I2C sensor support (temperature, humidity etc.) and initialised as high-impedance inputs.

---

## Quick Start

### 1. Prerequisites

- [PlatformIO](https://platformio.org) (VSCode extension or CLI)

### 2. Clone and build

```bash
git clone https://github.com/danielguedel/HMS-GW-S3.git
cd HMS-GW-S3

# Build firmware
pio run -e esp32-s3-devkitc-1

# Flash web assets (LittleFS) — required on first flash
pio run -e esp32-s3-devkitc-1 -t uploadfs

# Flash firmware
pio run -e esp32-s3-devkitc-1 -t upload

# Open serial monitor (COM14 pre-configured)
pio device monitor
```

### 3. First-time factory flash

> ⚠️ **ESP32-S3: Bootloader is at address `0x0` — not `0x1000` like classic ESP32.**

```bash
esptool.py --chip esp32s3 --baud 921600 \
  --before default_reset --after hard_reset write_flash \
  0x0      .pio/build/esp32-s3-devkitc-1/bootloader.bin \
  0x8000   .pio/build/esp32-s3-devkitc-1/partitions.bin \
  0xe000   ~/.platformio/packages/framework-arduinoespressif32/tools/partitions/boot_app0.bin \
  0x10000  .pio/build/esp32-s3-devkitc-1/firmware.bin
```

### 4. Initial setup

1. Connect to Wi-Fi: **`HMS-GW-S3`** (no password)
2. Captive portal opens automatically → configure your Wi-Fi SSID/password and DTU IP address
3. Device connects and data starts flowing within ~35 seconds (NTP sync + DTU handshake)

### 5. Access

| URL | Description |
|---|---|
| `http://<device-ip>` | Dashboard (Dark Mode) |
| `http://hmsgws3.local` | mDNS (if supported by your OS) |
| `http://<ip>/api/data.json` | Live inverter data (JSON) |
| `http://<ip>/api/info.json` | System info (JSON) |
| `http://<ip>/api/gpio` | GPIO state (GET/POST JSON) |
| `http://<ip>/update` | OTA firmware upload |
| `http://<ip>/updatefs` | OTA filesystem upload |

---

## LED States (GPIO38, WS2812B)

| State | Colour | Animation | Meaning |
|---|---|---|---|
| Boot | White | 3× flash | Starting up |
| WiFi connecting | Blue | 1 Hz blink | Connecting to WiFi |
| AP mode | Blue | Triple blink + pause | Needs configuration |
| DTU offline | Orange | Double blink | WiFi ok, inverter unreachable |
| No MQTT | Cyan | 4 s pulse | WiFi + DTU ok, MQTT disconnected |
| Operational | Green | 5 s heartbeat | Everything working |
| Standby | Green (dim) | 10 s slow pulse | No PV output (night) |
| Data received | White | 80 ms flash | New measurement arrived |
| OTA update | Magenta | 5 Hz fast blink | Firmware update in progress |
| Error | Red | 4 Hz blink | Critical error |
| Factory reset | Red | Steady | Config erased, rebooting |

---

## Serial Console

**115200 baud, 8N1** — connect via USB or 3.3 V UART adapter on GPIO43/44.

```
[00:00:01.906] [INF] [WIFI  ] Connected — IP: 10.1.1.145  RSSI: -42 dBm
[00:00:05.840] [INF] [DTU   ] NTP time: 1781430913
[00:00:06.923] [INF] [DTU   ] TCP connected — sent AppInfo immediately (24 bytes)
[00:00:31.295] [INF] [DATA  ] PV1: 26.6V/1.33A/353W  PV2: 27.7V/1.07A/296W
[00:00:31.295] [INF] [DATA  ] Grid: 241.9V/2.53A/610W  Temp: 45.2°C
[00:00:31.306] [INF] [DATA  ] Energy today: 1.591kWh  Total: 129.164kWh
```

### Commands

| Command | Description |
|---|---|
| `help` | List all commands |
| `version` | Firmware version and build info |
| `status` | System status (WiFi, DTU, MQTT, heap, uptime) |
| `wifi` | WiFi detail (IP, RSSI, MAC, SSID) |
| `dtu` | DTU status and last PV data |
| `mqtt` | MQTT status and config |
| `gpio` | GPIO states |
| `config` | Current configuration |
| `relay on\|off` | Set relay |
| `gpio1 on\|off` | Set GP1 |
| `gpio2 on\|off` | Set GP2 |
| `gpio3 on\|off` | Set GP3 |
| `gpio4 on\|off` | Set GP4 |
| `loglevel error\|warn\|info\|debug` | Set log level |
| `ledtest` | Cycle through all LED states |
| `restart` | Reboot gateway |
| `reset` | Factory reset (clears config.json) |

---

## MQTT Topics

All topics under `<mqttTopic>/` (default: `hmsgws3_XXXXXX`).

### Publish

| Topic | Value | Trigger |
|---|---|---|
| `grid/P`, `grid/U`, `grid/I` | float | Every DTU update |
| `grid/dailyEnergy`, `grid/totalEnergy` | float kWh | Every DTU update |
| `pv0/P`, `pv0/U`, `pv0/I`, `pv0/dailyEnergy`, `pv0/totalEnergy` | float | Every DTU update |
| `pv1/P`, `pv1/U`, `pv1/I`, `pv1/dailyEnergy`, `pv1/totalEnergy` | float | Every DTU update |
| `inverter/Temp` | float °C | Every DTU update |
| `inverter/PowerLimit` | int % | Every DTU update |
| `inverter/warningsActive` | int | Every DTU update |
| `relay/state`, `gpio1/state` … `gpio4/state` | 0 or 1 | On change |
| `system/uptime`, `system/rssi`, `system/heap` | int | Every 60 s |
| `system/status` | `online` / `offline` | Connect / LWT |

### Subscribe (control)

| Topic | Value | Action |
|---|---|---|
| `inverter/PowerLimitSet/set` | 2–100 | Set power limit (auto-resets after timeout if configured) |
| `inverter/RebootDtu/set` | 1 | Reboot DTU |
| `inverter/RebootGw/set` | 1 | Reboot gateway |
| `inverter/On/set` | 0 or 1 | Inverter on/off |
| `relay/set` | 0 or 1 | Switch relay |
| `gpio1/set` … `gpio4/set` | 0 or 1 | Set GPIO output |

### Home Assistant Auto-Discovery

Enable `mqttHaDiscovery` in config. Entities are published automatically 5 seconds after MQTT connect, one per 500 ms. All sensors and switches appear under the `HMS-800W-2T` device in HA.

### Power Limit Timeout

If `powerLimitTimeout > 0` (seconds), any power limit set via MQTT or web GUI will automatically reset to `powerLimitDefault` (100%) after the timeout — protecting against permanent throttling if the controller loses connection.

---

## Architecture

### DataStore Pattern

All tasks communicate exclusively through a central in-memory DataStore — no direct task-to-task dependencies:

```
taskDTU    ──► DataStore ◄── taskMQTT
taskWiFi   ──►           ◄── taskWebServer
taskGPIO   ──►           ◄── taskSerial
taskSysMonitor ──►       ◄── taskLED
```

### FreeRTOS Task Overview

| Task | Core | Priority | Stack | Function |
|---|---|---|---|---|
| taskWiFi | 1 | 5 | 6144 | WiFi connection, AP mode, NTP sync |
| taskDTU | 1 | 4 | 8192 | DTU TCP/Protobuf, data polling |
| taskGPIO | 1 | 4 | 4096 | Relay, GP1–GP4, factory reset |
| taskMQTT | 1 | 3 | 6144 | MQTT publish/subscribe, HA discovery |
| taskWebServer | 1 | 3 | 8192 | HTTP server, REST API, OTA, captive portal |
| taskLED | 1 | 2 | 3072 | NeoPixel state machine |
| taskSerial | 1 | 2 | 4096 | Serial console commands |
| taskSysMonitor | 1 | 1 | 3072 | Heap monitor, uptime |

> **Core 0** is reserved exclusively for the ESP32-S3 WiFi stack (lwIP). All user tasks run on **Core 1**.

### DTU Protocol

The Hoymiles HMS-800W-2T communicates over raw TCP (port 10081) using a 10-byte header + Protocol Buffers payload:

```
Byte 0–1:  0x48 0x4D          (magic)
Byte 2–3:  [cmd0] [cmd1]      (command)
Byte 4–5:  0x00 0x01
Byte 6–7:  CRC16-MODBUS       (over payload only)
Byte 8–9:  total length       (10 + payload bytes)
Byte 10+:  Protobuf payload
```

Poll sequence per cycle: `AppInfo (0xa3 0x01)` → `RealDataNew (0xa3 0x11)` → `GetConfig (0xa3 0x09)`

The DTU syncs with Hoymiles cloud every 5 minutes (~30 s window). During this time new connections are rejected with TCP RST. The gateway automatically pauses and retries after the configurable `dtuCloudPause` delay.

---

## Project Structure

```
HMS-GW-S3/
├── src/
│   ├── main.cpp              # FreeRTOS setup, task launch, DataStore init
│   ├── appConfig.cpp         # JSON config (LittleFS load/save)
│   ├── dataStore.cpp         # Central in-memory DataStore
│   ├── logger.cpp            # Structured serial logger [HH:MM:SS.mmm]
│   ├── taskWiFi.cpp          # WiFi + NTP
│   ├── taskDTU.cpp           # Hoymiles TCP/Protobuf communication
│   ├── taskMQTT.cpp          # MQTT (esp-mqtt, non-blocking)
│   ├── taskWebServer.cpp     # Async HTTP server, REST API, OTA, captive portal
│   ├── taskNeoPixel.cpp      # WS2812B LED state machine
│   ├── taskGPIO.cpp          # Relay & GPIO, debounce, factory reset
│   ├── taskSerial.cpp        # Serial console commands
│   └── taskSysMonitor.cpp    # Heap monitor, uptime
├── include/
│   ├── config.h              # Pin defaults, stack sizes, task priorities
│   ├── appConfig.h           # AppConfig struct
│   ├── dataStore.h           # DataStore struct + API
│   ├── systemState.h         # EventGroup bits, LED state enum
│   ├── logger.h              # LOG_I/W/E/D macros
│   └── proto/                # Hoymiles Protobuf definitions
├── data/www/
│   └── index.html            # Web dashboard SPA (LittleFS)
├── docs/
│   └── HMS-GW-S3_Spezifikation_v2.md  # Full architecture specification
├── platformio.ini
├── custom_partitions.csv
└── version_inc.py
```

---

## Compatibility

Requires a Hoymiles **HMS-xxxW-2T** inverter with **integrated Wi-Fi DTU** (built into the inverter body). External DTU sticks (DTU-Lite, DTU-Pro) are **not supported**.

Tested with: HMS-800W-2T firmware 01.00.xx

---

## Based On

- [dtuGateway](https://github.com/ohAnd/dtuGateway) by ohAnd — Apache 2.0
- Hoymiles Protobuf protocol from dtuGateway community research

## License

Apache License 2.0 — see [LICENSE](LICENSE)
