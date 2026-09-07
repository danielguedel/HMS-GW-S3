# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

ESP32-S3 firmware (Arduino + FreeRTOS) that gateways a Hoymiles HMS-800W-2T solar inverter: polls it over raw TCP/Protobuf, serves a local web dashboard, and forwards data via MQTT (with an optional remote PWA over MQTT/WebSocket). Based on [dtuGateway](https://github.com/ohAnd/dtuGateway).

## Commands

```bash
# Build firmware
pio run -e esp32-s3-devkitc-1

# Flash web assets (data/www/) to LittleFS — required after any dashboard change,
# and on first flash of a new device
pio run -e esp32-s3-devkitc-1 -t uploadfs

# Flash firmware
pio run -e esp32-s3-devkitc-1 -t upload

# Serial monitor (COM14 / 115200 pre-configured in platformio.ini)
pio device monitor
```

There is no automated test suite (embedded firmware). Verify changes by building, flashing, and using the serial console commands (`status`, `dtu`, `mqtt`, `gpio`, `tasks`, `heap`, `ledtest`, `loglevel debug`) or the REST API (`/api/data.json`, `/api/info.json`) against real or simulated hardware.

**`pio run` (any target, including a plain build) auto-increments `include/buildnumber.txt`** via `version_inc.py` — expect it to always show as modified in `git status` after building; it is committed as part of normal version tracking, not accidental churn.

**`uploadfs` over USB erases `/config.json`** on an already-configured device — it writes LittleFS directly via esptool, bypassing the app's own backup logic that runs during a Filesystem-OTA. Only Filesystem-OTA (`/updatefs` endpoint or the Internet Update flow, both going through the running firmware) preserves config across a filesystem flash.

## Architecture

**DataStore pattern**: all FreeRTOS tasks read/write a single central in-memory `DataStore` (`src/dataStore.cpp`, `include/dataStore.h`) — there are no direct task-to-task dependencies or queues between tasks. When tracing a data flow (e.g. "how does a new PV reading reach MQTT"), look at how the producing task writes to DataStore and how the consuming task polls it, not at any direct call between the two task files.

**8 FreeRTOS tasks, all pinned to Core 1** (Core 0 is reserved exclusively for the ESP32-S3 WiFi/lwIP stack — never schedule application work there): `taskWiFi` (WiFi/NTP), `taskDTU` (Hoymiles TCP/Protobuf polling), `taskGPIO` (relay/IO, factory reset), `taskMQTT` (publish/subscribe, HA discovery), `taskWebServer` (HTTP/REST/OTA/captive portal), `taskLED` (NeoPixel state machine), `taskSerial` (console), `taskSysMonitor` (heap/uptime). Priorities/stack sizes are in `include/config.h`.

**DTU protocol** (`src/taskDTU.cpp`, `include/proto/`): raw TCP to port 10081, 10-byte header (magic `0x48 0x4D`, command, CRC16-MODBUS over payload, total length) + Protobuf payload. Poll sequence per cycle: `AppInfo (0xa3 0x01)` → `RealDataNew (0xa3 0x11)` → `GetConfig (0xa3 0x09)`. The DTU disconnects for ~30s every 5 minutes to sync with the Hoymiles cloud (connections get TCP RST during this window) — `taskDTU` pauses and retries after `dtuCloudPause` rather than treating this as an error.

**Two separate front-ends that share a look but not code**: `data/www/index.html` is the local dashboard, served from the device's own LittleFS and talking to the gateway's REST API directly. `app/` is an independent standalone PWA (deployed separately via GitHub Pages, see `.github/workflows/pages.yml`) that never talks to the gateway directly — it connects to the MQTT broker over WebSocket (port 9001), so it needs the cloud MQTT broker setup (`deploy/getting-started.sh`, Mosquitto+TLS) to be reachable from outside the LAN. Changes to one do not affect the other, and dashboard changes to `data/www/` require an `uploadfs` to take effect on device.

**Config persistence** (`src/appConfig.cpp`, `include/appConfig.h`): a single `AppConfig` struct serialized to `config.json` on LittleFS. Backup/restore via `/api/config/backup` and `/api/config/restore` strips WiFi/MQTT/web passwords on export; restore re-applies everything else and requires re-entering those passwords.

**OTA / release flow**: `release/manifest.json` (semver `version` + `buildNumber`, firmware/filesystem URLs + MD5) is polled by deployed gateways for update checks; `.github/workflows/release.yml` (manual trigger) builds firmware, publishes a GitHub Release, and updates that manifest in one step. Semver comparison always wins over build number; build number is only a tiebreaker for identical semver strings.

**ESP32-S3 quirk**: bootloader lives at flash address `0x0`, not `0x1000` as on classic ESP32 — relevant for any manual `esptool.py write_flash` invocation.

Full protocol/config field-level detail lives in `docs/HMS-GW-S3_Specification_v2.md`; recent findings/fixes history is in `docs/code_review.md`.
