# HMS-GW-S3 — Architecture Specification v2.0

**Project:** ESP32-S3 Gateway for Hoymiles HMS-800W-2T
**Hardware:** ESP32-S3-DevKitC-1-N8R8 (8MB Flash, 8MB PSRAM)
**Framework:** Arduino / FreeRTOS (PlatformIO)
**Repository:** https://github.com/danielguedel/HMS-GW-S3
**Date:** 2026-06-18 (Web auth, web port, static IP added)
**Status:** Implemented and in production (v0.2.0, build number is incremented automatically on every build — see `include/buildnumber.txt`)

---

## 1. Goal

A stable, maintainable gateway firmware that:
- Receives data from the Hoymiles HMS-800W-2T DTU via TCP/Protobuf
- Publishes data via MQTT to Home Assistant (HA auto-discovery)
- Provides a web dashboard
- Controls GPIO (relay, 3x IO)
- Has no dependencies between tasks

---

## 2. Core Principle: Central DataStore

All tasks communicate **exclusively** through a central in-memory DataStore. No task knows any other task directly.

```
┌──────────────┐    write    ┌─────────────────────┐    read    ┌──────────────┐
│  taskDTU     │────────────►│                     │───────────►│  taskMQTT    │
│  taskWiFi    │────────────►│      DataStore      │───────────►│  taskWeb     │
│  taskGPIO    │────────────►│   (In-Memory DB)    │───────────►│  taskSerial  │
│  taskSystem  │────────────►│   Mutex-protected   │───────────►│  taskLED     │
└──────────────┘             │   Not persistent    │            └──────────────┘
                             └─────────────────────┘
```

### 2.1 DataStore Structure

```cpp
struct DataStore {
    // ── PV data (written by taskDTU) ───────────────────────────────────────
    struct PvData {
        float   pv0_v, pv0_i, pv0_p;       // PV1: voltage, current, power
        float   pv0_dE, pv0_tE;             // PV1: daily yield, total yield [kWh]
        float   pv1_v, pv1_i, pv1_p;       // PV2: voltage, current, power
        float   pv1_dE, pv1_tE;             // PV2: daily yield, total yield [kWh]
        float   grid_v, grid_i, grid_p;     // Grid: voltage, current, power
        float   grid_dE, grid_tE;           // Grid: daily yield, total yield [kWh]
        float   temp;                        // Inverter temperature [°C]
        int32_t powerLimit;                  // Power limit [%]
        int32_t powerLimitSet;              // Limit value that was set [%]
        bool    inverterActive;             // Inverter active
        int32_t warningsActive;             // Active warnings
        int32_t wifiRssi;                   // RSSI of the DTU's WiFi
        uint32_t timestamp;                  // Unix timestamp of the last measurement
        uint32_t lastResponseMs;            // millis() of the last receipt
        bool    valid;                       // Data valid (at least 1 receipt)
    } pv;

    // ── System status (written by taskWiFi / taskDTU / taskMQTT) ──────────
    struct SystemStatus {
        // WiFi
        bool    wifiConnected;
        bool    wifiApMode;
        String  wifiIp;
        int8_t  wifiRssi;
        String  wifiSsid;
        // DTU
        bool    dtuOnline;
        uint32_t dtuLastConnectMs;
        int     dtuFailCount;
        bool    dtuCloudBusy;               // ERR_RST -14 received
        // MQTT
        bool    mqttConnected;
        uint32_t mqttLastConnectMs;
        // System
        uint32_t uptimeS;
        uint32_t freeHeap;
        String  fwVersion;
        int     buildNumber;
        String  macAddress;
        uint32_t ntpTime;                   // Unix timestamp (NTP)
    } system;

    // ── GPIO state (written by taskGPIO) ────────────────────────────────────
    struct GpioState {
        bool relay;                          // Relay state
        bool gpio[3];                        // IO1–IO3 states
    } gpio;

    // ── GPIO commands (written by taskMQTT / taskWeb / taskSerial) ────────
    struct GpioCommand {
        bool    pending;
        int     target;                      // 0=relay, 1-3=IO1-IO3
        bool    state;
    } gpioCmd;

    // ── DTU control commands ────────────────────────────────────────────────
    struct DtuCommand {
        bool    setPowerLimit;
        int     powerLimitValue;             // [%]
        bool    rebootDtu;
        bool    rebootInverter;
        bool    setInverterOn;
        bool    inverterOnValue;
    } dtuCmd;

    // ── Internet OTA status (written by taskWebServer) ─────────────────────
    struct OtaInfo {
        bool     available;          // newer version found
        bool     checking;           // manifest check currently in progress
        char     version[32];        // version from the manifest
        int      buildNumber;        // build number from the manifest
        char     url[256];           // firmware download URL
        char     fsUrl[256];         // filesystem download URL (empty = no FS update)
        char     md5[33];            // expected MD5 hash of the firmware (empty = no check)
        char     fsMd5[33];          // expected MD5 hash of the filesystem image (empty = no check)
        char     notes[128];         // release notes
        uint32_t lastCheckMs;        // millis() of the last check (0 = never)
    } otaInfo;
};

extern DataStore ds;
```

The protective mutex is **not** part of the struct — it is hidden as a `static SemaphoreHandle_t _mutex`, private to `dataStore.cpp` (since commit `8e843bf`), and unreachable from code outside that file.

### 2.2 DataStore API

```cpp
// Initialization (in main.cpp before task startup)
void dsInit();

// Read (returns a copy — no long lock needed)
DataStore::PvData       dsGetPv();
DataStore::SystemStatus dsGetSystem();
DataStore::GpioState    dsGetGpio();
DataStore::GpioCommand  dsGetGpioCommand();  // atomic read + clears the pending flag
DataStore::DtuCommand   dsGetDtuCommand();
DataStore::OtaInfo      dsGetOtaInfo();

// Write (atomic, mutex-protected)
void dsSetPv(const DataStore::PvData& data);
void dsSetSystem(const DataStore::SystemStatus& status);
void dsSetGpio(const DataStore::GpioState& state);
void dsSetGpioCommand(int target, bool state);
void dsSetDtuCommand(const DataStore::DtuCommand& cmd);
void dsClearDtuCommand();
void dsSetOtaInfo(const DataStore::OtaInfo& info);

// Convenience
bool dsIsDtuOnline();
bool dsIsWifiConnected();
bool dsPvValid();
```

**Important:** since commit `8e843bf`, the mutex is technically inaccessible from outside `dataStore.cpp` — access to `DataStore` fields is therefore only possible through the API functions above. `dsGetGpioCommand()`/`dsGetDtuCommand()` were added to replace the former direct `ds.mutex` access in `taskGPIO`/`taskDTU`.

---

## 3. Task Architecture

### 3.1 Overview

| Task | Core | Prio | Stack | Function |
|---|---|---|---|---|
| taskWiFi | 1 | 5 | 6144 | WiFi connection, AP mode, NTP |
| taskDTU | 1 | 4 | 8192 | DTU TCP connection, protocol, data |
| taskMQTT | 1 | 3 | 6144 | MQTT connection, publish, subscribe |
| taskWebServer | 1 | 3 | 8192 | HTTP API, OTA, static files |
| taskGPIO | 1 | 4 | 4096 | Relay/IO input/output |
| taskLED | 1 | 2 | 3072 | NeoPixel status indicator |
| taskSerial | 1 | 2 | 4096 | Console commands |
| taskSysMonitor | 1 | 1 | 3072 | Heap monitoring, uptime |

**Core 0:** Exclusively the WiFi stack (lwIP) — no user tasks
**Core 1:** All user tasks

### 3.2 Startup Sequence

```
main.cpp:
  1. dsInit()                    // initialize DataStore
  2. Serial.begin()
  3. LittleFS.begin()
  4. configLoad()
  5. xTaskCreatePinnedToCore(taskWiFi, ...)     // starts first
  6. xTaskCreatePinnedToCore(taskLED, ...)
  7. xTaskCreatePinnedToCore(taskGPIO, ...)
  8. xTaskCreatePinnedToCore(taskSerial, ...)
  9. xTaskCreatePinnedToCore(taskSysMonitor, ...)
  10. xTaskCreatePinnedToCore(taskDTU, ...)     // waits for EVT_WIFI_CONNECTED
  11. xTaskCreatePinnedToCore(taskMQTT, ...)    // waits for EVT_WIFI_CONNECTED
  12. xTaskCreatePinnedToCore(taskWebServer, ...)
```

### 3.3 Synchronization

A FreeRTOS EventGroup is used for system-wide events:

```cpp
// Bits (in systemState.h)
#define EVT_WIFI_CONNECTED    BIT0
#define EVT_WIFI_AP_MODE      BIT1
#define EVT_DTU_ONLINE        BIT2
#define EVT_MQTT_CONNECTED    BIT3
#define EVT_DATA_RECEIVED     BIT4
#define EVT_OTA_RUNNING       BIT5
#define EVT_FACTORY_RESET     BIT6
#define EVT_REBOOT            BIT7
```

Tasks set and read these bits. The DataStore holds the detailed state.

**Deferred reboot/factory reset:** AsyncWebServer callbacks run on the lwIP/AsyncTCP thread — a `vTaskDelay()` + `ESP.restart()` directly in the callback would block that thread. Instead, the API handlers only set `EVT_REBOOT` (or `EVT_FACTORY_RESET | EVT_REBOOT`); the actual restart (including delay and `LittleFS.remove()` on factory reset) runs centrally in `taskWebServer`'s task loop (`taskWebServer.cpp:574–580`).

---

## 4. DTU Protocol

### 4.1 Connection

- **Address:** `appConfig.dtuHost:appConfig.dtuPort` (default: port 10081)
- **Protocol:** Raw TCP (no HTTP)
- **Library:** AsyncTCP
- **RX timeout:** 60s (`setRxTimeout(60)`) — longer than the poll interval, so the connection stays open between cycles

### 4.2 Packet Format

```
Byte 0-1:   0x48 0x4D          (header magic)
Byte 2-3:   [cmd0] [cmd1]      (command)
Byte 4-5:   0x00 0x01          (constant)
Byte 6-7:   [CRC16-MODBUS-hi] [CRC16-MODBUS-lo]  (over the payload)
Byte 8-9:   [total-len-hi] [total-len-lo]          (10 + payload length)
Byte 10+:   [Protobuf payload]
```

**CRC16:** MODBUS variant (Initial=0xFFFF, Poly=0x8005, RefIn=true, RefOut=true)

### 4.3 Commands

| Command | Bytes | Direction | Description |
|---|---|---|---|
| AppInformation | 0xa3 0x01 | → DTU | Handshake (immediately after connect) |
| RealDataNew | 0xa3 0x11 | → DTU | Request real-time PV data |
| GetConfig | 0xa3 0x09 | → DTU | Configuration (power limit) |
| SetPowerLimit | 0xa3 0x05 | → DTU | Set power limit (CommandResDTO payload, see §4a) |

### 4.4 Per-Cycle Flow

```
On connect (once):
  1. TCP connect to DTU
  2. Immediately in the onConnect callback: send AppInformation (0xa3 0x01)
  3. Wait for the AppInfo response (max. 8s) — DIRECTLY after connect,
     BEFORE the poll interval, so the response isn't lost to the
     RX timeout
  4. TCP connection stays open (RxTimeout=60s > poll interval)

Per poll interval (dtuInterval seconds):
  5. Send RealDataNew (0xa3 0x11)
  6. Wait for response (max. 5s)
  7. Parse data → dsSetPv()  →  setLedState(LED_DATA_FLASH)
  8. Send GetConfig (0xa3 0x09)
  9. Wait for response (max. 3s, non-fatal on timeout)
  10. Power limit + DTU RSSI → dsSetPv()
  11. dsSetPv() + setDtuOnline(true) → MQTT publishes automatically
  12. Continue at step 5

Important — onData flag ordering:
  _appReady / _dataReady / _cfgReady accumulate (once true, stays true).
  Only sendRealDataNew() sets _dataReady=false, sendGetConfig() sets _cfgReady=false.
  waitFor() must NOT reset flags, or the detection chain breaks.

On ERR_RST (-14): cloud-sync pause, then reconnect (from step 1)
```

### 4.5 Cloud-Sync Pause

The DTU communicates with the Hoymiles cloud every 5 minutes:
- Time window: second >= 50 when minute % 5 == 4
- Duration: ~30 seconds
- Symptom: TCP error -14 (ERR_RST)
- Behavior: wait `dtuCloudPause` seconds, then retry


---

## 4a. Power Limit — Timeout Mechanism

The inverter's power limit is set via MQTT or the web GUI. For safety, the value automatically falls back to the default after a configurable timeout.

### Flow

```
MQTT/Web: setPowerLimit(80%)
  → taskDTU sets the limit to 80%
  → timer starts (powerLimitTimeout seconds)

If no further setPowerLimit before expiry:
  → timer expires
  → taskDTU resets the limit to powerLimitDefault (100%)
  → log: [WRN] [DTU] Power limit timeout — reset to 100%
  → MQTT publish: {topic}/inverter/PowerLimit = 100
```

### Configuration

| Parameter | Default | Description |
|---|---|---|
| `powerLimitDefault` | 100 | Fallback value [%] |
| `powerLimitTimeout` | 0 | Timeout [s], 0 = disabled |

### Wire Protocol (verified 2026-06-20 against a live capture)

`SetPowerLimit` is **not** a dedicated message with a plain integer field — it reuses the generic `CommandResDTO` command envelope (see `include/proto/CommandPB.proto`), sent under command bytes `0xa3 0x05` (not `0x0e`, which was the original, incorrect assumption carried over from the v2 rewrite and always resulted in a silently-ignored packet):

| Field | # | Value |
|---|---|---|
| `time` | 1 | current NTP epoch time |
| `action` | 2 | `8` (`CMD_ACTION_LIMIT_POWER`, per ohAnd/dtuGateway's `dtuConst.h`) |
| `package_nub` | 4 | `1` |
| `tid` | 6 | current NTP epoch time |
| `data` | 7 | string `"A:<value>,B:0,C:0\r"`, where `value = percent * 10`, clamped to 20–1000 (i.e. 2.0%–100.0%) |

The DTU acknowledges with a status reply (`action` echoed back, `err_code` omitted on the wire = `0` = success). The actual confirmed value is read back via the next `GetConfig` poll, field **5** (`limit_power_mypower`, value = percent × 10) — **not field 3**, which is essentially always absent/0 on the wire (it maps to `lock_password` in the request-side schema) and was the original, silently-wrong read-back mapping. A `0` reading on field 5 is treated as transient/spurious and ignored (matches ohAnd/dtuGateway's `readRespGetConfig()` guard), keeping the last known good value instead of flickering to 0%.

### Implementation in taskDTU

```cpp
// In the task loop:
if (_powerLimitPending) {
    dtuSetPowerLimit(_pendingLimit);
    _powerLimitSetAt = millis();
    _powerLimitPending = false;
}

// Timeout check:
if (appConfig.powerLimitTimeout > 0 && _powerLimitSetAt > 0) {
    if ((millis() - _powerLimitSetAt) > (uint32_t)appConfig.powerLimitTimeout * 1000) {
        if (ds.pv.powerLimit != appConfig.powerLimitDefault) {
            LOG_W(MOD_DTU, "Power limit timeout — reset to %d%%", appConfig.powerLimitDefault);
            dtuSetPowerLimit(appConfig.powerLimitDefault);
        }
        _powerLimitSetAt = 0;  // reset the timer
    }
}
```

---

## 5. MQTT Implementation

### 5.1 Problem with PubSubClient

PubSubClient is synchronous and blocking. `connect()` blocks the thread for >1 second and trips the WiFi task watchdog on Core 0. For the reimplementation, **esp-mqtt** (native ESP-IDF, non-blocking) or a custom solution is therefore used.

### 5.2 Proposed Solution: esp_mqtt

```cpp
// Non-blocking MQTT client (native ESP-IDF, flat 4.x API)
// Note: framework-arduinoespressif32 @ 3.x bundles ESP-IDF 4.x →
//       flat struct fields, NOT the nested 5.x API (broker.address.uri)
#include "mqtt_client.h"

esp_mqtt_client_config_t cfg = {};
cfg.uri         = "mqtt://10.1.1.41:1883";
cfg.client_id   = "hmsgws3_406194";        // last 3 bytes of the WiFi MAC, see esp_read_mac()
cfg.keepalive   = 60;
cfg.lwt_topic   = "hmsgws3_406194/system/status";  // "<mqttTopic>/system/status"
cfg.lwt_msg     = "offline";
cfg.lwt_msg_len = 7;
cfg.lwt_retain  = 1;
esp_mqtt_client_handle_t client = esp_mqtt_client_init(&cfg);
esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqttEventHandler, NULL);
esp_mqtt_client_start(client);
```

All callbacks run asynchronously — no blocking, no watchdog.

### 5.3 Topics

```
{mqttTopic}/grid/U                 Grid voltage [V]
{mqttTopic}/grid/I                 Grid current [A]
{mqttTopic}/grid/P                 Grid power [W]
{mqttTopic}/grid/dailyEnergy       Daily yield [kWh]
{mqttTopic}/grid/totalEnergy       Total yield [kWh]
{mqttTopic}/pv0/U, /pv0/I, /pv0/P PV1
{mqttTopic}/pv1/U, /pv1/I, /pv1/P PV2
{mqttTopic}/inverter/Temp          Temperature [°C]
{mqttTopic}/inverter/PowerLimit    Power limit [%] (confirmed by the DTU)
{mqttTopic}/inverter/PowerLimitTarget  Target power limit [%] — last requested value, may briefly differ from PowerLimit until the DTU confirms it
{mqttTopic}/relay/state            Relay state (0/1)
{mqttTopic}/io{1-3}/state          GPIO state (0/1)
{mqttTopic}/system/uptime          Uptime [s]
{mqttTopic}/system/rssi            WiFi RSSI [dBm]
{mqttTopic}/system/heap            Free heap [bytes]
{mqttTopic}/system/status          online/offline (retained, also used as the LWT topic)

# Control topics (subscribe)
{mqttTopic}/relay/set              Set relay (0/1)
{mqttTopic}/io{1-3}/set           Set GPIO (0/1)
{mqttTopic}/inverter/PowerLimitSet/set  Set power limit [%]
{mqttTopic}/inverter/On/set        Inverter on/off (0/1)
{mqttTopic}/inverter/RebootDtu/set Reboot DTU (1)
{mqttTopic}/inverter/RebootGw/set  Reboot gateway (1)
```

### 5.4 HA Auto-Discovery

Discovery messages are sent 5 seconds after the MQTT connect, one every 500ms (prevents a watchdog reset from a burst).

---

## 6. Web API

### 6.1 Endpoints

| Method | Path | Description |
|---|---|---|
| GET | `/api/data.json` | Real-time PV data |
| GET | `/api/info.json` | System info, connection status |
| GET | `/api/config` | Current configuration (without passwords) |
| POST | `/api/config` | Save configuration |
| GET | `/api/config/backup` | Full `config.json` as a download (incl. passwords in plaintext) |
| POST | `/api/config/restore` | Upload a `config.json` backup, validate, apply + restart |
| GET | `/api/gpio` | GPIO state |
| POST | `/api/gpio` | Set GPIO |
| GET | `/api/dtu` | DTU status (power limit, power limit target, inverter active) |
| POST | `/api/dtu` | DTU control commands (power limit, reboot, inverter on/off) |
| POST | `/update` | OTA firmware update (file upload) |
| POST | `/updatefs` | OTA filesystem update (file upload) |
| GET | `/api/ota/check` | Query the last Internet OTA check status |
| POST | `/api/ota/check` | Manually trigger an Internet OTA version check |
| POST | `/api/ota/url` | Internet OTA: download and flash firmware/filesystem from a URL |

### 6.2 data.json

```json
{
  "pv0": { "v": 26.6, "i": 1.33, "p": 353.0, "dE": 1.591, "tE": 129.164 },
  "pv1": { "v": 27.7, "i": 1.07, "p": 296.0, "dE": 1.450, "tE": 112.300 },
  "grid": { "v": 241.9, "i": 2.53, "p": 610.0, "dE": 3.041, "tE": 241.464 },
  "inverter": { "temp": 45.2, "powerLimit": 100, "powerLimitSet": 100, "active": true },
  "timestamp": 1781430913,
  "valid": true
}
```

### 6.3 info.json

```json
{
  "fw": "0.2.0",
  "build": 132,
  "uptime": 3600,
  "heap": 278000,
  "mac": "946140D4DB1C",
  "ip": "10.1.1.145",
  "rssi": -42,
  "wifi": true,
  "dtu": true,
  "mqtt": false,
  "ntpTime": 1781430913
}
```

### 6.4 Access Protection and Port (implemented 2026-06-18)

- **Username/password protection:** Optional HTTP Basic Auth protection for the entire web GUI, configurable in the Config tab (`webAuthEnabled`, `webUser`, `webPass` in `AppConfig`, §8). Default: disabled. Implemented via the `AsyncAuthenticationMiddleware` built into ESPAsyncWebServer 3.x, attached globally via `server->addMiddleware(&authMiddleware)` (`taskWebServer.cpp`, `setupRoutes()`) — this automatically covers **all** routes (`/api/*`, `/update`, `/updatefs`, static files, captive portal), not just individually maintained handlers. Safety guard: saving `webAuthEnabled=true` without a password is rejected (lockout protection), both when loading the config and on `POST /api/config`.
- **Configurable port:** `appConfig.webPort` (default: 80). For this, `server` was changed from a global static `AsyncWebServer` object to a pointer, allocated only in `setupRoutes()` (after `configLoad()`) via `new AsyncWebServer(appConfig.webPort)`.
- Both settings only take effect after the automatic restart (like all config changes). After a port change, the web GUI must subsequently be accessed under the new port number; with auth protection active, the browser will automatically prompt for credentials on the next access (native Basic Auth popup).

### 6.5 Configuration Backup/Restore (implemented 2026-06-18)

- **Backup:** `GET /api/config/backup` returns the complete, raw `config.json` as a file download (`Content-Disposition: attachment`) — **including** WiFi/MQTT/web-auth passwords in plaintext, a deliberate decision so a restore really restores everything without having to re-enter passwords. Reads directly from LittleFS (`req->send(LittleFS, CONFIG_FILE, "application/json", true)`), no detour through `appConfig`.
- **Restore:** `POST /api/config/restore` (file upload, like `/update`/`/updatefs`) validates the uploaded file (must parse as JSON AND contain at least `wifiSsid` or `dtuHost`, to reject random unrelated files), applies it via `applyConfigJson()` — the same validation/clamping logic as `configLoad()` — and saves it via `configSave()`. On invalid content: `400`, the config remains unchanged.
- **Important implementation pitfall:** ESPAsyncWebServer's simple string routes are "backward compatible" — a route `/api/config` also matches `/api/config/backup` and `/api/config/restore` by prefix (`path.startsWith(_value + "/")`). The more specific routes **must be registered before** `/api/config` (`setupRoutes()`), otherwise the broader `/api/config` handler intercepts the request — which is exactly what happened during implementation (the backup silently returned the password-free `GET /api/config` response instead of the actual file) and was only discovered by comparing the in-memory value with the response actually delivered.

---

## 7. LED States (GPIO38, WS2812B)

| State | Color | Pattern | Meaning |
|---|---|---|---|
| LED_BOOT | White | 3× blink (120ms) | Boot in progress |
| LED_WIFI_CONNECTING | Blue | 1 Hz blink | WiFi connection in progress |
| LED_AP_MODE | Blue | 3× short + pause | AP mode active (needs user action) |
| LED_DTU_OFFLINE | Orange | Double blink + pause | WiFi OK, DTU offline |
| LED_NO_MQTT | Cyan | Slow pulse 4s | WiFi+DTU OK, MQTT offline |
| LED_OPERATIONAL | Green | 5s heartbeat | Full operation, PV active (≥ 1W) |
| LED_STANDBY | Green (10%) | Very slow pulse 10s | No PV output (night/overcast) |
| LED_DATA_FLASH | Orange | 1× 80ms | New DTU data received (transient) |
| LED_OTA | Magenta | Fast 5 Hz | OTA update in progress |
| LED_ERROR | Red | 4 Hz | Critical error |
| LED_FACTORY_RESET | Red | Steady | Factory reset in progress |

The state is **auto-derived** (`deriveState()`) — no manual `setLedState()` needed except for transients (`LED_DATA_FLASH`) and OTA.

---

## 8. Configuration (appConfig)

```cpp
struct AppConfig {
    // WiFi
    char wifiSsid[33];
    char wifiPass[65];
    bool wifiApFallback;        // AP mode when WiFi is unavailable

    // WiFi — Static IP (useStaticIp=false -> DHCP, default)
    bool useStaticIp;
    char staticIp[16];          // e.g. "192.168.1.50"
    char subnet[16];            // default: "255.255.255.0"
    char gateway[16];           // e.g. "192.168.1.1" — also used as the DNS server

    // DTU
    char dtuHost[40];
    uint16_t dtuPort;           // default: 10081
    int  dtuInterval;           // poll interval [s], default: 31
    int  dtuCloudPause;         // wait time during cloud sync [s], default: 30
    int  dtuRebootAfterFails;   // reconnect after N failures, default: 3

    // Power Limit
    int  powerLimitDefault;     // default limit [%], default: 100
    int  powerLimitTimeout;     // timeout [s] after which it falls back to the default
                                // 0 = no timeout (value persists until manually changed)
                                // default: 0

    // MQTT
    char mqttHost[40];
    uint16_t mqttPort;          // default: 1883
    char mqttUser[33];
    char mqttPass[65];
    char mqttTopic[33];         // default: "hmsgws3_XXXXXX" (last 3 bytes of the WiFi MAC, e.g. "hmsgws3_406194")
    bool mqttRetain;
    bool mqttHaDiscovery;       // enable HA auto-discovery
    bool mqttOpenDtu;           // OpenDTU-compatible topics

    // GPIO — default pin assignment (adjustable in the web GUI)
    struct {
        uint8_t pin;            // default: GPIO1
        bool inverted;
    } relay;
    struct {
        uint8_t pin;             // default: GPIO2, GPIO3, GPIO4
        enum { IO_OUTPUT, IO_INPUT, IO_RESERVED } mode;
        char    altFunction[16]; // purely informational, e.g. "I2C_SDA" — does not change behavior
        bool inverted;
        bool pullup;
    } io[3];

    // LED
    uint8_t ledPin;             // default: GPIO38 (onboard WS2812B)
    uint8_t ledBrightness;      // 0-255, default: 80

    // System
    int  tzOffset;              // timezone offset [s], default: 3600 (UTC+1)
    char ntpServer[65];         // default: "pool.ntp.org"
    int  logLevel;              // 0=ERROR, 1=WARN, 2=INFO, 3=DEBUG

    // Internet OTA
    char otaManifestUrl[256];   // URL to the version manifest (empty = disabled)

    // Web server (see §6.4)
    bool     webAuthEnabled;    // enable username/password protection, default: false
    char     webUser[33];       // username, default: "admin"
    char     webPass[65];       // password
    uint16_t webPort;           // web GUI port, default: 80
};
```

### 8.0 Static IP (implemented 2026-06-18)

`useStaticIp`/`staticIp`/`subnet`/`gateway` in the Config tab (WiFi block). Default: DHCP (`useStaticIp=false`). When enabled, `taskWiFi.cpp` calls `WiFi.config(ip, gateway, subnet, gateway)` before `WiFi.begin()` — the gateway address is also used as the DNS server (there is no separate DNS field; this covers the common case since most routers act as a DNS proxy themselves, which is needed for NTP resolution). Validation (`IPAddress::fromString()`) happens both in `appConfig.cpp` (`configLoad()`) and in `taskWebServer.cpp` (`POST /api/config`) — on invalid values, `useStaticIp` stays disabled (falls back to DHCP), or the request is rejected with a 400.

### 8.1 GPIO Default Pin Assignment

| Function | Pin | Mode (Default) | Alt Function (Label) | Configurable |
|---|---|---|---|---|
| Relay | GPIO1 | OUTPUT | — | ✅ |
| *(internal)* | GPIO0 | BOOT / factory reset | — | — |
| IO1 | GPIO2 | OUTPUT | "I2C_SDA" | ✅ |
| IO2 | GPIO3 | OUTPUT | "I2C_SCL" | ✅ |
| IO3 | GPIO4 | OUTPUT | "ADC1_CH3" | ✅ |
| LED (WS2812B) | GPIO38 | — | — | ✅ |

GPIO0 is exclusively reserved for the BOOT button/factory reset (see `BOOT_PIN` in `config.h`) and is not part of the IO array — no API/MQTT access.

`altFunction` is a free-text field (max. 16 characters), purely informational. It indicates what the pin would additionally be suited for according to the ESP32-S3 datasheet, but does not change the firmware logic — IO1–IO3 all start in `OUTPUT` mode and are freely reconfigurable.

GPIO2 (SDA) and GPIO3 (SCL) are reserved for future I2C sensors (temperature, humidity, etc.). In `I2C_RESERVED` mode, the pins are initialized as standard GPIO but marked as "reserved" in the web GUI. The actual I2C implementation will follow in a later version.

All pins are adjustable via the web GUI and stored in `config.json`.

Stored as JSON in LittleFS (`/config.json`).

---

## 9. Web Dashboard Design

### 9.1 Design Principles

**"Neon Flow"** dark-mode design — glowing neon-tube accents on a near-black background, no light theme (the glow aesthetic only works on dark; the previous dark/light toggle was removed along with the old Shelly-style theme).

- **Dark only**, no theme toggle — `:root` defines the palette directly, no `[data-theme]` variants
- **Bilingual** (English default, German toggleable via a `DE`/`EN` icon button in the header, preference in `localStorage`) — `I18N` object (`en`/`de`) + `data-i18n`/`data-i18n-ph` attributes + `tr(key, ...args)` lookup
- **Color palette** (`:root` custom properties), each color carries one consistent meaning, not used decoratively:

  | Color | Var | Meaning |
  |---|---|---|
  | Cyan `#2bf0ff` | `--cyan` | Data / connected / confirmed / default action buttons |
  | Pink `#ff2bd6` | `--pink` | Attention / danger / hard error / title-glow accent |
  | Violet `#b04bff` | `--purple` | Parameter / configuration (box titles, card titles, input text) |
  | Lime `#c8ff2b` | `--lime` | Status "Active" (the one true "all good" state) |
  | Orange `#ff9d2b` | `--orange` | Status "Offline"/warning, pending power-limit, non-destructive danger-zone actions (Reboot) |
  | Gray `var(--muted)` | `--muted` | Default body text, muted/standby/off states |

- Typography: system font stack (`-apple-system, BlinkMacSystemFont, "Segoe UI"`)
- Card/box/section titles consistently in violet, uppercase (`.card-title`, `.sec`, `.gpio-name`) — **except** Config/System box headers, which use the same violet but at a larger size (`.cfg-box h3`); Grid/PV1/PV2 card titles are gray like other dashboard titles, not violet
- Card layout for dashboard elements. Dashboard `.card` elements have a **permanent** glowing border (`::before` gradient-mask trick, colored via a `--glow` CSS custom property per card) since they show the "hero" live data; `.cfg-box`/`.gpio-card` elements only glow on `:hover` (a permanent multi-color glow across 4–8 simultaneously visible boxes per tab would look busy)
- Status indicators (the big dashboard status text and the small Relay/IO ON/OFF badges) render as **plain glowing text, no pill/badge background** — pill shapes are reserved for actual clickable buttons, so a status indicator is never visually confused with something tappable
- Responsive: mobile-first, works on a smartphone without zooming
- No external dependencies — everything in `index.html` (inline CSS + JS + inline SVG logo)
- Auto-refresh: PV data every 10s (`refreshData()`), connection status/header info every 15s (`refreshInfo()`), time/date in the header every second (`tickClock()`)

**Header:** title on the left — an inline SVG logo (a neon-tube lightning bolt in pink + two signal arcs in cyan, both with a white-hot core stroke under the colored glow stroke) followed by "HMS-GW-S3", the whole title wrapped in a link to the project's GitHub repo. On the right, stacked in two rows: the WiFi/DTU/MQTT status dots plus the `DE`/`EN` language button on top, the info bar (MAC | firmware+build | date | time, separated by `.hdr-sep` dashes) below. The date/time format is fixed as `DD.MM.YYYY` / `HH:MM:SS` (`fmtDate()`/`fmtTime()`), independent of the UI language — identical to the format in the System tab (`loadSysInfo()`).

A standalone, purely visual exploration of this design (mock data, never wired to the device) lives at `design/neon-dashboard.html` — useful as a reference when iterating on styling without touching the production file or needing a live gateway.

### 9.2 Dashboard Tab — PV Output Display

```
┌──────────────────────────────────────────────────────────────┐
│  ⚡≈ HMS-GW-S3                    ● WiFi ● DTU ● MQTT  [DE]  │  ← Header, row 1
│                                  MAC | FW+Build | Date | Time │  ← Header, row 2
├────────────────────────────────────────────────────────────────┤
│  Status               │  Power Limit                          │
│  ACTIVE (glow text)   │  [════════●══] 100%  [ Set ]          │  ← Status card +
├──────┬──────┬─────────┼──────────────────────────────────────┤     Power-limit card
│ Temp │Limit │Yield tdy│ Total                                  │  ← Temp/Limit/Yield/
│45.2°C│ 100% │ 3.04 kWh│ 241.4 kWh                              │     Total cards
├──────┴──────┴─────────┴──────────────────────────────────────┤
│  ┌──────────┐  ┌──────────┐  ┌──────────┐                    │
│  │  Grid    │  │  PV1     │  │  PV2     │                    │  ← Power cards: power [W]
│  │ 610 W    │  │ 353 W    │  │ 296 W    │                    │     + bar display (% of
│  │ ▓▓▓▓░░░  │  │ ▓▓▓░░░░  │  │ ▓▓░░░░░  │                    │     a max value) + sub-
│  │241.9V/2.5A│  │ 26.6V/1.3A│ │ 27.7V/1.1A│                   │     line voltage/current
│  └──────────┘  └──────────┘  └──────────┘                    │
│   cyan glow      pink glow     violet glow                    │
└──────────────────────────────────────────────────────────────┘
```

The cards are updated every 10s via `GET /api/data.json` (`pv0`/`pv1`/`grid` → power, bar width, voltage/current subline; `inverter` → temp/limit cards). The daily-yield/total-yield cards come from `pv0.dE+pv1.dE+grid.dE` and the `tE` fields respectively. All four cards (Temp/Limit/Yield/Total) are always visible and show `–` as a placeholder as long as no valid data is available — they are NO LONGER hidden (unlike in an earlier version).

**Status display (centralized via `updateStatusBadge()`):** The status card combines two independent data sources into one of four states:
- `dtuOnline` from `GET /api/info.json` (`dtu` field) — DTU connection
- `inverter.active` from `GET /api/data.json` — feed-in in progress

| State | Condition | Color |
|---|---|---|
| **Offline** | `dtuOnline === false` | Orange (`var(--orange)`) |
| **Active** | DTU online + `inverter.active === true` | Lime (`var(--lime)`) |
| **Standby** | DTU online + `inverter.active === false` | Gray (`var(--muted)`) |
| **No data** (transition) | DTU online, but no PV data received yet | Gray (`var(--muted)`) |

The status renders as plain glowing text (`.badge-lg` with a `.b-ok`/`.b-warn`/`.b-off` color class) — no pill background, no dot indicator; pill shapes are reserved for buttons. The same `cls`/`bcls` state additionally drives:
- Header separator dashes (`.hdr-sep` — empty/`warn`/`muted`)
- Grid/PV1/PV2 values and progress bars (`.card-val`/`.bar` — same empty/`warn`/`muted` classes override the card's own glow color to orange/gray when the DTU is offline or there's no live data; when data is live and the DTU is online, each card keeps its own glow: Grid cyan, PV1 pink, PV2 violet via a `--glow` custom property per `.card`)
- Temp/Limit/daily-yield/Total values (`s-temp`/`s-limit`/`s-de`/`s-te`)

**Browser tab title:** On every refresh with valid data, `document.title` is set to the current grid power, e.g. `"610 W — HMS-GW-S3"` — allows reading the current output even with a minimized/inactive browser tab (e.g. when overviewing multiple tabs).

### 9.3 Pages (Tabs)

Four tabs in the main navigation (`nav button`, `showTab()`):

- **Dashboard** — status card, power-limit card, Temp/Limit/Yield/Total cards, PV output (Grid/PV1/PV2 cards) (section 9.2)
- **Relay / IO** — its own tab (not part of the dashboard) with toggle switches for relay, IO1–IO3, including status badge (called "GPIO / Relay" until 2026-06-18)
- **Config** — WiFi (incl. static IP/subnet/gateway), DTU, MQTT, GPIO pin assignment, web server (port, access protection), System (organized within the tab)
- **System** — firmware OTA (file upload), filesystem OTA (file upload), Internet update (manifest check + install), config backup/restore (download/upload `config.json`), device information, Danger Zone (reboot, factory reset)

---

## 10. OTA Update

Two methods:

### 10.1 File Upload (Local)

In the web GUI under System → OTA:
- Firmware: upload a `.bin` file → `POST /update` → ESPAsyncWebServer + Update library
- Filesystem: upload a `.bin` file → `POST /updatefs`
- Progress display in the browser
- Automatic restart after a successful update

### 10.2 Internet Update (URL)

- `POST /api/ota/url` with a JSON body `{"url": "...", "fsUrl": "..."}` (both optional, at least one must be set) — the web GUI auto-fills the URL and FS URL from the manifest
- The request is buffered in `_otaUrl`/`_otaFsUrl` and handed to `taskWebServer`'s task loop via a pending flag (no download in the AsyncTCP callback context)
- Download via `WiFiClientSecure` (`setInsecure()`, no certificate pinning) + `HTTPClient` with `HTTPC_STRICT_FOLLOW_REDIRECTS` (needed for GitHub Releases CDN redirects)
- The stream is read in 512-byte chunks and written via `Update.write()`; progress is logged every 10% as `LOG_I`
- Firmware and filesystem updates run sequentially — both flags are set before each `LOG_I` to avoid a race condition with the reboot (fix in commit `07ffd0d`)
- No WebSocket/live progress in the browser — status is polled via `GET /api/ota/check`
- MD5 hash check is optional (see below) — no cryptographic signature verification
- **Config preservation on filesystem update:** A filesystem OTA (`U_SPIFFS`) overwrites the entire LittleFS partition with the CI build image (only `data/www/*`, no `/config.json`). Since the fix on 2026-06-17 (production incident, see `docs/code_review.md` §0), `/config.json` is backed up to RAM before writing and written back afterward (`backupConfigBeforeFsOta()`/`restoreConfigAfterFsOta()` in `taskWebServer.cpp`) — both for local upload (`/updatefs`) and Internet URL OTA.
- **MD5 verification:** `POST /api/ota/url` optionally accepts `md5`/`fsMd5` (32-character hex). If provided, `doUrlOtaPartition()` calls `Update.setMD5()` before writing — `Update.end()` then checks the hash and fails with a clear error message, instead of a corrupted download merely satisfying the byte count, appearing to succeed, but then being silently discarded by the bootloader on the next boot for lacking a valid checksum (exactly this case occurred in production on 2026-06-17: build 208 was "successfully" written, but the device silently rebooted back to the old build). The hashes come from the manifest (`md5`/`fs_md5`, computed by the release workflow via `md5sum`) and are passed through to the web GUI via `GET /api/ota/check` (`md5`/`fsMd5`).

### 10.3 Internet OTA Version Check (Manifest)

- `appConfig.otaManifestUrl` points to a JSON manifest (default: `https://raw.githubusercontent.com/danielguedel/HMS-GW-S3/main/release/manifest.json`)
- Automatic check after a WiFi connect; manual trigger via `POST /api/ota/check`
- The result lands in `DataStore::OtaInfo` (`available`, `version`, `buildNumber`, `url`, `fsUrl`, `md5`, `fsMd5`, `notes`, `lastCheckMs`) and is served via `GET /api/ota/check`
- The web GUI's "Internet Update" tile shows the result and offers "Install now" (triggers `POST /api/ota/url` with the manifest URLs)
- GitHub Actions release workflow (`workflow_dispatch` with `version` + `notes`) creates a release + updates `manifest.json`

---

## 11. Console (Serial + Web Terminal)

### 11.1 Output Format

Linux-like formatted output:

```
[00:01:23.456] [INF] [DTU   ] TCP connected to 10.1.1.143:10081
[00:01:23.789] [INF] [DATA  ] PV1: 26.6V/1.33A/353W  PV2: 27.7V/1.07A/296W
[00:01:23.790] [INF] [DATA  ] Grid: 241.9V/2.53A/610W  Temp: 45.2°C
[00:01:23.791] [DBG] [DTU   ] RX 245 bytes — CRC OK
[00:01:24.001] [WRN] [MQTT  ] Reconnecting... (attempt 3/5)
[00:01:24.500] [ERR] [WIFI  ] Connection lost — RSSI: -85 dBm
```

Format: `[HH:MM:SS.mmm] [LVL] [MODULE] message`

| Level | Abbreviation | Color (ANSI) |
|---|---|---|
| ERROR | ERR | Red (bold) `\e[1;31m` |
| WARNING | WRN | Yellow `\e[33m` |
| INFO | INF | Green `\e[32m` |
| DEBUG | DBG | Cyan `\e[36m` |

### 11.2 Log Level

Configurable via the web GUI and console command:

```bash
loglevel debug    # all messages
loglevel info     # default
loglevel warn     # warnings and errors only
loglevel error    # errors only
```

### 11.3 Console Commands

```bash
help              # command overview
status            # system status (WiFi, DTU, MQTT, heap)
config            # show current configuration
restart           # restart the gateway
reset             # factory reset
wifi              # WiFi status
dtu               # DTU status and last data
mqtt              # MQTT status
gpio              # GPIO state
relay on|off      # switch relay
io1 on|off        # switch IO1
loglevel <lvl>    # set log level
version           # firmware version
uptime            # uptime
heap              # heap usage
tasks             # FreeRTOS task list
ledtest           # cycle through all LED states (diagnostics)
```

### 11.4 Web Terminal (Optional, Phase 2)

A terminal window in the web dashboard that mirrors serial output via WebSocket — so the log can be read without a USB connection too.

---

## 12. File Structure

```
HMS-GW-S3/
├── platformio.ini
├── custom_partitions.csv
├── version_inc.py
├── data/
│   └── www/
│       └── index.html          (dashboard SPA — "Neon Flow" dark-glow design)
├── include/
│   ├── config.h                (build constants, stack sizes, pin defaults)
│   ├── appConfig.h             (AppConfig struct)
│   ├── dataStore.h             (DataStore struct + API)
│   ├── systemState.h           (EventGroup bits)
│   ├── logger.h                (LOG_I/W/E/D macros with ANSI colors)
│   └── taskLED.h               (setLedState() declaration)
└── src/
    ├── main.cpp                (setup, task startup, dsInit)
    ├── appConfig.cpp           (load/save config)
    ├── dataStore.cpp           (DataStore implementation)
    ├── logger.cpp              (formatted output HH:MM:SS.mmm)
    ├── taskWiFi.cpp            (WiFi + NTP as its own task)
    ├── taskDTU.cpp             (TCP + manual Protobuf, no Nanopb)
    ├── taskMQTT.cpp            (esp-mqtt non-blocking, ESP-IDF 4.x flat API)
    ├── taskWebServer.cpp       (HTTP API, OTA file+URL, static files)
    ├── taskGPIO.cpp            (relay GPIO1, IO1-3)
    ├── taskNeoPixel.cpp        (WS2812B GPIO38, state auto-derivation)
    ├── taskSerial.cpp          (console with commands)
    └── taskSysMonitor.cpp      (heap, uptime)
```

---

## 13. Implementation Order

1. **DataStore** (`dataStore.h` / `dataStore.cpp`) — foundation
2. **taskWiFi** — WiFi + NTP as its own task, sets EVT_WIFI_CONNECTED
3. **taskDTU** — refactored, reads from appConfig, writes to DataStore
4. **taskWebServer** — reads from DataStore for API responses
5. **taskGPIO** — reads/writes GpioState in the DataStore
6. **taskLED** — reads SystemStatus from the DataStore for the LED state
7. **taskMQTT** — non-blocking esp-mqtt, reads the DataStore, publishes
8. **taskSerial** / **taskSysMonitor** — read the DataStore for status
9. **Web dashboard** — adapted to the new API

---

## 14. Known Issues in the Old Implementation (v1)

| Problem | Cause | Solution in v2 |
|---|---|---|
| Tasks know each other directly | Global variables (`latestDtuData`, `gpioState`) | DataStore with API |
| MQTT watchdog reset | PubSubClient blocks Core 0 | esp-mqtt (non-blocking) |
| WiFi in taskWebServer | WiFi init too early, wrong task | Dedicated taskWiFi |
| NTP in taskDTU | NTP doesn't belong in the DTU task | taskWiFi takes over NTP |
| HA discovery watchdog | 16 publishes in one block | Staggered, 1 per 500ms |
| Stack overflows | Stacks too small | Generous values in config.h |
| LittleFS opened before mount | Ordering in taskWebServer | LittleFS.begin() in main.cpp |
