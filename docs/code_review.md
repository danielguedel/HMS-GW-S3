# Code Review — HMS-GW-S3

**Datum:** 2026-06-15 (re-verifiziert 2026-06-16 nach GPIO-Rename)
**Basis:** commit 4c4983d, re-verifiziert gegen 4bc063a
**Reviewer:** Claude Sonnet 4.6

---

## Update 2026-06-16

Zwischen Erstreview und heute wurde GP1–GP4 auf io1–io3 umbenannt (commit `4bc063a`, siehe `project_v2_status.md`). Dabei wurde **P1 "`pinMode(255, OUTPUT)` für GP4"** als Nebeneffekt behoben — IO3 hat jetzt einen festen Default-Pin (GPIO4) statt 255. Alle übrigen Befunde unten wurden gegen den aktuellen Code re-verifiziert; Zeilennummern und Bezeichner sind aktualisiert, wo betroffen. Inhaltlich unverändert: die beiden verbleibenden P1-Befunde (Race Condition `_rxBuf`/`_rxLen`, `vTaskDelay` in Callbacks) sowie alle P2–P4-Punkte.

---

## Zusammenfassung

Die Implementierung entspricht der Spezifikation v2 in allen Kernpunkten: DataStore-Muster, Task-Prios/Stacks, EventGroup-Bits, LED-Zustände und API-Endpunkte stimmen überein. Die kritischste verbleibende Schwachstelle ist eine Race Condition im `onData`-Callback von AsyncTCP: `_rxBuf` und `_rxLen` werden ohne Mutex aus dem ISR-Kontext geschrieben und im Task-Kontext gelesen — bei sehr kurzen Antwortintervallen ist Datenverfälschung möglich. Daneben gibt es eine direkte `ds.mutex`-Nutzung in `taskGPIO` und `taskDTU` (Bypass der API), eine `vTaskDelay`-Nutzung in einem AsyncWebServer-Callback (blockiert lwIP-Thread), und mehrere kleinere Punkte zu Fehlerbehandlung und Konfigurationsvalidierung. Der Gesamtzustand ist produktionstauglich; die verbleibenden P1-Befunde sollten priorisiert behoben werden.

---

## 1. Spezifikations-Konformität

### Konform

- **Task-Prioritäten** (Spec §3.1): Alle 8 Tasks korrekt implementiert — `config.h:79–87` stimmt 1:1 mit Tabelle überein.
- **Stack-Grössen** (Spec §3.1): Alle Werte in `config.h:90–98` stimmen überein (WiFi 6144, DTU 8192, MQTT 6144, WebServer 8192, GPIO 4096, LED 3072, Serial 4096, SysMonitor 3072).
- **Core-Zuweisung** (Spec §3.1): Alle Tasks auf Core 1 (`config.h:100–111`). Core 0 frei für WiFi-Stack.
- **Start-Sequenz** (Spec §3.2): `main.cpp:31–84` — dsInit → Serial → LittleFS → configLoad → systemStateEvents → Tasks in exakt der spezifizierten Reihenfolge.
- **EventGroup-Bits** (Spec §3.3): `systemState.h:6–12` — alle 7 Bits BIT0–BIT6 vorhanden und korrekt benannt.
- **DTU-Paketformat** (Spec §4.2): Header 0x48 0x4M + CRC16-MODBUS korrekt in `taskDTU.cpp:44–58`.
- **Befehle** (Spec §4.3): AppInformation 0xa3 0x01, RealDataNew 0xa3 0x11, GetConfig 0xa3 0x09, SetPowerLimit 0xa3 0x0e — alle implementiert.
- **AppInfo sofort nach Connect** (Spec §4.4): `onConnect`-Callback sendet AppInfo unmittelbar (`taskDTU.cpp:265–276`).
- **Flag-Reihenfolge** (Spec §4.4): `onData` akkumuliert `_appReady / _dataReady / _cfgReady` korrekt; nur `sendRealDataNew()` setzt `_dataReady=false`, nur `sendGetConfig()` setzt `_cfgReady=false` (`taskDTU.cpp:314,325`).
- **Cloud-Sync-Pause** (Spec §4.5): ERR_RST (-14) erkannt in `onError`, Pause in Task-Loop (`taskDTU.cpp:287,427–438`).
- **Power-Limit-Timeout** (Spec §4a): Vollständig implementiert in `taskDTU.cpp:384–397`, Logausgabe `[WRN] Power limit timeout — reset to N%`.
- **esp-mqtt flat API** (Spec §5.2): `taskMQTT.cpp:323–340` — flat struct, kein nested 5.x API. Persistente Strings `_uri/_clientId/_lwtTopic` korrekt.
- **MQTT-Topics** (Spec §5.3): Alle Publish-Topics implementiert. Subscribe-Topics inkl. `io{1-3}/set` vorhanden.
- **HA Discovery** (Spec §5.4): 5s Delay + 500ms/Entity in `haDiscoveryStep()` (`taskMQTT.cpp:158–184`). 15 Entities (10 Sensoren + 1 PowerLimit + 4 Schalter: relay, io1, io2, io3).
- **Web-API-Endpunkte** (Spec §6.1): Alle 8 Endpunkte registriert (`taskWebServer.cpp:294–318`).
- **data.json** (Spec §6.2): Alle Felder vorhanden, Format korrekt (`taskWebServer.cpp:24–49`).
- **info.json** (Spec §6.3): Alle Felder vorhanden (`taskWebServer.cpp:52–68`).
- **LED-Zustände** (Spec §7): Alle 11 Zustände implementiert; `deriveState()` auto-deriviert (`taskNeoPixel.cpp:43–57`). `setLedState()` nur für Transienten (DATA_FLASH) und explizite Overrides (OTA).
- **AppConfig-Felder** (Spec §8): `appConfig.h` stimmt vollständig mit Spec überein.
- **GPIO-Defaults** (Spec §8.1): Relay GPIO1 (OUTPUT), GPIO0 intern für BOOT/Factory-Reset (kein API-Zugriff), io1 GPIO2 (OUTPUT, altFunction "I2C_SDA"), io2 GPIO3 (OUTPUT, altFunction "I2C_SCL"), io3 GPIO4 (OUTPUT, altFunction "ADC1_CH3") — alle mit festem Default-Pin, alles korrekt.
- **Konsole** (Spec §11.3): 16 Befehle implementiert (`io1`–`io3` statt vormals `gpio1`–`gpio4`) inkl. `ledtest` (Extra).
- **Log-Format** (Spec §11.1): `[HH:MM:SS.mmm] [LVL] [MODULE] msg` in `logger.cpp:44–47`.

### Abweichungen

1. **DTU-Verbindungs-Timeout zu kurz** — Spec §4.4 impliziert Warten bis TCP-Verbindung steht; in `taskDTU.cpp:443` wird nur 3000 ms auf `_connected=true` gewartet (`vTaskDelay(pdMS_TO_TICKS(3000))`). `DTU_CONNECT_TIMEOUT_MS` in `config.h:53` ist 5000 ms — dieser Wert wird aber nicht verwendet; das Hard-Coded `3000` weicht ab.

2. **Kein `/api/dtu` GET in Spec §6.1** — Spec listet 8 Endpunkte; `POST /api/dtu` ist vorhanden, aber `GET /api/dtu` (`taskWebServer.cpp:99–111`) und `POST /api/dtu` sind nicht in der Tabelle §6.1. De-facto-Endpunkt ist nützlich, aber nicht spezifiziert.

3. **OTA URL-Methode nicht implementiert** — Spec §10.2 beschreibt Internet-OTA per URL-Eingabe (HTTPClient + Update). Nur File-Upload (§10.1) ist implementiert (`taskWebServer.cpp:248–284`). Kein `/api/ota/url`-Endpunkt.

4. **`tasks`-Konsolenbefehl fehlt** — Spec §11.3 listet `tasks` (FreeRTOS Task-Liste). In `taskSerial.cpp:240–258` nicht implementiert; kein `vTaskList()`-Aufruf.

5. **`heap`-Konsolenbefehl fehlt** — Spec §11.3 listet `heap`. Heapdaten sind in `status` enthalten, aber kein eigenständiger `heap`-Befehl.

6. **`uptime`-Konsolenbefehl fehlt** — Spec §11.3 listet `uptime`. Ebenfalls nur in `status` enthalten.

7. **LED_DATA_FLASH ist Orange** — Spec §7 sagt Orange für DATA_FLASH. `taskNeoPixel.cpp:181` verwendet `COL_ORANGE` — korrekt, bereits seit commit `be6d19e` gefixt.

---

## 2. Task-Abhängigkeiten

Das DataStore-Muster ist überwiegend sauber eingehalten. Kein Task kennt einen anderen Task direkt. Es gibt jedoch zwei Stellen, wo die API umgangen wird:

### Direkter `ds.mutex`-Zugriff (Bypass der DataStore-API)

**`taskDTU.cpp:404–407`:**
```cpp
xSemaphoreTake(ds.mutex, portMAX_DELAY);
cmd = ds.dtuCmd;
xSemaphoreGive(ds.mutex);
```
Anstatt `dsGetDtuCommand()` (existiert nicht in API) direkten Mutex-Zugriff. Der Effekt ist korrekt, aber `ds.*` wird direkt referenziert statt über die API. Es gibt keine `dsGetDtuCommand()`-Funktion in `dataStore.h`.

**`taskGPIO.cpp:88–94`:**
```cpp
xSemaphoreTake(ds.mutex, portMAX_DELAY);
bool pending = ds.gpioCmd.pending;
int  target  = ds.gpioCmd.target;
bool state   = ds.gpioCmd.state;
if (pending) ds.gpioCmd.pending = false;
xSemaphoreGive(ds.mutex);
```
Gleiche Situation: kein `dsGetGpioCommand()` / `dsClearGpioCommand()` in der API. Das `pending=false`-Setzen innerhalb des Locks ist korrekt, aber es bricht das "Never access ds.* directly"-Prinzip aus `dataStore.h:9`. (Befund bleibt nach GPIO-Rename inhaltlich unverändert, nur Zeilennummer verschoben und Bereich jetzt `target` 1–3 statt 1–4.)

### Cross-Task `setLedState()`-Aufrufe

`setLedState()` wird aus drei verschiedenen Tasks aufgerufen:
- `taskDTU.cpp:522`: `setLedState(LED_DATA_FLASH)` — aus taskDTU
- `taskWebServer.cpp:254,276`: `setLedState(LED_OTA)` — aus AsyncWebServer-Callback-Kontext
- `taskGPIO.cpp:38`: `setLedState(LED_FACTORY_RESET)` — aus taskGPIO (unverändert)

Die Funktion schreibt auf `volatile bool _dataFlash` und `volatile LedState_t _currentState` (`taskNeoPixel.cpp:34–40`). Da kein Mutex verwendet wird, ist dies technisch eine Race Condition, aber in der Praxis unkritisch da `bool` und `LedState_t` (4 Bytes, aligned) auf ESP32 atomar geschrieben werden. Es fehlt jedoch ein formales Speichermodell-Guarantee.

### Fazit Task-Abhängigkeiten

Keine direkten Task-zu-Task-Aufrufe. Alle Kommunikation über DataStore oder systemStateEvents. Die zwei direkten `ds.*`-Zugriffe sind ein Designproblem (fehlende API-Funktionen), kein Sicherheitsproblem.

---

## 3. FreeRTOS-Sicherheit

### KRITISCH

**3.1 Race Condition: `_rxBuf` / `_rxLen` ohne Synchronisation** — `taskDTU.cpp:248–249, 253–262`

`onData` (AsyncTCP-Callback, läuft im lwIP-/AsyncTCP-Task auf Core 0) schreibt in `_rxBuf[2048]` und `_rxLen` ohne Mutex:
```cpp
static uint8_t         _rxBuf[2048];
static volatile size_t _rxLen     = 0;

static void onData(...) {
    size_t copy = len < sizeof(_rxBuf) ? len : sizeof(_rxBuf);
    memcpy(_rxBuf, data, copy);   // <-- kein Lock
    _rxLen = copy;                 // <-- kein Lock
    ...
    if (!_appReady) { _appReady = true; return; }
    ...
}
```
Und in `taskDTU` (Core 1) wird gelesen:
```cpp
// taskDTU.cpp:495-496
localLen = _rxLen;
memcpy(localBuf, (const uint8_t*)_rxBuf, localLen);
```
Zwischen `_rxLen = copy` (onData) und `localLen = _rxLen; memcpy(...)` (taskDTU) kann ein weiteres Paket eintreffen (z.B. wenn DTU schnell antwortet), was zu partiell überschriebenen Daten führt. In der Praxis selten, da das Protokoll request-response ist und `waitFor(_dataReady, 5000)` vor dem Read steht — aber ein zweites `onData` könnte trotzdem eintreffen (z.B. spätes App-Info-Paket).

**Empfehlung:** `_rxBuf`/`_rxLen` durch eine Queue (`xQueueSend`/`xQueueReceive`) oder einen Mutex schützen.

### WARNUNG

**3.2 `vTaskDelay` in AsyncWebServer-Callback** — `taskWebServer.cpp:122–123, 266`

```cpp
// handleApiDtuPost — AsyncWebServer-Callback-Kontext
vTaskDelay(pdMS_TO_TICKS(500)); ESP.restart();

// handleOtaDone
vTaskDelay(pdMS_TO_TICKS(500)); ESP.restart();
```
AsyncWebServer-Callbacks laufen im lwIP/AsyncTCP-Task (Core 0 oder ein interner Thread). `vTaskDelay` in diesem Kontext blockiert den AsyncTCP-Task und kann zu TCP-Timeouts oder Watchdog-Auslösung führen. Der Restart funktioniert, ist aber nicht sauber. Korrekt wäre: ein Flag setzen (z.B. `EVT_REBOOT` in systemStateEvents) und den Restart aus dem `loop()` oder einem eigenen Task ausführen.

Gleiches gilt für `taskWebServer.cpp:127`:
```cpp
vTaskDelay(pdMS_TO_TICKS(3500)); ESP.restart();  // Factory Reset via Web
```

**3.3 `onConnect`-Callback ruft `dsGetSystem()` auf** — `taskDTU.cpp:271`

```cpp
static void onConnect(void*, AsyncClient* c) {
    ...
    uint32_t ntpTime = dsGetSystem().ntpTime;  // nimmt ds.mutex
    ...
    c->write(...);
}
```
`dsGetSystem()` nimmt `ds.mutex` (`portMAX_DELAY`). Wenn `ds.mutex` gerade von taskDTU, taskMQTT oder taskWiFi gehalten wird, blockiert der AsyncTCP-Callback. Auf einem Single-Core würde dies zu einem Deadlock führen, aber auf Dual-Core ESP32-S3 ist dies ein Livelock-Risiko wenn der Mutex-Halter auch auf einen Callback wartet. In der Praxis unwahrscheinlich (kurze Lock-Dauer), aber formal falsch. Mutex-Timeout statt `portMAX_DELAY` wäre sicherer.

**3.4 Potenzielle Deadlock-Kette: DataStore-Mutex + Log-Mutex** — `logger.cpp:29, dataStore.cpp:49`

`logMsg` nimmt `_logMutex`. `dsGetPv()` nimmt `ds.mutex`. Wenn Thread A `ds.mutex` hält und `logMsg()` aufruft (nimmt `_logMutex`), und gleichzeitig Thread B `_logMutex` hält und `dsGetPv()` aufruft — klassischer Deadlock. In der Implementierung wird aus allen DS-Funktionen heraus nichts geloggt, und aus `logMsg` heraus kein DS-Zugriff gemacht — aber bei zukünftigen Erweiterungen ist dieses Muster gefährlich. Kein echter Deadlock im aktuellen Code; Risiko für die Zukunft.

**3.5 Factory Reset Race Condition** — `main.cpp:93–100` vs. `taskGPIO.cpp:36–42`

```cpp
// main.cpp loop():
if (!_factoryResetHandled &&
    (xEventGroupGetBits(systemStateEvents) & EVT_FACTORY_RESET)) {
    _factoryResetHandled = true;
    LittleFS.remove(CONFIG_FILE);
    // Reboot triggered by task that set EVT_FACTORY_RESET
}

// taskGPIO.cpp checkFactoryReset():
xEventGroupSetBits(systemStateEvents, EVT_FACTORY_RESET);
setLedState(LED_FACTORY_RESET);
vTaskDelay(pdMS_TO_TICKS(1500));
LittleFS.remove(CONFIG_FILE);  // <-- doppeltes Remove
ESP.restart();
```
`LittleFS.remove(CONFIG_FILE)` wird zweimal aufgerufen: einmal in `main.cpp:99` und einmal in `taskGPIO.cpp:41`. Das zweite `remove` auf eine bereits gelöschte Datei ist harmlos (gibt false zurück), aber es zeigt eine Inkonsistenz: der Kommentar in `main.cpp:99` sagt "Reboot is triggered by the task", aber `main.cpp` löscht die Datei selbst auch. Wenn `taskWebServer` den Reset triggert (`taskWebServer.cpp:124–127`), löscht weder `main.cpp` (schnell genug) noch WebServer redundant — aber der Webserver macht `vTaskDelay(3500)` dann `ESP.restart()` ohne `LittleFS.remove`. Der `main.cpp:99`-Handler hat dann 3.5s Zeit — das funktioniert, ist aber fragil.

### INFO

**3.6 `_dataFlash` / `_currentState` ohne formales Speichermodell** — `taskNeoPixel.cpp:34–39`

Beide sind `volatile`, aber `volatile` garantiert auf Xtensa/RISC-V keine atomare Lese-Schreib-Sequenzen für Typen > 1 Byte. `LedState_t` ist ein `typedef enum` (int, 4 Bytes). Auf ESP32 sind aligned 4-Byte-Writes de facto atomar, aber es fehlt `_Atomic` oder ein Mutex. Für den aktuellen Use-Case (ein Schreiber pro Variable, ein Leser) unkritisch.

**3.7 `waitFor()` nutzt 50ms-Polling** — `taskDTU.cpp:342–347`

```cpp
static bool waitFor(volatile bool& flag, uint32_t timeoutMs) {
    uint32_t t0 = millis();
    while (!flag && (millis() - t0) < timeoutMs)
        vTaskDelay(pdMS_TO_TICKS(50));
    return flag;
}
```
Der Task yieldet alle 50ms. Bei `timeoutMs=5000` sind das bis zu 100 Wakeups. Effizienter wäre eine FreeRTOS-Notification oder Semaphore aus dem `onData`-Callback. Funktional korrekt, aber verschwendet CPU-Zyklen.

---

## 4. Speicher

### Stack-Analyse

| Task | Stack konfiguriert | Besonderheiten |
|------|-------------------|----------------|
| taskWiFi | 6144 B | WiFi, NTP, String-Allokationen — ausreichend |
| taskDTU | 8192 B | Protobuf-Decoder auf Stack; `localBuf[2048]` auf Stack (`taskDTU.cpp:379`) — **2 KB Stack-Variable** |
| taskMQTT | 6144 B | esp-mqtt läuft im eigenen Thread; Task selbst nur Poll-Loop — ausreichend |
| taskWebServer | 8192 B | ArduinoJson `JsonDocument` in Callbacks auf Stack; Callbacks laufen im AsyncTCP-Thread, nicht im WebServer-Task |
| taskGPIO | 4096 B | Minimal, ausreichend |
| taskLED | 3072 B | FastLED, nur statische Arrays — ausreichend |
| taskSerial | 4096 B | `buf[256]` auf Stack in `printf_`, `buf[128]` in Task — ausreichend |
| taskSysMonitor | 3072 B | Minimal — ausreichend |

**Kritisch: `taskDTU.cpp:379` — `uint8_t localBuf[2048]` auf Stack**

```cpp
uint8_t  localBuf[2048];  // 2048 Bytes Stack-Allokation in taskDTU
```
Plus `_rxBuf[2048]` als statische Variable (BSS, kein Stack-Problem). Der DTU-Stack ist 8192 Bytes; der Task-Frame + `localBuf[2048]` + Protobuf-Decoder-Frames + FreeRTOS-Overhead lässt schätzungsweise ~3–4 KB Puffer. Kein Overflow in Normalbetrieb, aber bei tiefen Callstacks (z.B. mehrere verschachtelte Funktionen mit eigenen Locals) eng.

**`taskNeoPixel.cpp:23` — `static CRGB leds[LED_COUNT]`**

`LED_COUNT=1`, daher 3 Bytes — kein Problem.

### Heap-Allokationen

- **ArduinoJson `JsonDocument`** in `taskWebServer.cpp` (mehrere Handler) und `taskMQTT.cpp` (HA Discovery): Jede `JsonDocument`-Instanz allokiert im Heap. Bei häufigen API-Aufrufen entsteht Fragmentierung. Da alle Handler im AsyncTCP-Thread laufen und kurzlebig sind, sollte die Fragmentierung begrenzt bleiben.
- **`String`-Objekte in DataStore** (`SystemStatus.wifiIp`, `.wifiSsid`, `.fwVersion`, `.macAddress`): Diese werden beim Schreiben über den Mutex kopiert (`dsSetSystem` macht `ds.system = status` — impliziter String-Copy). Häufige WiFi-Status-Updates erzeugen kleine Heap-Fragmente.
- **`T(suffix)` in `taskMQTT.cpp:23–25`**: `String T(...)` erzeugt bei jedem `pub()`-Aufruf eine temporäre String-Allokation. Bei 20+ Publishes pro Zyklus (PV-Daten) sind das 20+ kurzlebige String-Objekte.

### PSRAM

Das Board hat 8 MB PSRAM (aktiviert via `board_build.arduino.memory_type = qio_opi`). Der Code nutzt PSRAM **nicht explizit** — kein `ps_malloc()`, kein `PSRAM_ATTR`. Der ESP-IDF-Heap-Manager kann PSRAM automatisch für große Allokationen verwenden (wenn `CONFIG_SPIRAM_USE_CAPS_ALLOC` aktiv). Statische Arrays wie `_rxBuf[2048]` landen im IRAM/DRAM-BSS. Explizite PSRAM-Nutzung für `_rxBuf` würde DRAM schonen.

---

## 5. Fehlerbehandlung

### WiFi-Verlust während DTU-Poll

`taskDTU.cpp:469–527`: Bei WiFi-Verlust während `waitFor(_dataReady, 5000)` läuft der Timeout ab, `failCount` wird inkrementiert, nach `dtuRebootAfterFails` Fehlern wird die Verbindung geschlossen. taskDTU wartet danach erneut auf WiFi-Reconnect **nicht** explizit — er versucht sofort `dtuConnect()` → schlägt fehl → `failCount++` → nach 3 Fehlern 30s Cooldown → wiederholt. Das ist funktional, aber taskDTU blockiert nicht sauber auf `EVT_WIFI_CONNECTED`. Nach WiFi-Reconnect arbeitet taskDTU sich nach max. ~90s (3 × 30s) wieder ein. Effizienter wäre: nach N Fehlern auf `EVT_WIFI_CONNECTED` warten.

### MQTT-Disconnect

esp-mqtt reconnect ist automatisch (intern, `reconnect_timeout_ms` default 10s). `MQTT_EVENT_DISCONNECTED` setzt `sys.mqttConnected=false` und `EVT_MQTT_CONNECTED` wird gecleared. Das reicht. Keine manuellen Reconnect-Versuche nötig — korrekt.

### OTA-Fehlerbehandlung

`taskWebServer.cpp:253–259`:
```cpp
if (index == 0) {
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) LOG_E(MOD_OTA, "Update.begin failed");
    // Kein return/Abbruch nach Fehler!
}
if (Update.write(data, len) != len) LOG_E(MOD_OTA, "Write error at %zu", index);
```
Nach `Update.begin(false)` wird weiter in `Update.write()` geschrieben. `Update.write` mit inaktivem Update-Objekt sollte 0 zurückgeben und den Fehlerweg auslösen — das wird gelogt aber nicht abgebrochen. Die Response in `handleOtaDone` prüft `Update.hasError()` und sendet 500. Kein Hardware-Schaden möglich, aber der Fehlerfluss ist unvollständig: nach `Update.begin`-Fehler sollten alle folgenden Chunk-Callbacks ignoriert werden (z.B. mit einem `_otaError`-Flag).

Gleiches gilt für `handleFsUpload` (`taskWebServer.cpp:273–286`).

### Factory Reset Race Condition

Wie in §3.5 beschrieben: doppeltes `LittleFS.remove()` (harmlos) und fragmentierte Verantwortlichkeit. Der WebServer-Weg (`taskWebServer.cpp:124–127`) setzt `EVT_FACTORY_RESET`, wartet 3.5s, bootet — `main.cpp` hat ~3.5s um `LittleFS.remove()` auszuführen. Das ist eine implizite Timing-Abhängigkeit.

### Watchdog-Behandlung

`platformio.ini:24` kommentiert: "Hardware WDT disabled" — aber kein Build-Flag setzt `CONFIG_ESP_TASK_WDT_EN=0`. Der Hardware-Watchdog ist defaultmässig aktiv. Der Software-Watchdog (esp32) ist per Default 30s für den Idle-Task. Alle Tasks nutzen `vTaskDelay` und sind co-operative — der WDT sollte nicht auslösen. Die ehemaligen PubSubClient-WDT-Probleme (Spec §5.1) sind durch esp-mqtt gelöst.

**Nicht implementiert:** Kein explizites `esp_task_wdt_reset()`. Bei sehr langen Blockierungen (z.B. 30s Cooldown in taskDTU: `vTaskDelay(pdMS_TO_TICKS(30000))`) könnte der WDT auslösen wenn der ESP32 Task-WDT für diesen Task registriert ist.

### Konfigurationsvalidierung

`appConfig.cpp:116`: `dtuInterval < DTU_MIN_INTERVAL` wird geclampt — gut.
Aber: Keine Validierung für `powerLimitDefault` (0–100%), `mqttPort` (1–65535), `ledBrightness` (0–255), GPIO-Pin-Nummern (kein Check ob Pin existiert/gültig für ESP32-S3).

**BEHOBEN (2026-06-16):** Der vormalige Befund "ungültige Pin-Nummer 255 für GP4-Default → `pinMode(255, ...)`" ist durch die GPIO-Umbenennung (commit `4bc063a`) gelöst — io3 hat jetzt den festen Default-Pin GPIO4 (`appConfig.cpp` `configSetDefaults()`, `IO3_PIN` in `config.h`). Es gibt keinen GPIO-Slot mehr ohne Default-Pin. Das allgemeine Fehlen einer Pin-Bounds-Validierung (falls ein Nutzer über die API einen ungültigen Pin wie 255 manuell setzt) besteht weiterhin als kleineres Restrisiko, ist aber kein Default-Verhalten mehr.

---

## 6. Sonstige Befunde

### 6.1 `DTU_CONNECT_TIMEOUT_MS` wird nicht verwendet

`config.h:53` definiert `DTU_CONNECT_TIMEOUT_MS 5000`, aber `taskDTU.cpp:443` verwendet hart `3000`:
```cpp
vTaskDelay(pdMS_TO_TICKS(3000));  // sollte DTU_CONNECT_TIMEOUT_MS sein
```
Der Constant ist toter Code.

### 6.2 Doppeltes Config-Load-Log

`appConfig.cpp:56–58` in `main.cpp` loggt Config-Felder. `appConfig.cpp:164–166` in `configLoad()` loggt dieselben Felder nochmals. Das ergibt beim Start zwei identische Log-Zeilen `Config loaded: ssid=...`.

### 6.3 `SemaphoreHandle_t mutex` im DataStore-Struct

`dataStore.h:81`: `SemaphoreHandle_t mutex` ist Teil des öffentlichen `DataStore`-Structs. Das ermöglicht direkten Zugriff wie in `taskDTU.cpp:404` und `taskGPIO.cpp:90`. Der Mutex sollte `private` sein (oder in eine anonyme Namespace versteckt) — bei Arduino/C++ ist das mit einem Wrapper-Objekt lösbar.

### 6.4 `version_inc.py` inkrementiert bei jedem Build

`version_inc.py:19–20`: Buildnummer wird bei jedem `pio run` inkrementiert, auch bei gescheiterten Builds. Das ist ein bekanntes SCons/PlatformIO-Verhalten — der Wert in `include/buildnumber.txt` (gitignored?) ist nach einem Build-Fehler trotzdem höher.

### 6.5 Kein ANSI-Farben im Log

Spec §11.1 spezifiziert ANSI-Farben für Log-Level (Rot/Gelb/Weiss/Cyan). `logger.cpp` hat kein ANSI-Escape-Coding. Kein kritisches Problem, aber Abweichung von der Spezifikation.

### 6.6 `mqttOpenDtu`-Modus publiziert keine GPIO-Daten

`taskMQTT.cpp:46–65`: Im OpenDTU-Modus werden nur PV-Daten publiziert; GPIO-State und System-Stats verwenden `pub()` das den normalen Topic-Prefix nutzt. Das ist konsistent, aber der OpenDTU-Modus ist für GPIO-Steuerung via HA nicht kompatibel (verschiedene Topic-Struktur).

### 6.7 `IoMode` Enum-Wert ohne Bounds-Check via JSON

`appConfig.cpp:147` (vormals `GpMode`/`appConfig.gp[i].mode`, nach GPIO-Rename umbenannt): `appConfig.io[i].mode = (IoMode)io["mode"].as<int>()` — weiterhin kein Bounds-Check. Ein `"mode": 99` im JSON würde zu einem ungültigen `IoMode`-Wert führen, der in `taskGPIO.cpp:57` (`switch (appConfig.io[i].mode)`) kein Case matched (kein `default` im `switch`). Befund bleibt nach dem Rename inhaltlich bestehen.

### 6.8 Fehlender `mqtt/buildnumber.txt` in `.gitignore`

`include/buildnumber.txt` ist nicht in `.gitignore` (nicht verifiziert, aber der `git status` zeigt `M include/buildnumber.txt` als modified — sprich es ist versioniert). Das führt zu einem Commit bei jedem Build.

### 6.9 `ESP.getEfuseMac()` für Topic-Suffix

`appConfig.cpp:38`:
```cpp
snprintf(appConfig.mqttTopic, sizeof(appConfig.mqttTopic),
         "hmsgws3_%06llX", (unsigned long long)(ESP.getEfuseMac() & 0xFFFFFF));
```
Dieser Default-Topic wird nur beim ersten Boot generiert (wenn keine config.json vorhanden). Beim nächsten Aufruf von `configSetDefaults()` (z.B. bei Parse-Fehler) wird ein anderer Suffix generiert wenn die MAC anders gelesen wird — auf ESP32 ist `getEfuseMac()` stabil, das ist kein Bug, aber der Topic-Suffix kann sich nach Factory Reset von der HA-Entity-ID unterscheiden.

---

## Priorisierte Massnahmen

| Prio | Bereich | Problem | Empfehlung |
|------|---------|---------|------------|
| P1 | FreeRTOS | `_rxBuf`/`_rxLen` Race Condition zwischen AsyncTCP-Callback (Core 0) und taskDTU (Core 1) — `taskDTU.cpp:248,495` | FreeRTOS Queue oder Mutex um `_rxBuf`/`_rxLen` schützen; alternativ im Callback direkt parsen und nur Flag + Ergebnis übergeben |
| P1 | FreeRTOS | `vTaskDelay` in AsyncWebServer-Callbacks — `taskWebServer.cpp:122,127,269` | `EVT_REBOOT`-Bit in systemStateEvents setzen; Reboot aus `loop()` oder dediziertem Task ausführen |
| ~~P1~~ | ~~Fehlerbehandlung~~ | ~~`pinMode(255, OUTPUT)` für GP4~~ | **BEHOBEN** in commit `4bc063a` — io3 hat jetzt festen Default-Pin GPIO4, kein Slot ohne Pin mehr |
| P2 | Task-Abhängigkeiten | Direkter `ds.mutex`/`ds.gpioCmd`-Zugriff in taskGPIO und taskDTU — `taskGPIO.cpp:88`, `taskDTU.cpp:404` | `dsGetGpioCommand()`/`dsClearGpioCommand()` und `dsGetDtuCommand()` zur DataStore-API hinzufügen |
| P2 | FreeRTOS | `onConnect`-Callback nimmt `ds.mutex` mit `portMAX_DELAY` — `taskDTU.cpp:271` | Timeout statt `portMAX_DELAY`; NTP-Zeit in lokale Variable cachen die taskDTU regelmässig aktualisiert |
| P2 | Fehlerbehandlung | OTA: kein Abbruch nach `Update.begin`-Fehler — `taskWebServer.cpp:253–259` | `_otaError`-Flag setzen; in Chunk-Callbacks prüfen und früh zurückkehren |
| P3 | Konformität | `DTU_CONNECT_TIMEOUT_MS` nicht genutzt — `taskDTU.cpp:443` / `config.h:53` | `vTaskDelay(pdMS_TO_TICKS(DTU_CONNECT_TIMEOUT_MS))` statt Hardcode `3000` |
| P3 | Konformität | `tasks`, `heap`, `uptime` Konsolenbefehle fehlen — `taskSerial.cpp` | `vTaskList()` für `tasks`; eigenständige Handler für `heap` und `uptime` |
| P3 | Konformität | Internet-OTA (URL) nicht implementiert — Spec §10.2 | `POST /api/ota/url` Handler mit `HTTPClient` + `Update`-Stream |
| P3 | Speicher | `localBuf[2048]` auf DTU-Task-Stack — `taskDTU.cpp:379` | Als statische Variable deklarieren (kein Stack-Druck) oder in PSRAM allokieren |
| P4 | Code-Qualität | Doppeltes `LittleFS.remove(CONFIG_FILE)` beim Factory-Reset — `main.cpp:99`, `taskGPIO.cpp:41` | Verantwortlichkeit konsolidieren: nur `main.cpp` löscht die Datei; tasks setzen nur `EVT_FACTORY_RESET` und booten nach Delay |
| P4 | Code-Qualität | Doppelter Config-Log beim Start — `main.cpp:56`, `appConfig.cpp:164` | Eine der beiden Log-Zeilen entfernen |
| P4 | Code-Qualität | `IoMode`-Enum ohne `default`-Case in taskGPIO switch und ohne Bounds-Check beim JSON-Laden — `appConfig.cpp:147`, `taskGPIO.cpp:57` | Bounds-Check: `(IoMode)(io["mode"].as<int>() <= IO_RESERVED ? ... : IO_INPUT)` |
| P4 | Konformität | ANSI-Farben fehlen im Logger — `logger.cpp` | ANSI-Escape-Codes für Level-Präfix hinzufügen (Spec §11.1) |
| P4 | Projekt | `include/buildnumber.txt` versioniert (erscheint in git status als modified nach jedem Build) | `.gitignore` Eintrag für `include/buildnumber.txt` hinzufügen |
