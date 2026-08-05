# Code Review — HMS-GW-S3

---

## Review 2026-08-05

**Based on:** Commit `7a87351`, cross-checked against `docs/HMS-GW-S3_Specification_v2.md`
**Reviewer:** Claude Sonnet 4.6

### Scope

Full re-review of all source files. Primary focus on code added since the 2026-06-17 review: MQTT TLS support (`taskMQTT.cpp`), semver OTA check (`taskWebServer.cpp`), deploy scripts (`getting-started.sh`, `uninstall.sh`). All P1–P4 findings from the previous review are confirmed fixed; none are re-reported.

### Findings — Fixed in this review

**P2-A — `inverter/RebootGw/set` bypassed OTA guard** — `src/taskMQTT.cpp`

The MQTT `RebootGw` handler called `vTaskDelay(200) + ESP.restart()` directly, bypassing the `EVT_OTA_RUNNING` / `EVT_REBOOT` deferred-restart pattern used everywhere else. An MQTT reboot command arriving during a URL-based filesystem OTA would have interrupted the write mid-partition, potentially wiping LittleFS or leaving the device in a state where `backupConfigBeforeFsOta()` ran but `restoreConfigAfterFsOta()` never did.

**Fix:** Handler now sets `EVT_REBOOT` via `xEventGroupSetBits()` and emits a warning log if `EVT_OTA_RUNNING` is active. `taskWebServer`'s loop picks up `EVT_REBOOT` only after any in-progress OTA completes.

**P2-B — Silent truncation of `_caCertPem` buffer** — `src/taskMQTT.cpp`

`f.readBytes(_caCertPem, sizeof(_caCertPem) - 1)` silently truncated the CA PEM at 2047 bytes with no warning. A common user mistake (placing `fullchain.pem` instead of the root CA) produces a 3 500+ byte file that exceeds this limit; the truncated PEM is not a valid certificate and causes TLS handshake failures that are hard to diagnose from the log.

**Fix:** Buffer enlarged to 4096 bytes (accommodates any single-CA file; BSS, zero-cost when not used). Truncation is now detected and logged as `LOG_W` with a hint to use the root CA only.

**P3-A — `handleApiConfigPost` missing input validation** — `src/taskWebServer.cpp`

`POST /api/config` applied pin numbers, port, and brightness values directly to `appConfig` without range checks, while `applyConfigJson()` in `appConfig.cpp` applies the same checks on config load. Between a POST and the next reboot, `appConfig` could hold an invalid pin (e.g. pin 300 → truncated to uint8 44 = GPIO44 / USB D-), causing `taskGPIO` to toggle the wrong hardware pin.

**Fix:** Inline bounds checks added matching `appConfig.cpp`'s logic: pins clamped to 0–48, `mqttPort` to 1–65535 (falls back to `MQTT_DEFAULT_PORT`), `powerLimitDefault` to 0–100, `ledBrightness` to 0–255. Values outside range are silently kept at their current value (no reject, no crash).

**P3-B — Incorrect `mqttTls` comment** — `include/appConfig.h`

Comment read `// connect via mqtts:// (TLS, no certificate validation)` — wrong when `/mqtt_ca.pem` is present on LittleFS, in which case mbedTLS verifies the broker certificate against that CA.

**Fix:** Comment updated to `// CA-verified if /mqtt_ca.pem present, unverified otherwise`.

**P3-C — Renewal hook heredoc: unquoted `${INSTALL_DIR}`** — `deploy/getting-started.sh`

The certbot renewal hook was written via a heredoc that expanded `${INSTALL_DIR}` without quoting. A path containing a space (non-default but valid) produced a broken generated script (`cd /opt/my mqtt` instead of `cd "/opt/my mqtt"`), causing silent renewal failures and expiry of the TLS certificate after 90 days.

**Fix:** All path expansions in the heredoc body are now double-quoted: `cp -L "..."`, `cd "${INSTALL_DIR}"`.

---

### Open items (Info, no action required)

| Area | Note |
|---|---|
| Project | `include/buildnumber.txt` intentionally tracked (release workflow) — appears as `modified` after every local build |
| MQTT | `mqttOpenDtu` mode does not publish GPIO/system in the OpenDTU schema — only relevant for OpenDTU-compatible HA GPIO control |
| FreeRTOS | Theoretical deadlock chain DataStore mutex + log mutex — currently unreachable; keep in mind: no logging from DataStore functions, no DataStore access from `logMsg()` |
| FreeRTOS | `waitFor()` uses 50 ms polling instead of a FreeRTOS notification — functionally correct, minor CPU waste |
| Error handling | `taskDTU` does not explicitly wait for `EVT_WIFI_CONNECTED` after WiFi loss — up to ~90 s recovery time after reconnect |
| Memory | `T(suffix)` in `taskMQTT.cpp`: temporary `String` allocation per `pub()` call, 20+ per poll cycle; PSRAM not used |

---

## Review 2026-06-17

**Based on:** Commit `07ffd0d` (fix: race condition FW+FS OTA), cross-checked against the synchronized specification v2 (`docs/HMS-GW-S3_Specification_v2.md`)
**Reviewer:** Claude Sonnet 4.6

---

## Preamble — Relationship to the Previous Review

The previous review (based on commit `4c4983d`/`4bc063a`, 2026-06-15/16) is now largely outdated: almost all P1/P2 findings flagged as open there have since been fixed. This review re-verifies the current code state line by line against the freshly synchronized specification.

---

## 0. Update 2026-06-17 (Production Incident) — Config Loss on Filesystem OTA

After this review, a real incident occurred on a production gateway: an Internet OTA (firmware + filesystem) caused the WiFi/DTU/MQTT/GPIO configuration to be completely reset to factory defaults after reboot. Clearly documented via serial log:

```
[INF] [OTA ] URL-OTA FS complete: 524288 B
... (Reboot) ...
[E] open(): /littlefs/config.json does not exist, no permits for creation
[E] open(): /littlefs/config.tmp does not exist, no permits for creation
[E][WiFiSTA.cpp:232] begin(): SSID too long or missing!
```

**Root cause:** `Update.begin(..., U_SPIFFS)` (in both `handleFsUpload()` and `doUrlOtaPartition()`) overwrites the entire LittleFS partition with the CI-built `littlefs.bin`, which only contains `data/www/*` — `/config.json` is deliberately gitignored and never part of the build image. Every filesystem OTA (whether local upload or Internet URL) therefore inevitably deleted the runtime-persisted config.

**Fix:** `backupConfigBeforeFsOta()`/`restoreConfigAfterFsOta()` in `taskWebServer.cpp` — backs up `/config.json` to RAM before the `Update.begin(..., U_SPIFFS)` call, remounts LittleFS after `Update.end()`, and writes the config back. Wired into both FS-OTA paths (upload + URL) and their error paths (best effort).

## 0.1 Update 2026-06-17 (Addendum) — Apparent "OTA Revert" Bug Was a Versioning Error in the Release Workflow

After the config fix, it was repeatedly observed that an Internet OTA (e.g. to "Build 211") — download/write/`Update.end()` all successful, including MD5 verification — reported an OLDER build number after reboot. Deep diagnosis (otadata status logging via the new `otainfo` console command, direct flash readout of the OTA partitions via `esptool read_flash` + MD5 comparison, full factory reflash of bootloader + partition table, timing variations before `ESP.restart()`) showed: **the OTA partition (`app1`) consistently contained exactly the correct, MD5-verified firmware image** — no write, bootloader, or partition issue.

**Actual cause:** `extra_scripts = pre:version_inc.py` (`platformio.ini`) increments `include/buildnumber.txt` on **every** `pio run` invocation, regardless of target. The release workflow called `pio run` (firmware) **and separately** `pio run -t buildfs` (filesystem) — incrementing the counter twice per release. `firmware.bin` for "Build N" had therefore actually compiled in `BUILD_NUMBER=N-1`, while the manifest and Git tag showed the value AFTER the second (filesystem) run (`N`). The firmware ran correctly the whole time — just with a version number one lower than, but internally consistent with, what the manifest claimed.

**Fix:** `.github/workflows/release.yml` now reads the build number immediately after the firmware build (before `buildfs`) and explicitly writes it back to `include/buildnumber.txt` after the filesystem build, so the committed value matches the `BUILD_NUMBER` actually compiled in.

**Lesson:** When facing a reproducible but implausible finding (write succeeded, content verified correct, yet "wrong" behavior), question the version bookkeeping itself before digging deeper into firmware/hardware layers — the bug here was never in the code that actually ran, but in its labeling.

This finding could/should have surfaced during the previous review round — the Internet OTA implementation was checked against the spec, but not against the effect of a `U_SPIFFS` update on runtime files already present on the same partition.

---

## 1. Specification Conformance

The specification was cross-checked against the code and updated before this review (DataStore API, EventGroup bits, web API endpoints, AppConfig fields, Internet OTA flow, logger colors, console commands, web dashboard tabs). There are therefore **no open discrepancies** between code and spec — with two exceptions explicitly marked in the spec itself as "planned, not yet implemented":

- **§6.4 Access Protection and Port:** Username/password protection (`webAuthEnabled`/`webUser`/`webPass`) and a configurable web port (`webPort`) are specified but not yet present in the code (`taskWebServer.cpp`, `appConfig.h`). `AsyncWebServer server` is still a global static object with a fixed `WEB_DEFAULT_PORT` (`taskWebServer.cpp:21`, `config.h:62`). Not a bug — simply not yet implemented.

All other sections (DataStore, EventGroup, DTU protocol, MQTT topics, web API, LED states, console, file structure) match the code 1:1.

---

## 2. Task Dependencies

The DataStore pattern is now fully adhered to. The previous direct `ds.mutex` access in `taskGPIO.cpp` and `taskDTU.cpp` was replaced with `dsGetGpioCommand()`/`dsGetDtuCommand()` (`dataStore.cpp:72-85`) — no task accesses `ds.*` directly, bypassing the API, anymore.

> **Update 2026-06-19 (fixed):** `mutex` is no longer part of the public `DataStore` struct — moved to a `static SemaphoreHandle_t _mutex` in `dataStore.cpp` (commit `8e843bf`). Verified that no code outside `dataStore.cpp` ever accessed `ds.mutex`; API signatures unchanged. The design guideline is now technically enforced rather than merely documented.

> **Update 2026-06-17 (after this review):** The two independent `LittleFS.remove(CONFIG_FILE)` paths (§3.2/§6, formerly P4) have been consolidated — `taskWebServer.cpp` no longer deletes the config itself, it only sets `EVT_FACTORY_RESET`/`EVT_REBOOT`. The sole owner is now `main.cpp`'s `loop()`.

`setLedState()` is still called from three tasks (`taskDTU`, `taskWebServer`, `taskGPIO`) and writes unprotected to `volatile bool`/`volatile LedState_t` (`taskNeoPixel.cpp:34-39`). On ESP32 these are atomic 1-/4-byte writes — non-critical, but without a formal memory-model guarantee (unchanged since the last review).

---

## 3. FreeRTOS Safety

### Correction Relative to the Previous Review

**The former P1 finding "`_rxBuf`/`_rxLen` race condition" is incorrect / already fixed.** Closer inspection (not just a grep on `_rxBuf`/`_rxLen`, but reading the complete function bodies) shows: a dedicated `_rxMutex` exists (`taskDTU.cpp:251`), correctly taken/given via `xSemaphoreTake`/`xSemaphoreGive` both in `onData()` (lines 257-260, AsyncTCP callback on Core 0) and at both read sites in `taskDTU` (lines 496-499, 513-516, Core 1). The mutex is created in `taskDTU()` (line 370) before `dtuConnect()` and thus before callback registration — no initialization race. The race condition described in the previous review does not exist in the current code. No action needed.

> **Update 2026-06-19 (hardening):** `onData()` used `xSemaphoreTake(_rxMutex, portMAX_DELAY)` — an unbounded block in the AsyncTCP callback (lwIP thread) if the mutex was currently held by `taskDTU`. Changed to a 10ms timeout (commit `8e843bf`); on failure, a `LOG_W` warning is emitted and the function returns without copying data or setting ready flags, instead of stalling the network thread.

### Fixed Since the Last Review

- **`vTaskDelay` + `ESP.restart()` in AsyncWebServer callbacks** — now deferred via `EVT_REBOOT`/`EVT_FACTORY_RESET` bits; the actual restart runs in `taskWebServer`'s task loop (`taskWebServer.cpp:574-580`), no longer in the lwIP/AsyncTCP thread.
- **`onConnect` callback with `dsGetSystem()` + `portMAX_DELAY`** — replaced with `_ntpTimeCache` (a lock-free `volatile uint32_t`, populated from task context before connecting). No more mutex access in the AsyncTCP callback (`taskDTU.cpp:252, 442`).
- **`_rxBuf`/`_rxLen` race condition** — see correction above, was not a current finding.

### WARNING

**3.2 Potential Deadlock Chain: DataStore Mutex + Log Mutex** — `logger.cpp`, `dataStore.cpp`

Unchanged, still only a theoretical risk: currently no DataStore function calls `logMsg()` and vice versa. Remains a risk for future extensions, not a current bug.

### INFO

**3.3 `waitFor()` uses 50ms polling** — `taskDTU.cpp` — unchanged, functionally correct, but unnecessary wakeups instead of a FreeRTOS notification/semaphore.

**3.4 `_dataFlash`/`_currentState` without a formal memory model** — `taskNeoPixel.cpp:34-39` — unchanged, non-critical in practice (see section 2).

---

## 4. Memory

### Fixed Since the Last Review

**`localBuf[2048]` is now `static`** instead of a stack variable (`taskDTU.cpp:384`) — no more stack pressure on the 8192-byte `taskDTU` stack.

### Unchanged (Info, No Action Needed)

- ArduinoJson `JsonDocument` in `taskWebServer.cpp`/`taskMQTT.cpp` handlers: heap allocation per request, short-lived, limited fragmentation.
- `String` fields in `DataStore::SystemStatus` (`wifiIp`, `wifiSsid`, `fwVersion`, `macAddress`): implicit copy on every `dsSetSystem()`.
- `T(suffix)` in `taskMQTT.cpp`: temporary `String` allocation per `pub()` call, 20+ per cycle for PV data.
- PSRAM (8 MB available) is not used explicitly (`ps_malloc`/`PSRAM_ATTR`); static arrays such as `_rxBuf[2048]` live in DRAM BSS.

---

## 5. Error Handling

### Fixed Since the Last Review

**OTA error handling** — `_otaFwError`/`_otaFsError` flags are present (`taskWebServer.cpp:279, 285, 289, 310, 316, 320`). After an `Update.begin()` failure, subsequent chunk callbacks are correctly ignored instead of blindly continuing to write.

**Configuration validation — fixed since this review:** `powerLimitDefault` is clamped to 0–100, `mqttPort` to 1–65535 (falls back to `MQTT_DEFAULT_PORT`), `ledBrightness` to 0–255, and all GPIO pin fields (`relay.pin`, `io[i].pin`, `ledPin`) via a shared `clampPin()` helper to the valid ESP32-S3 range 0–48 (falls back to the build default) — analogous to the already-existing `dtuInterval` clamp (`appConfig.cpp`). `IoMode` already had a bounds check (`appConfig.cpp`) and a `default` case in the `switch` (`taskGPIO.cpp`) beforehand.

### Unchanged

- **WiFi loss during DTU polling:** after failures, `taskDTU` does not explicitly wait for `EVT_WIFI_CONNECTED`, but retries connecting immediately → up to ~90s (3×30s cooldown) before recovery after a WiFi reconnect. Functionally correct, but inefficient.
- **Watchdog:** no explicit `esp_task_wdt_reset()`; all tasks are cooperative (`vTaskDelay`), so it does not trigger in practice. The 30s cooldown in `taskDTU` (`vTaskDelay(pdMS_TO_TICKS(30000))`) remains a theoretical risk if the task WDT were enabled for this task.

---

## 6. Other Findings

### Fixed Since the Last Review

- `DTU_CONNECT_TIMEOUT_MS` is now used (`taskDTU.cpp:444`), no more hardcoded `3000`.
- Duplicate config log at startup is gone — `main.cpp` no longer logs config fields separately, only `appConfig.cpp:177`.
- ANSI colors implemented in the logger (`logger.cpp:13-18`) — the spec was outdated, the code was already correct.
- Console commands `tasks`/`heap`/`uptime`/`ledtest` fully implemented (`taskSerial.cpp`).
- Internet OTA via URL fully implemented (was only described as planned in spec v2).
- **Browser cache:** `serveStatic` now sets `Cache-Control: no-cache` (`taskWebServer.cpp:550`) — filesystem OTA updates are visible without a hard refresh.
- **`AsyncClient::close(bool)` deprecation warning** fixed — all three call sites (`taskDTU.cpp:418, 463, 491`) now use `close()` without an argument.
- **Duplicate factory-reset `remove()`** fixed — see update note in section 2.

### Still Open

**6.1 `include/buildnumber.txt` not in `.gitignore`**

Deliberate design decision (see project memory): the file is intentionally tracked so the GitHub Actions release workflow can rely on a consistent build number. Therefore appears as `modified` in `git status` after every local build — no fix needed, noted for awareness only.

**6.2 `mqttOpenDtu` mode does not publish GPIO data** — `taskMQTT.cpp`

Unchanged: in OpenDTU-compatible mode, only PV data is published in the OpenDTU schema; GPIO/system topics still use the normal schema. Consistent, but not OpenDTU-compatible for GPIO control via HA.

---

## Prioritized Action Items

All P1–P4 findings from this review have been fixed (Cache-Control header, consolidated factory-reset `remove()`, bounds validation for config fields, `AsyncClient::close()` deprecation). Only informational notes without required action remain:

| Priority | Area | Note |
|------|---------|---------|
| Info | Project | `include/buildnumber.txt` intentionally tracked (release workflow) — appears as `modified` after every build |
| Info | MQTT | `mqttOpenDtu` mode does not publish GPIO/system in the OpenDTU schema — only relevant for OpenDTU-compatible HA GPIO control |
| Info | Build | `version_inc.py` increments on every build, even on failures — known SCons behavior |
| Info | FreeRTOS | Theoretical deadlock chain DataStore mutex + log mutex, currently unreachable — keep in mind for future extensions: no logging from DataStore functions, no DataStore access from `logMsg()` |
| Info | FreeRTOS | `waitFor()` uses 50ms polling instead of a notification/semaphore — functionally correct, minor CPU waste |
| Info | Error handling | `taskDTU` does not explicitly wait for `EVT_WIFI_CONNECTED` after WiFi loss — up to ~90s recovery time after reconnect |
