# HMS-GW-S3 — Architektur-Spezifikation v2.0

**Projekt:** ESP32-S3 Gateway für Hoymiles HMS-800W-2T  
**Hardware:** ESP32-S3-DevKitC-1-N8R8 (8MB Flash, 8MB PSRAM)  
**Framework:** Arduino / FreeRTOS (PlatformIO)  
**Repository:** https://github.com/danielguedel/HMS-GW-S3  
**Datum:** 2026-06-14  
**Status:** Implementiert und produktiv (v0.2.0, Build 150)

---

## 1. Ziel

Stabile, wartbare Gateway-Firmware die:
- Daten vom Hoymiles HMS-800W-2T DTU über TCP/Protobuf empfängt
- Daten per MQTT an Home Assistant publiziert (HA Auto-Discovery)
- Ein Web-Dashboard bereitstellt
- GPIO (Relay, 4x GP) steuert
- Ohne Abhängigkeiten zwischen Tasks auskommt

---

## 2. Kernprinzip: Zentraler DataStore

Alle Tasks kommunizieren **ausschliesslich** über einen zentralen In-Memory-DataStore. Kein Task kennt einen anderen Task direkt.

```
┌──────────────┐    write    ┌─────────────────────┐    read    ┌──────────────┐
│  taskDTU     │────────────►│                     │───────────►│  taskMQTT    │
│  taskWiFi    │────────────►│      DataStore      │───────────►│  taskWeb     │
│  taskGPIO    │────────────►│   (In-Memory DB)    │───────────►│  taskSerial  │
│  taskSystem  │────────────►│   Mutex-geschützt   │───────────►│  taskLED     │
└──────────────┘             │   Nicht persistent  │            └──────────────┘
                             └─────────────────────┘
```

### 2.1 DataStore-Struktur

```cpp
struct DataStore {
    // ── PV-Daten (geschrieben von taskDTU) ────────────────────────────────
    struct PvData {
        float   pv0_v, pv0_i, pv0_p;       // PV1: Spannung, Strom, Leistung
        float   pv0_dE, pv0_tE;             // PV1: Tagesertrag, Gesamtertrag [kWh]
        float   pv1_v, pv1_i, pv1_p;       // PV2: Spannung, Strom, Leistung
        float   pv1_dE, pv1_tE;             // PV2: Tagesertrag, Gesamtertrag [kWh]
        float   grid_v, grid_i, grid_p;     // Grid: Spannung, Strom, Leistung
        float   grid_dE, grid_tE;           // Grid: Tagesertrag, Gesamtertrag [kWh]
        float   temp;                        // Wechselrichter-Temperatur [°C]
        int32_t powerLimit;                  // Leistungsbegrenzung [%]
        int32_t powerLimitSet;              // Gesetzter Grenzwert [%]
        bool    inverterActive;             // Wechselrichter aktiv
        int32_t warningsActive;             // Aktive Warnungen
        int32_t wifiRssi;                   // RSSI des DTU-WLAN
        uint32_t timestamp;                  // Unix-Timestamp der letzten Messung
        uint32_t lastResponseMs;            // millis() des letzten Empfangs
        bool    valid;                       // Daten gültig (mindestens 1 Empfang)
    } pv;

    // ── System-Status (geschrieben von taskWiFi / taskDTU / taskMQTT) ────
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
        bool    dtuCloudBusy;               // ERR_RST -14 empfangen
        // MQTT
        bool    mqttConnected;
        uint32_t mqttLastConnectMs;
        // System
        uint32_t uptimeS;
        uint32_t freeHeap;
        String  fwVersion;
        int     buildNumber;
        String  macAddress;
        uint32_t ntpTime;                   // Unix-Timestamp (NTP)
    } system;

    // ── GPIO-Zustand (geschrieben von taskGPIO) ───────────────────────────
    struct GpioState {
        bool relay;                          // Relay-Zustand
        bool gpio[4];                        // GP1–GP4 Zustände
    } gpio;

    // ── GPIO-Befehle (geschrieben von taskMQTT / taskWeb / taskSerial) ───
    struct GpioCommand {
        bool    pending;
        int     target;                      // 0=Relay, 1-4=GP1-GP4
        bool    state;
    } gpioCmd;

    // ── DTU-Steuerbefehle ─────────────────────────────────────────────────
    struct DtuCommand {
        bool    setPowerLimit;
        int     powerLimitValue;             // [%]
        bool    rebootDtu;
        bool    rebootInverter;
        bool    setInverterOn;
        bool    inverterOnValue;
    } dtuCmd;

    // ── Mutex ──────────────────────────────────────────────────────────────
    SemaphoreHandle_t mutex;
};

extern DataStore ds;
```

### 2.2 DataStore API

```cpp
// Initialisierung (in main.cpp vor Task-Start)
void dsInit();

// Lesen (gibt Kopie zurück — kein langer Lock nötig)
DataStore::PvData      dsGetPv();
DataStore::SystemStatus dsGetSystem();
DataStore::GpioState   dsGetGpio();

// Schreiben (atomisch mit Mutex)
void dsSetPv(const DataStore::PvData& data);
void dsSetSystem(const DataStore::SystemStatus& status);
void dsSetGpio(const DataStore::GpioState& state);
void dsSetGpioCommand(int target, bool state);
void dsSetDtuCommand(const DataStore::DtuCommand& cmd);
void dsClearDtuCommand();

// Convenience
bool dsIsDtuOnline();
bool dsIsWifiConnected();
bool dsPvValid();
```

---

## 3. Task-Architektur

### 3.1 Übersicht

| Task | Core | Prio | Stack | Funktion |
|---|---|---|---|---|
| taskWiFi | 1 | 5 | 6144 | WiFi-Verbindung, AP-Modus, NTP |
| taskDTU | 1 | 4 | 8192 | DTU TCP-Verbindung, Protokoll, Daten |
| taskMQTT | 1 | 3 | 6144 | MQTT-Verbindung, Publish, Subscribe |
| taskWebServer | 1 | 3 | 8192 | HTTP-API, OTA, Static Files |
| taskGPIO | 1 | 4 | 4096 | Relay/GP Ein-/Ausgabe |
| taskLED | 1 | 2 | 3072 | NeoPixel Status-Anzeige |
| taskSerial | 1 | 2 | 4096 | Konsolen-Kommandos |
| taskSysMonitor | 1 | 1 | 3072 | Heap-Überwachung, Uptime |

**Core 0:** Ausschliesslich WiFi-Stack (lwIP) — keine User-Tasks  
**Core 1:** Alle User-Tasks

### 3.2 Start-Sequenz

```
main.cpp:
  1. dsInit()                    // DataStore initialisieren
  2. Serial.begin()
  3. LittleFS.begin()
  4. configLoad()
  5. xTaskCreatePinnedToCore(taskWiFi, ...)     // startet zuerst
  6. xTaskCreatePinnedToCore(taskLED, ...)
  7. xTaskCreatePinnedToCore(taskGPIO, ...)
  8. xTaskCreatePinnedToCore(taskSerial, ...)
  9. xTaskCreatePinnedToCore(taskSysMonitor, ...)
  10. xTaskCreatePinnedToCore(taskDTU, ...)     // wartet auf EVT_WIFI_CONNECTED
  11. xTaskCreatePinnedToCore(taskMQTT, ...)    // wartet auf EVT_WIFI_CONNECTED
  12. xTaskCreatePinnedToCore(taskWebServer, ...)
```

### 3.3 Synchronisation

Für systemweite Events wird ein FreeRTOS EventGroup verwendet:

```cpp
// Bits (in systemState.h)
#define EVT_WIFI_CONNECTED    BIT0
#define EVT_WIFI_AP_MODE      BIT1
#define EVT_DTU_ONLINE        BIT2
#define EVT_MQTT_CONNECTED    BIT3
#define EVT_DATA_RECEIVED     BIT4
#define EVT_OTA_RUNNING       BIT5
#define EVT_FACTORY_RESET     BIT6
```

Tasks setzen und lesen diese Bits. Der DataStore enthält die detaillierten Zustände.

---

## 4. DTU-Protokoll

### 4.1 Verbindung

- **Adresse:** `appConfig.dtuHost:appConfig.dtuPort` (default: Port 10081)
- **Protokoll:** Rohes TCP (kein HTTP)
- **Library:** AsyncTCP
- **RX-Timeout:** 60s (`setRxTimeout(60)`) — länger als Poll-Interval, damit die Verbindung zwischen Zyklen offen bleibt

### 4.2 Paket-Format

```
Byte 0-1:   0x48 0x4D          (Header-Magic)
Byte 2-3:   [cmd0] [cmd1]      (Command)
Byte 4-5:   0x00 0x01          (konstant)
Byte 6-7:   [CRC16-MODBUS-hi] [CRC16-MODBUS-lo]  (über Payload)
Byte 8-9:   [total-len-hi] [total-len-lo]          (10 + Payload-Länge)
Byte 10+:   [Protobuf-Payload]
```

**CRC16:** MODBUS-Variante (Initial=0xFFFF, Poly=0x8005, RefIn=true, RefOut=true)

### 4.3 Befehle

| Command | Bytes | Richtung | Beschreibung |
|---|---|---|---|
| AppInformation | 0xa3 0x01 | → DTU | Handshake (sofort nach Connect) |
| RealDataNew | 0xa3 0x11 | → DTU | Echtzeit-PV-Daten anfordern |
| GetConfig | 0xa3 0x09 | → DTU | Konfiguration (Power Limit) |
| SetPowerLimit | 0xa3 0x0e | → DTU | Leistungsbegrenzung setzen |

### 4.4 Ablauf pro Zyklus

```
Beim Connect (einmalig):
  1. TCP Connect zu DTU
  2. Sofort in onConnect-Callback: AppInformation senden (0xa3 0x01)
  3. Warten auf AppInfo-Response (max. 8s) — DIREKT nach Connect,
     VOR dem Poll-Interval, damit die Response nicht durch den
     RX-Timeout verloren geht
  4. TCP-Verbindung bleibt offen (RxTimeout=60s > Poll-Interval)

Pro Poll-Interval (dtuInterval Sekunden):
  5. RealDataNew senden (0xa3 0x11)
  6. Warten auf Response (max. 5s)
  7. Daten parsen → dsSetPv()  →  setLedState(LED_DATA_FLASH)
  8. GetConfig senden (0xa3 0x09)
  9. Warten auf Response (max. 3s, non-fatal bei Timeout)
  10. Power Limit + DTU RSSI → dsSetPv()
  11. dsSetPv() + setDtuOnline(true) → MQTT publiziert automatisch
  12. Weiter bei Schritt 5

Wichtig — onData Flag-Reihenfolge:
  _appReady / _dataReady / _cfgReady akkumulieren sich (true bleibt true).
  Nur sendRealDataNew() setzt _dataReady=false, sendGetConfig() setzt _cfgReady=false.
  waitFor() darf Flags NICHT zurücksetzen, sonst bricht die Detektionskette.

Bei ERR_RST (-14): Cloud-Sync-Pause, dann neu verbinden (ab Schritt 1)
```

### 4.5 Cloud-Sync-Pause

Der DTU kommuniziert alle 5 Minuten mit der Hoymiles Cloud:
- Zeitfenster: Sekunde >= 50 wenn Minute % 5 == 4
- Dauer: ~30 Sekunden
- Symptom: TCP error -14 (ERR_RST)
- Verhalten: `dtuCloudPause` Sekunden warten, dann erneut versuchen


---

## 4a. Power Limit — Timeout-Mechanismus

Der Wechselrichter-Leistungsgrenzwert wird per MQTT oder Web-GUI gesetzt. Um Sicherheit zu gewährleisten fällt der Wert nach einem konfigurierbaren Timeout automatisch auf den Default-Wert zurück.

### Ablauf

```
MQTT/Web: setPowerLimit(80%)
  → taskDTU setzt Limit auf 80%
  → Timer startet (powerLimitTimeout Sekunden)

Falls kein erneutes setPowerLimit vor Ablauf:
  → Timer läuft ab
  → taskDTU setzt Limit zurück auf powerLimitDefault (100%)
  → Log: [WRN] [DTU] Power limit timeout — reset to 100%
  → MQTT publish: {topic}/inverter/PowerLimit = 100
```

### Konfiguration

| Parameter | Default | Beschreibung |
|---|---|---|
| `powerLimitDefault` | 100 | Rückfall-Wert [%] |
| `powerLimitTimeout` | 0 | Timeout [s], 0 = deaktiviert |

### Implementierung in taskDTU

```cpp
// In der Task-Loop:
if (_powerLimitPending) {
    dtuSetPowerLimit(_pendingLimit);
    _powerLimitSetAt = millis();
    _powerLimitPending = false;
}

// Timeout-Check:
if (appConfig.powerLimitTimeout > 0 && _powerLimitSetAt > 0) {
    if ((millis() - _powerLimitSetAt) > (uint32_t)appConfig.powerLimitTimeout * 1000) {
        if (ds.pv.powerLimit != appConfig.powerLimitDefault) {
            LOG_W(MOD_DTU, "Power limit timeout — reset to %d%%", appConfig.powerLimitDefault);
            dtuSetPowerLimit(appConfig.powerLimitDefault);
        }
        _powerLimitSetAt = 0;  // Timer zurücksetzen
    }
}
```

---

## 5. MQTT-Implementierung

### 5.1 Problem mit PubSubClient

PubSubClient ist synchron-blockierend. `connect()` blockiert den Thread für >1 Sekunde und löst den WiFi-Task-Watchdog auf Core 0 aus. Deshalb wird für die Neuimplementierung **esp-mqtt** (ESP-IDF native, non-blocking) oder eine eigene Lösung verwendet.

### 5.2 Vorgeschlagene Lösung: esp_mqtt

```cpp
// Nicht-blockierender MQTT-Client (ESP-IDF native, flat 4.x API)
// Hinweis: framework-arduinoespressif32 @ 3.x bundelt ESP-IDF 4.x →
//          flat struct fields, NICHT die nested 5.x API (broker.address.uri)
#include "mqtt_client.h"

esp_mqtt_client_config_t cfg = {};
cfg.uri         = "mqtt://10.1.1.41:1883";
cfg.client_id   = "hmsgws3";
cfg.keepalive   = 60;
cfg.lwt_topic   = "hmsgws3/LWT";
cfg.lwt_msg     = "offline";
cfg.lwt_msg_len = 7;
cfg.lwt_retain  = 1;
esp_mqtt_client_handle_t client = esp_mqtt_client_init(&cfg);
esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqttEventHandler, NULL);
esp_mqtt_client_start(client);
```

Alle Callbacks laufen asynchron — kein Blocking, kein Watchdog.

### 5.3 Topics

```
{mqttTopic}/grid/U                 Netzspannung [V]
{mqttTopic}/grid/I                 Netzstrom [A]
{mqttTopic}/grid/P                 Netzleistung [W]
{mqttTopic}/grid/dailyEnergy       Tagesertrag [kWh]
{mqttTopic}/grid/totalEnergy       Gesamtertrag [kWh]
{mqttTopic}/pv0/U, /pv0/I, /pv0/P PV1
{mqttTopic}/pv1/U, /pv1/I, /pv1/P PV2
{mqttTopic}/inverter/Temp          Temperatur [°C]
{mqttTopic}/inverter/PowerLimit    Leistungsgrenze [%]
{mqttTopic}/relay/state            Relay-Zustand (0/1)
{mqttTopic}/gpio{1-4}/state        GPIO-Zustand (0/1)
{mqttTopic}/system/uptime          Uptime [s]
{mqttTopic}/system/rssi            WLAN-RSSI [dBm]
{mqttTopic}/system/heap            Freier Heap [Bytes]

# Steuertopics (Subscribe)
{mqttTopic}/relay/set              Relay setzen (0/1)
{mqttTopic}/gpio{1-4}/set         GPIO setzen (0/1)
{mqttTopic}/inverter/PowerLimitSet/set  Leistungsgrenze setzen [%]
{mqttTopic}/inverter/On/set        Wechselrichter ein/aus (0/1)
{mqttTopic}/inverter/RebootDtu/set DTU neustarten (1)
{mqttTopic}/inverter/RebootGw/set  Gateway neustarten (1)
```

### 5.4 HA Auto-Discovery

Discovery-Nachrichten werden 5 Sekunden nach MQTT-Connect gesendet, eine pro 500ms (verhindert Watchdog-Reset durch Burst).

---

## 6. Web-API

### 6.1 Endpoints

| Method | Path | Beschreibung |
|---|---|---|
| GET | `/api/data.json` | PV-Echtzeit-Daten |
| GET | `/api/info.json` | System-Info, Verbindungsstatus |
| GET | `/api/config` | Aktuelle Konfiguration |
| POST | `/api/config` | Konfiguration speichern |
| GET | `/api/gpio` | GPIO-Zustand |
| POST | `/api/gpio` | GPIO setzen |
| GET | `/api/dtu` | DTU-Steuerung |
| POST | `/update` | OTA-Firmware-Update |

### 6.2 data.json

```json
{
  "pv0": { "v": 26.6, "i": 1.33, "p": 353.0, "dE": 1.591, "tE": 129.164 },
  "pv1": { "v": 27.7, "i": 1.07, "p": 296.0, "dE": 1.450, "tE": 112.300 },
  "grid": { "v": 241.9, "i": 2.53, "p": 610.0, "dE": 3.041, "tE": 241.464 },
  "inverter": { "temp": 45.2, "powerLimit": 100, "active": true },
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

---

## 7. LED-Zustände (GPIO38, WS2812B)

| Zustand | Farbe | Muster | Bedeutung |
|---|---|---|---|
| LED_BOOT | Weiss | 3× Blinken (120ms) | Bootvorgang |
| LED_WIFI_CONNECTING | Blau | Blinken 1 Hz | WiFi-Verbindungsaufbau |
| LED_AP_MODE | Blau | 3× kurz + Pause | AP-Modus aktiv (braucht Nutzeraktion) |
| LED_DTU_OFFLINE | Orange | Doppelblink + Pause | WiFi OK, DTU offline |
| LED_NO_MQTT | Cyan | Langsamer Puls 4s | WiFi+DTU OK, MQTT offline |
| LED_OPERATIONAL | Grün | Herzschlag 5s | Vollbetrieb, PV aktiv (≥ 1W) |
| LED_STANDBY | Grün (10%) | Sehr langer Puls 10s | Kein PV-Ertrag (Nacht/bewölkt) |
| LED_DATA_FLASH | Weiss | 1× 80ms | Neue DTU-Daten empfangen (transient) |
| LED_OTA | Magenta | Schnell 5 Hz | OTA-Update läuft |
| LED_ERROR | Rot | 4 Hz | Kritischer Fehler |
| LED_FACTORY_RESET | Rot | Dauerhaft | Factory Reset läuft |

Der Zustand wird **auto-deriviert** (`deriveState()`) — kein manuelles `setLedState()` nötig ausser für Transienten (`LED_DATA_FLASH`) und OTA.

---

## 8. Konfiguration (appConfig)

```cpp
struct AppConfig {
    // WiFi
    char wifiSsid[33];
    char wifiPass[65];
    bool wifiApFallback;        // AP-Modus wenn WiFi nicht verfügbar

    // DTU
    char dtuHost[40];
    uint16_t dtuPort;           // default: 10081
    int  dtuInterval;           // Abfrage-Intervall [s], default: 31
    int  dtuCloudPause;         // Wartezeit bei Cloud-Sync [s], default: 30
    int  dtuRebootAfterFails;   // Reconnect nach N Fehlern, default: 3

    // Power Limit
    int  powerLimitDefault;     // Standard-Grenzwert [%], default: 100
    int  powerLimitTimeout;     // Timeout [s] nach dem auf Default zurückgefallen wird
                                // 0 = kein Timeout (Wert bleibt bis manuell geändert)
                                // default: 0

    // MQTT
    char mqttHost[40];
    uint16_t mqttPort;          // default: 1883
    char mqttUser[33];
    char mqttPass[65];
    char mqttTopic[33];         // default: "hmsgws3"
    bool mqttRetain;
    bool mqttHaDiscovery;       // HA Auto-Discovery aktivieren
    bool mqttOpenDtu;           // OpenDTU-kompatible Topics

    // GPIO — Default-Pinbelegung (anpassbar im Web-GUI)
    struct {
        uint8_t pin;            // default: GPIO1
        bool inverted;
    } relay;
    struct {
        uint8_t pin;            // default: GPIO0, GPIO2, GPIO3, (frei)
        enum { GPIO_OUTPUT, GPIO_INPUT, GPIO_I2C_RESERVED } mode;
                                // GPIO_I2C_RESERVED: Pin für zukünftige I2C-Nutzung reserviert
        bool inverted;
        bool pullup;
    } gp[4];

    // LED
    uint8_t ledPin;             // default: GPIO38 (WS2812B onboard)
    uint8_t ledBrightness;      // 0-255, default: 80

    // System
    int  tzOffset;              // Zeitzone-Offset [s], default: 3600 (UTC+1)
    char ntpServer[65];         // default: "pool.ntp.org"
    int  logLevel;              // 0=ERROR, 1=WARN, 2=INFO, 3=DEBUG
};
```

### 8.1 GPIO Default-Pinbelegung

| Funktion | Pin | Mode (Default) | Konfigurierbar |
|---|---|---|---|
| Relay | GPIO1 | OUTPUT | ✅ |
| GP1 | GPIO0 | INPUT | ✅ |
| GP2 | GPIO2 | I2C_RESERVED (SDA) | ✅ |
| GP3 | GPIO3 | I2C_RESERVED (SCL) | ✅ |
| GP4 | — | — | ✅ |
| LED (WS2812B) | GPIO38 | — | ✅ |

GPIO2 (SDA) und GPIO3 (SCL) sind für zukünftige I2C-Sensoren (Temperatur, Feuchte usw.) reserviert. Im Mode `I2C_RESERVED` werden die Pins als Standard-GPIO initialisiert aber im Web-GUI als "reserviert" gekennzeichnet. Die eigentliche I2C-Implementierung erfolgt in einer späteren Version.

Alle Pins sind über das Web-GUI anpassbar und werden in `config.json` gespeichert.

Gespeichert als JSON in LittleFS (`/config.json`).

---

## 9. Web-Dashboard Design

### 9.1 Designprinzipien

Anlehnung an **Shelly Web-UI**: modern, aufgeräumt, professionell.

- **Dark Mode** als Standard (umschaltbar, Präferenz wird lokal gespeichert)
- Farbpalette: Dunkelgrau `#1a1a2e` / `#16213e` Hintergrund, Akzentfarbe Grün `#00b894` für Produktionsdaten
- Typografie: System-Font-Stack (`-apple-system, BlinkMacSystemFont, "Segoe UI"`)
- Karten-Layout (Cards) für Dashboard-Elemente
- Responsive: Mobile-first, funktioniert auf Smartphone ohne Zoomen
- Keine externen Abhängigkeiten — alles in `index.html` (inline CSS + JS)
- Auto-Refresh alle 10 Sekunden via Fetch API

### 9.2 Dashboard-Struktur

```
┌─────────────────────────────────────────────┐
│  HMS-GW-S3          🌙  ⚙️  ℹ️              │  ← Header + Dark/Light Toggle
├─────────────────────────────────────────────┤
│  ┌──────────┐  ┌──────────┐  ┌──────────┐  │
│  │  PV1     │  │  PV2     │  │  Grid    │  │  ← Karten mit Live-Daten
│  │ 353 W    │  │ 296 W    │  │ 610 W    │  │
│  │ 26.6V    │  │ 27.7V    │  │ 241.9V   │  │
│  └──────────┘  └──────────┘  └──────────┘  │
│  ┌─────────────────────────────────────────┐│
│  │  Tagesertrag: 3.04 kWh  Total: 241 kWh ││  ← Energie-Übersicht
│  └─────────────────────────────────────────┘│
│  ┌──────────────┐  ┌──────────────────────┐ │
│  │  Relay  [●]  │  │  GP1 [●]  GP2 [○]   │ │  ← GPIO-Steuerung
│  └──────────────┘  └──────────────────────┘ │
│  ┌─────────────────────────────────────────┐│
│  │  Status: WiFi ✅  DTU ✅  MQTT ✅       ││  ← Verbindungsstatus
│  │  Temp: 45.2°C   Limit: 100%            ││
│  └─────────────────────────────────────────┘│
└─────────────────────────────────────────────┘
```

### 9.3 Seiten

- **Dashboard** — Live-Daten, GPIO-Steuerung, Status
- **Config** — WiFi, DTU, MQTT, GPIO, System (Tab-Navigation)
- **System** — OTA-Update, Log-Level, Factory Reset, Reboot

---

## 10. OTA-Update

Zwei Methoden:

### 10.1 File Upload (lokal)

Im Web-GUI unter System → OTA:
- Firmware: `.bin` Datei hochladen → `POST /update` → ESPAsyncWebServer + Update-Library
- Filesystem: `.bin` Datei hochladen → `POST /updatefs`
- Fortschrittsanzeige im Browser
- Automatischer Neustart nach erfolgreichem Update

### 10.2 Internet-Update (URL)

- URL eines `.bin` Files eingeben → Gateway lädt direkt herunter
- Verwendet `HTTPClient` + `Update` Library (läuft in eigenem Task)
- Fortschritt wird per WebSocket oder Polling im Browser angezeigt
- Signatur-Prüfung optional (MD5-Hash)

```cpp
// Ablauf Internet-OTA
HTTPClient http;
http.begin(firmwareUrl);
int len = http.GET();
Update.begin(len);
// Stream direkt in Update schreiben
Update.writeStream(*http.getStreamPtr());
Update.end(true);
ESP.restart();
```

---

## 11. Konsole (Serial + Web-Terminal)

### 11.1 Ausgabe-Format

Linux-ähnliche formatierte Ausgabe:

```
[00:01:23.456] [INF] [DTU   ] TCP connected to 10.1.1.143:10081
[00:01:23.789] [INF] [DATA  ] PV1: 26.6V/1.33A/353W  PV2: 27.7V/1.07A/296W
[00:01:23.790] [INF] [DATA  ] Grid: 241.9V/2.53A/610W  Temp: 45.2°C
[00:01:23.791] [DBG] [DTU   ] RX 245 bytes — CRC OK
[00:01:24.001] [WRN] [MQTT  ] Reconnecting... (attempt 3/5)
[00:01:24.500] [ERR] [WIFI  ] Connection lost — RSSI: -85 dBm
```

Format: `[HH:MM:SS.mmm] [LVL] [MODULE] Nachricht`

| Level | Kürzel | Farbe (ANSI) |
|---|---|---|
| ERROR | ERR | Rot `\e[31m` |
| WARNING | WRN | Gelb `\e[33m` |
| INFO | INF | Weiss `\e[0m` |
| DEBUG | DBG | Cyan `\e[36m` |

### 11.2 Log-Level

Konfigurierbar per Web-GUI und Konsolen-Kommando:

```bash
loglevel debug    # Alle Meldungen
loglevel info     # Standard
loglevel warn     # Nur Warnungen und Fehler
loglevel error    # Nur Fehler
```

### 11.3 Konsolen-Kommandos

```bash
help              # Befehlsübersicht
status            # Systemstatus (WiFi, DTU, MQTT, Heap)
config            # Aktuelle Konfiguration anzeigen
restart           # Gateway neustarten
reset             # Factory Reset
wifi              # WiFi-Status
dtu               # DTU-Status und letzte Daten
mqtt              # MQTT-Status
gpio              # GPIO-Zustand
relay on|off      # Relay schalten
gpio1 on|off      # GP1 schalten
loglevel <lvl>    # Log-Level setzen
version           # Firmware-Version
uptime            # Laufzeit
heap              # Heap-Nutzung
tasks             # FreeRTOS Task-Liste
```

### 11.4 Web-Terminal (optional, Phase 2)

Ein Terminal-Fenster im Web-Dashboard das Serial-Output via WebSocket spiegelt — so kann man den Log auch ohne USB-Verbindung lesen.

---

```
HMS-GW-S3/
├── platformio.ini
├── custom_partitions.csv
├── version_inc.py
├── data/
│   └── www/
│       └── index.html          (Dashboard SPA)
├── include/
│   ├── config.h                (Build-Konstanten, Stack-Grössen, Pin-Definitionen)
│   ├── appConfig.h             (AppConfig struct)
│   ├── dataStore.h             (DataStore struct + API)
│   ├── systemState.h           (EventGroup Bits)
│   ├── logger.h                (LOG_I/W/E/D Makros)
│   └── taskLED.h               (setLedState() Deklaration)
└── src/
    ├── main.cpp                (Setup, Task-Start)
    ├── appConfig.cpp           (Config laden/speichern)
    ├── dataStore.cpp           (DataStore Implementation)
    ├── logger.cpp
    ├── taskWiFi.cpp            (WiFi + NTP als eigener Task)
    ├── taskDTU.cpp             (TCP + manuelles Protobuf, kein Nanopb)
    ├── taskMQTT.cpp            (esp-mqtt, non-blocking, ESP-IDF 4.x flat API)
    ├── taskWebServer.cpp       (ESPAsyncWebServer)
    ├── taskGPIO.cpp            (Relay + GP1-4)
    ├── taskNeoPixel.cpp        (WS2812B GPIO38, Zustands-Auto-Derivierung)
    ├── taskSerial.cpp          (Konsole)
    └── taskSysMonitor.cpp      (Heap, Uptime)
```

---

## 12. Datei-Struktur

```
HMS-GW-S3/
├── platformio.ini
├── custom_partitions.csv
├── version_inc.py
├── data/
│   └── www/
│       └── index.html          (Dashboard SPA — Dark Mode, Shelly-Design)
├── include/
│   ├── config.h                (Build-Konstanten, Stack-Grössen, Pin-Defaults)
│   ├── appConfig.h             (AppConfig struct)
│   ├── dataStore.h             (DataStore struct + API)
│   ├── systemState.h           (EventGroup Bits)
│   ├── logger.h                (LOG_I/W/E/D Makros mit ANSI-Farben)
│   └── taskLED.h               (setLedState() Deklaration)
└── src/
    ├── main.cpp                (Setup, Task-Start, dsInit)
    ├── appConfig.cpp           (Config laden/speichern)
    ├── dataStore.cpp           (DataStore Implementation)
    ├── logger.cpp              (Formatierte Ausgabe HH:MM:SS.mmm)
    ├── taskWiFi.cpp            (WiFi + NTP als eigener Task)
    ├── taskDTU.cpp             (TCP + manuelles Protobuf, kein Nanopb)
    ├── taskMQTT.cpp            (esp-mqtt non-blocking, ESP-IDF 4.x flat API)
    ├── taskWebServer.cpp       (HTTP-API, OTA File+URL, Static Files)
    ├── taskGPIO.cpp            (Relay GPIO1, GP1-4)
    ├── taskNeoPixel.cpp        (WS2812B GPIO38, Zustands-Auto-Derivierung)
    ├── taskSerial.cpp          (Konsole mit Kommandos)
    └── taskSysMonitor.cpp      (Heap, Uptime)
```

---

## 13. Implementierungs-Reihenfolge

1. **DataStore** (`dataStore.h` / `dataStore.cpp`) — Fundament
2. **taskWiFi** — WiFi + NTP als eigener Task, setzt EVT_WIFI_CONNECTED
3. **taskDTU** — refactored, liest aus appConfig, schreibt in DataStore
4. **taskWebServer** — liest aus DataStore für API-Responses
5. **taskGPIO** — liest/schreibt GpioState im DataStore
6. **taskLED** — liest SystemStatus aus DataStore für LED-Zustand
7. **taskMQTT** — non-blocking esp-mqtt, liest DataStore, publiziert
8. **taskSerial** / **taskSysMonitor** — lesen DataStore für Status
9. **Web-Dashboard** — angepasst auf neue API

---

## 14. Bekannte Probleme der alten Implementierung (v1)

| Problem | Ursache | Lösung in v2 |
|---|---|---|
| Tasks kennen sich direkt | Globale Variablen (`latestDtuData`, `gpioState`) | DataStore mit API |
| MQTT Watchdog-Reset | PubSubClient blockiert Core 0 | esp-mqtt (non-blocking) |
| WiFi in taskWebServer | WiFi-Init zu früh, falscher Task | Eigener taskWiFi |
| NTP in taskDTU | NTP gehört nicht zum DTU-Task | taskWiFi übernimmt NTP |
| HA Discovery Watchdog | 16 Publishes in einem Block | Zeitgestaffelt, 1 pro 500ms |
| Stack-Overflows | Zu kleine Stacks | Grosszügige Werte in config.h |
| LittleFS vor Mount geöffnet | Reihenfolge in taskWebServer | LittleFS.begin() in main.cpp |
