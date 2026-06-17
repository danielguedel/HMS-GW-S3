# Code Review — HMS-GW-S3

**Datum:** 2026-06-17
**Basis:** Commit `07ffd0d` (fix: Race-Condition FW+FS OTA), abgeglichen gegen die synchronisierte Spezifikation v2 (`docs/HMS-GW-S3_Spezifikation_v2.md`)
**Reviewer:** Claude Sonnet 4.6

---

## Vorbemerkung — Verhältnis zum vorherigen Review

Das vorherige Review (Basis: Commit `4c4983d`/`4bc063a`, 2026-06-15/16) ist inzwischen grösstenteils veraltet: fast alle dort als offen markierten P1/P2-Befunde wurden in der Zwischenzeit behoben. Dieses Review verifiziert den aktuellen Code-Stand neu, Zeile für Zeile, gegen die frisch synchronisierte Spezifikation.

---

## 0. Update 2026-06-17 (Produktionsvorfall) — Config-Verlust bei Filesystem-OTA

Nach diesem Review trat auf einem produktiven Gateway ein realer Vorfall auf: ein Internet-OTA (Firmware + Filesystem) führte dazu, dass nach dem Reboot WiFi/DTU/MQTT/GPIO-Konfiguration komplett auf Werkseinstellungen zurückgesetzt waren. Per Serial-Log eindeutig belegt:

```
[INF] [OTA ] URL-OTA FS complete: 524288 B
... (Reboot) ...
[E] open(): /littlefs/config.json does not exist, no permits for creation
[E] open(): /littlefs/config.tmp does not exist, no permits for creation
[E][WiFiSTA.cpp:232] begin(): SSID too long or missing!
```

**Root Cause:** `Update.begin(..., U_SPIFFS)` (sowohl in `handleFsUpload()` als auch in `doUrlOtaPartition()`) überschreibt die komplette LittleFS-Partition mit dem von CI gebauten `littlefs.bin`, das nur `data/www/*` enthält — `/config.json` ist bewusst gitignored und nie Teil des Build-Images. Jedes Filesystem-OTA (egal ob lokaler Upload oder Internet-URL) hat daher bisher zwangsläufig die laufzeit-persistierte Config gelöscht.

**Fix:** `backupConfigBeforeFsOta()`/`restoreConfigAfterFsOta()` in `taskWebServer.cpp` — sichert `/config.json` vor dem `Update.begin(..., U_SPIFFS)`-Aufruf in den RAM, remounted LittleFS nach `Update.end()` und schreibt die Config zurück. In beiden FS-OTA-Pfaden (Upload + URL) sowie deren Fehlerpfaden (best effort) verdrahtet.

## 0.1 Update 2026-06-17 (Nachtrag) — Vermeintlicher "OTA-Revert"-Bug war ein Versionierungsfehler im Release-Workflow

Nach dem Config-Fix wurde wiederholt beobachtet, dass ein Internet-OTA (z. B. auf "Build 211") nach Download/Schreiben/`Update.end()` — allesamt erfolgreich, inkl. MD5-Verifikation — nach dem Reboot eine ÄLTERE Build-Nummer meldete. Tiefe Diagnose (otadata-Status-Logging via neuem `otainfo`-Konsolenbefehl, direkte Flash-Auslese der OTA-Partitionen per `esptool read_flash` + MD5-Vergleich, vollständiger Werks-Reflash von Bootloader+Partitionstabelle, Timing-Variation vor `ESP.restart()`) zeigte: **die OTA-Partition (`app1`) enthielt durchgehend exakt das korrekte, MD5-verifizierte Firmware-Image** — kein Schreib-, Bootloader- oder Partitions-Problem.

**Tatsächliche Ursache:** `extra_scripts = pre:version_inc.py` (`platformio.ini`) inkrementiert `include/buildnumber.txt` bei **jedem** `pio run`-Aufruf, ohne Rücksicht auf das Target. Der Release-Workflow rief `pio run` (Firmware) **und separat** `pio run -t buildfs` (Filesystem) auf — das inkrementiert den Zähler zweimal pro Release. `firmware.bin` für "Build N" hatte dadurch tatsächlich `BUILD_NUMBER=N-1` einkompiliert, während Manifest und Git-Tag den Wert NACH dem zweiten (Filesystem-)Lauf zeigten (`N`). Die Firmware lief die ganze Zeit korrekt — nur mit einer um 1 niedrigeren, aber intern konsistenten Versionsnummer als im Manifest behauptet.

**Fix:** `.github/workflows/release.yml` liest die Build-Nummer jetzt sofort nach dem Firmware-Build (vor `buildfs`) und schreibt sie nach dem Filesystem-Build explizit zurück in `include/buildnumber.txt`, damit der committete Wert wieder mit dem tatsächlich einkompilierten `BUILD_NUMBER` übereinstimmt.

**Lektion:** Bei einem reproduzierbaren, aber unplausiblen Befund (Schreiben erfolgreich, Inhalt verifiziert korrekt, trotzdem "falsches" Verhalten) zuerst die Versionsbuchhaltung selbst hinterfragen, bevor tiefer in Firmware-/Hardware-Ebenen gegraben wird — der Fehler lag hier nie im Code, der tatsächlich ausgeführt wurde, sondern in der Beschriftung.

Dieser Befund hätte bei der vorherigen Review-Runde auffallen können/sollen — die Internet-OTA-Implementierung wurde zwar gegen die Spec abgeglichen, aber nicht gegen den Effekt eines `U_SPIFFS`-Updates auf bereits vorhandene Laufzeitdateien auf derselben Partition geprüft.

---

## 1. Spezifikations-Konformität

Die Spezifikation wurde vor diesem Review gegen den Code abgeglichen und aktualisiert (DataStore-API, EventGroup-Bits, Web-API-Endpunkte, AppConfig-Felder, Internet-OTA-Ablauf, Logger-Farben, Konsolen-Kommandos, Web-Dashboard-Tabs). Es bestehen daher **keine offenen Abweichungen** zwischen Code und Spec — mit zwei Ausnahmen, die in der Spec selbst explizit als "geplant, noch nicht implementiert" markiert sind:

- **§6.4 Zugriffsschutz und Port:** Benutzername/Passwort-Schutz (`webAuthEnabled`/`webUser`/`webPass`) und konfigurierbarer Web-Port (`webPort`) sind spezifiziert, aber im Code (`taskWebServer.cpp`, `appConfig.h`) noch nicht vorhanden. `AsyncWebServer server` ist weiterhin ein globales statisches Objekt mit fixem `WEB_DEFAULT_PORT` (`taskWebServer.cpp:21`, `config.h:62`). Kein Bug — schlicht noch nicht umgesetzt.

Alle übrigen Abschnitte (DataStore, EventGroup, DTU-Protokoll, MQTT-Topics, Web-API, LED-Zustände, Konsole, Dateistruktur) stimmen 1:1 mit dem Code überein.

---

## 2. Task-Abhängigkeiten

Das DataStore-Muster ist jetzt vollständig eingehalten. Der frühere direkte `ds.mutex`-Zugriff in `taskGPIO.cpp` und `taskDTU.cpp` wurde durch `dsGetGpioCommand()`/`dsGetDtuCommand()` ersetzt (`dataStore.cpp:72-85`) — kein Task greift mehr unter Umgehung der API auf `ds.*` zu.

**Verbleibend (kein Bug, Designhinweis):** `ds.mutex` ist weiterhin Teil des öffentlichen `DataStore`-Structs (`dataStore.h:121`) und könnte theoretisch direkt verwendet werden. Die Spec wurde mit einem expliziten Hinweis ergänzt ("nie direkt verwenden"); eine technische Durchsetzung (z. B. `private`-Wrapper) gibt es nicht.

> **Update 2026-06-17 (nach diesem Review):** Die zwei unabhängigen `LittleFS.remove(CONFIG_FILE)`-Pfade (§3.2/§6, ehemals P4) sind konsolidiert — `taskWebServer.cpp` löscht die Config nicht mehr selbst, sondern setzt nur noch `EVT_FACTORY_RESET`/`EVT_REBOOT`. Alleiniger Eigentümer ist jetzt `main.cpp`s `loop()`.

`setLedState()` wird weiterhin aus drei Tasks aufgerufen (`taskDTU`, `taskWebServer`, `taskGPIO`) und schreibt ungeschützt auf `volatile bool`/`volatile LedState_t` (`taskNeoPixel.cpp:34-39`). Auf ESP32 sind das atomare 1-/4-Byte-Writes — unkritisch, aber ohne formales Speichermodell-Guarantee (unverändert seit letztem Review).

---

## 3. FreeRTOS-Sicherheit

### Korrektur gegenüber dem vorherigen Review

**Der vormalige P1-Befund "`_rxBuf`/`_rxLen` Race Condition" ist falsch / bereits behoben.** Bei genauerer Prüfung (nicht nur Grep auf `_rxBuf`/`_rxLen`, sondern auf den vollständigen Funktionskörper) zeigt sich: Es existiert ein dediziertes `_rxMutex` (`taskDTU.cpp:251`), das sowohl in `onData()` (Z. 257-260, AsyncTCP-Callback auf Core 0) als auch an beiden Lesestellen in `taskDTU` (Z. 496-499, 513-516, Core 1) korrekt per `xSemaphoreTake`/`xSemaphoreGive` genommen wird. Das Mutex wird in `taskDTU()` (Z. 370) erzeugt, bevor `dtuConnect()` und damit die Callback-Registrierung erfolgt — kein Initialisierungs-Race. Die im vorherigen Review beschriebene Race Condition existiert im aktuellen Code nicht. Kein Handlungsbedarf.

### Behoben seit letztem Review

- **`vTaskDelay` + `ESP.restart()` in AsyncWebServer-Callbacks** — jetzt über `EVT_REBOOT`/`EVT_FACTORY_RESET`-Bits deferred; der eigentliche Restart läuft in der Task-Loop von `taskWebServer` (`taskWebServer.cpp:574-580`), nicht mehr im lwIP/AsyncTCP-Thread.
- **`onConnect`-Callback mit `dsGetSystem()` + `portMAX_DELAY`** — ersetzt durch `_ntpTimeCache` (lock-freier `volatile uint32_t`, vor dem Connect aus dem Task-Kontext gefüllt). Kein Mutex-Zugriff mehr im AsyncTCP-Callback (`taskDTU.cpp:252, 442`).
- **`_rxBuf`/`_rxLen` Race Condition** — siehe Korrektur oben, war kein aktueller Befund.

### WARNUNG

**3.2 Potenzielle Deadlock-Kette: DataStore-Mutex + Log-Mutex** — `logger.cpp`, `dataStore.cpp`

Unverändert, weiterhin nur theoretisches Risiko: Aktuell ruft keine DataStore-Funktion `logMsg()` auf und umgekehrt. Bleibt ein Risiko für künftige Erweiterungen, kein aktueller Bug.

### INFO

**3.3 `waitFor()` nutzt 50ms-Polling** — `taskDTU.cpp` — unverändert, funktional korrekt, aber unnötige Wakeups statt FreeRTOS-Notification/Semaphore.

**3.4 `_dataFlash`/`_currentState` ohne formales Speichermodell** — `taskNeoPixel.cpp:34-39` — unverändert, in der Praxis unkritisch (siehe Abschnitt 2).

---

## 4. Speicher

### Behoben seit letztem Review

**`localBuf[2048]` ist jetzt `static`** statt Stack-Variable (`taskDTU.cpp:384`) — kein Stack-Druck mehr im 8192-Byte-`taskDTU`-Stack.

### Unverändert (Info, kein Handlungsbedarf)

- ArduinoJson `JsonDocument` in `taskWebServer.cpp`/`taskMQTT.cpp`-Handlern: Heap-Allokation pro Request, kurzlebig, begrenzte Fragmentierung.
- `String`-Felder in `DataStore::SystemStatus` (`wifiIp`, `wifiSsid`, `fwVersion`, `macAddress`): impliziter Copy bei jedem `dsSetSystem()`.
- `T(suffix)` in `taskMQTT.cpp`: temporäre `String`-Allokation pro `pub()`-Aufruf, bei PV-Daten 20+ pro Zyklus.
- PSRAM (8 MB vorhanden) wird nicht explizit genutzt (`ps_malloc`/`PSRAM_ATTR`); statische Arrays wie `_rxBuf[2048]` liegen im DRAM-BSS.

---

## 5. Fehlerbehandlung

### Behoben seit letztem Review

**OTA-Fehlerbehandlung** — `_otaFwError`/`_otaFsError`-Flags vorhanden (`taskWebServer.cpp:279, 285, 289, 310, 316, 320`). Nach einem `Update.begin()`-Fehler werden nachfolgende Chunk-Callbacks korrekt ignoriert, statt blind weiterzuschreiben.

**Konfigurationsvalidierung — behoben seit diesem Review:** `powerLimitDefault` wird auf 0–100 geclampt, `mqttPort` auf 1–65535 (Fallback `MQTT_DEFAULT_PORT`), `ledBrightness` auf 0–255, und alle GPIO-Pin-Felder (`relay.pin`, `io[i].pin`, `ledPin`) über einen gemeinsamen `clampPin()`-Helper auf den gültigen ESP32-S3-Bereich 0–48 (Fallback auf den Build-Default) — analog zum bereits vorhandenen `dtuInterval`-Clamp (`appConfig.cpp`). `IoMode` hatte bereits vorher einen Bounds-Check (`appConfig.cpp`) und einen `default`-Case im `switch` (`taskGPIO.cpp`).

### Unverändert

- **WiFi-Verlust während DTU-Poll:** `taskDTU` wartet nach Fails nicht explizit auf `EVT_WIFI_CONNECTED`, sondern versucht sofort erneut zu verbinden → bis zu ~90s (3×30s Cooldown) bis zur Wiederherstellung nach WiFi-Reconnect. Funktional korrekt, aber ineffizient.
- **Watchdog:** Kein explizites `esp_task_wdt_reset()`; alle Tasks sind kooperativ (`vTaskDelay`), löst in der Praxis nicht aus. Der 30s-Cooldown in `taskDTU` (`vTaskDelay(pdMS_TO_TICKS(30000))`) bleibt ein theoretisches Risiko, falls der Task-WDT für diesen Task aktiviert würde.

---

## 6. Sonstige Befunde

### Behoben seit letztem Review

- `DTU_CONNECT_TIMEOUT_MS` wird jetzt verwendet (`taskDTU.cpp:444`), kein Hardcode `3000` mehr.
- Doppeltes Config-Log beim Start nicht mehr vorhanden — `main.cpp` loggt keine Config-Felder mehr separat, nur `appConfig.cpp:177`.
- ANSI-Farben im Logger implementiert (`logger.cpp:13-18`) — Spec war veraltet, Code war schon korrekt.
- Konsolenbefehle `tasks`/`heap`/`uptime`/`ledtest` vollständig implementiert (`taskSerial.cpp`).
- Internet-OTA via URL vollständig implementiert (war in Spec v2 nur als Vorhaben beschrieben).
- **Browser-Cache:** `serveStatic` setzt jetzt `Cache-Control: no-cache` (`taskWebServer.cpp:550`) — Filesystem-OTA-Updates sind ohne Hard-Refresh sichtbar.
- **`AsyncClient::close(bool)` Deprecation-Warning** behoben — alle drei Aufrufstellen (`taskDTU.cpp:418, 463, 491`) nutzen jetzt `close()` ohne Argument.
- **Factory-Reset-Doppel-`remove()`** behoben — siehe Update-Hinweis in Abschnitt 2.

### Weiterhin offen

**6.1 `include/buildnumber.txt` nicht in `.gitignore`**

Bewusste Design-Entscheidung (siehe Projekt-Memory): die Datei wird absichtlich getrackt, damit der GitHub-Actions-Release-Workflow auf eine konsistente Buildnummer zugreifen kann. Erscheint deshalb nach jedem lokalen Build als `modified` in `git status` — kein Fix nötig, nur zur Kenntnis.

**6.2 `mqttOpenDtu`-Modus publiziert keine GPIO-Daten** — `taskMQTT.cpp`

Unverändert: im OpenDTU-kompatiblen Modus werden nur PV-Daten im OpenDTU-Schema publiziert; GPIO/System-Topics nutzen weiterhin das normale Schema. Konsistent, aber nicht OpenDTU-kompatibel für GPIO-Steuerung via HA.

---

## Priorisierte Massnahmen

Alle P1–P4-Befunde aus diesem Review sind behoben (Cache-Control-Header, konsolidiertes Factory-Reset-`remove()`, Bounds-Validierung für Config-Felder, `AsyncClient::close()`-Deprecation). Es bleiben nur informative Hinweise ohne Handlungsbedarf:

| Prio | Bereich | Hinweis |
|------|---------|---------|
| Info | Projekt | `include/buildnumber.txt` bewusst getrackt (Release-Workflow) — erscheint nach jedem Build als `modified` |
| Info | MQTT | `mqttOpenDtu`-Modus publiziert GPIO/System nicht im OpenDTU-Schema — nur relevant bei OpenDTU-kompatibler HA-GPIO-Steuerung |
| Info | Build | `version_inc.py` inkrementiert bei jedem Build, auch bei Fehlern — bekanntes SCons-Verhalten |
| Info | FreeRTOS | Theoretische Deadlock-Kette DataStore-Mutex + Log-Mutex, aktuell nicht erreichbar — bei künftigen Erweiterungen beachten: keine Logs aus DataStore-Funktionen, kein DataStore-Zugriff aus `logMsg()` |
| Info | FreeRTOS | `waitFor()` nutzt 50ms-Polling statt Notification/Semaphore — funktional korrekt, geringe CPU-Verschwendung |
| Info | Fehlerbehandlung | `taskDTU` wartet nach WiFi-Verlust nicht explizit auf `EVT_WIFI_CONNECTED` — bis zu ~90s Wiederherstellungszeit nach Reconnect |
