// taskWebServer.cpp  -  v2 (DataStore pattern)
// LittleFS, config, WiFi are initialized in main.cpp before task start.

#include "taskWebServer.h"
#include "dataStore.h"
#include "appConfig.h"
#include "systemState.h"
#include "taskLED.h"
#include "config.h"
#include "logger.h"
#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include <AsyncTCP.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <Update.h>
#include <DNSServer.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <esp_ota_ops.h>

// Allocated in taskWebServer() after configLoad() so the configured webPort
// can be used  -  a global static AsyncWebServer would have to be constructed
// with WEB_DEFAULT_PORT before config is even loaded.
static AsyncWebServer*              server = nullptr;
static AsyncAuthenticationMiddleware authMiddleware;
static DNSServer      dnsServer;
static bool           _dnsStarted  = false;
static bool           _otaFwError  = false;
static bool           _otaFsError  = false;
static char           _otaUrl[512]   = "";
static char           _otaFsUrl[512] = "";
static char           _otaMd5[33]    = "";
static char           _otaFsMd5[33]  = "";
static volatile bool  _otaUrlPending   = false;
static volatile bool  _otaFsUrlPending = false;
static volatile bool  _otaCheckPending = false;
static bool           _bootCheckDone   = false;
static String         _fsOtaConfigBackup;  // /config.json content, preserved across a U_SPIFFS OTA write

// A Filesystem-OTA (Update.begin(..., U_SPIFFS)) overwrites the entire LittleFS
// partition with the CI-built image, which only contains data/www/* -  the
// runtime-persisted /config.json lives on the same partition and would
// otherwise be wiped, resetting WiFi/DTU/MQTT/GPIO config to factory defaults.
static void backupConfigBeforeFsOta() {
    File f = LittleFS.open(CONFIG_FILE, "r");
    if (!f) { _fsOtaConfigBackup = ""; return; }
    _fsOtaConfigBackup = f.readString();
    f.close();
    LOG_I(MOD_OTA, "Backed up %s (%u B) before filesystem OTA", CONFIG_FILE, (unsigned)_fsOtaConfigBackup.length());
}

// Diagnostic for the OTA-revert bug: logs the otadata state of both OTA slots
// right after a firmware write claims success, so we can see whether the
// bootloader's boot-partition pointer was actually persisted before the
// reboot happens (vs. silently staying on the currently-running slot).
static void logOtaPartitionState(const char* when) {
    static const char* stateName[] = {"NEW", "PENDING_VERIFY", "VALID", "INVALID", "ABORTED"};
    const esp_partition_t* running = esp_ota_get_running_partition();
    const esp_partition_t* next    = esp_ota_get_next_update_partition(NULL);
    esp_ota_img_states_t runSt = ESP_OTA_IMG_UNDEFINED, nextSt = ESP_OTA_IMG_UNDEFINED;
    if (running) esp_ota_get_state_partition(running, &runSt);
    if (next)    esp_ota_get_state_partition(next, &nextSt);
    auto name = [](esp_ota_img_states_t s) {
        return (s <= ESP_OTA_IMG_ABORTED) ? stateName[s] : "UNDEFINED";
    };
    LOG_I(MOD_OTA, "otadata %s: running=%s(%s)  next=%s(%s)", when,
          running ? running->label : "?", name(runSt),
          next    ? next->label    : "?", name(nextSt));
}

// Writes back the backup taken by backupConfigBeforeFsOta(); a no-op if no backup was captured (e.g. first-ever boot). Requires an explicit LittleFS unmount/remount since the just-written CI filesystem image isn't visible to the already-mounted instance.
static void restoreConfigAfterFsOta() {
    if (_fsOtaConfigBackup.length() == 0) return;
    LittleFS.end();
    if (!LittleFS.begin()) {
        LOG_E(MOD_OTA, "LittleFS remount failed after filesystem OTA  -  cannot restore %s", CONFIG_FILE);
        return;
    }
    File f = LittleFS.open(CONFIG_FILE, "w");
    if (!f) { LOG_E(MOD_OTA, "Cannot reopen %s to restore config after filesystem OTA", CONFIG_FILE); return; }
    f.print(_fsOtaConfigBackup);
    f.close();
    _fsOtaConfigBackup = "";
    LOG_I(MOD_OTA, "Restored %s after filesystem OTA", CONFIG_FILE);
}

// --- GET /api/data.json  (Spec §6.2) -----------------------------------------
static void handleApiData(AsyncWebServerRequest* req) {
    DataStore::PvData pv = dsGetPv();
    JsonDocument doc;
    doc["pv0"]["v"]  = serialized(String(pv.pv0_v,  1));
    doc["pv0"]["i"]  = serialized(String(pv.pv0_i,  2));
    doc["pv0"]["p"]  = serialized(String(pv.pv0_p,  1));
    doc["pv0"]["dE"] = serialized(String(pv.pv0_dE, 3));
    doc["pv0"]["tE"] = serialized(String(pv.pv0_tE, 3));
    doc["pv1"]["v"]  = serialized(String(pv.pv1_v,  1));
    doc["pv1"]["i"]  = serialized(String(pv.pv1_i,  2));
    doc["pv1"]["p"]  = serialized(String(pv.pv1_p,  1));
    doc["pv1"]["dE"] = serialized(String(pv.pv1_dE, 3));
    doc["pv1"]["tE"] = serialized(String(pv.pv1_tE, 3));
    doc["grid"]["v"]  = serialized(String(pv.grid_v,  1));
    doc["grid"]["i"]  = serialized(String(pv.grid_i,  2));
    doc["grid"]["p"]  = serialized(String(pv.grid_p,  1));
    doc["grid"]["dE"] = serialized(String(pv.grid_dE, 3));
    doc["grid"]["tE"] = serialized(String(pv.grid_tE, 3));
    doc["inverter"]["temp"]         = serialized(String(pv.temp, 1));
    doc["inverter"]["powerLimit"]    = pv.powerLimit;
    doc["inverter"]["powerLimitSet"] = pv.powerLimitSet;
    doc["inverter"]["active"]       = pv.inverterActive;
    doc["timestamp"]              = pv.timestamp;
    doc["valid"]                  = pv.valid;
    String out; serializeJson(doc, out);
    req->send(200, "application/json", out);
}

// --- GET /api/info.json  (Spec §6.3) -----------------------------------------
static void handleApiInfo(AsyncWebServerRequest* req) {
    DataStore::SystemStatus sys = dsGetSystem();
    JsonDocument doc;
    doc["fw"]      = sys.fwVersion;
    doc["build"]   = sys.buildNumber;
    doc["uptime"]  = sys.uptimeS;
    doc["heap"]    = sys.freeHeap;
    doc["mac"]     = sys.macAddress;
    doc["ip"]      = sys.wifiIp;
    doc["rssi"]    = sys.wifiRssi;
    doc["wifi"]    = sys.wifiConnected;
    doc["dtu"]     = sys.dtuOnline;
    doc["mqtt"]    = sys.mqttConnected;
    doc["ntpTime"] = sys.ntpTime;
    String out; serializeJson(doc, out);
    req->send(200, "application/json", out);
}

// --- GET /api/gpio ------------------------------------------------------------
static void handleApiGpioGet(AsyncWebServerRequest* req) {
    DataStore::GpioState gpio = dsGetGpio();
    JsonDocument doc;
    doc["relay"] = gpio.relay ? 1 : 0;
    for (int i = 0; i < 3; i++) {
        char key[8]; snprintf(key, sizeof(key), "io%d", i + 1);
        doc[key] = gpio.gpio[i] ? 1 : 0;
    }
    String out; serializeJson(doc, out);
    req->send(200, "application/json", out);
}

// --- POST /api/gpio -----------------------------------------------------------
static void handleApiGpioPost(AsyncWebServerRequest* req, uint8_t* data,
                               size_t len, size_t /*index*/, size_t /*total*/) {
    JsonDocument doc;
    if (deserializeJson(doc, (char*)data) != DeserializationError::Ok) {
        req->send(400, "application/json", "{\"error\":\"invalid json\"}"); return;
    }
    if (!doc["relay"].isNull()) dsSetGpioCommand(0, doc["relay"].as<int>() == 1);
    for (int i = 1; i <= 3; i++) {
        char key[8]; snprintf(key, sizeof(key), "io%d", i);
        if (!doc[key].isNull()) dsSetGpioCommand(i, doc[key].as<int>() == 1);
    }
    req->send(200, "application/json", "{\"ok\":true}");
}

// --- GET /api/dtu -------------------------------------------------------------
static void handleApiDtuGet(AsyncWebServerRequest* req) {
    DataStore::SystemStatus sys = dsGetSystem();
    DataStore::PvData       pv  = dsGetPv();
    JsonDocument doc;
    doc["online"]        = sys.dtuOnline;
    doc["cloudBusy"]     = sys.dtuCloudBusy;
    doc["failCount"]     = sys.dtuFailCount;
    doc["powerLimit"]    = pv.powerLimit;
    doc["powerLimitSet"] = pv.powerLimitSet;
    doc["dtuRssi"]       = pv.wifiRssi;
    String out; serializeJson(doc, out);
    req->send(200, "application/json", out);
}

// --- POST /api/dtu  (DTU & system commands) ----------------------------------
// Single endpoint for several distinct actions, selected by which JSON key is present: rebootGateway/factoryReset act immediately via EventGroup bits; powerLimit/inverterOn/rebootDtu/rebootInverter are queued as a DtuCommand for taskDTU to pick up.
static void handleApiDtuPost(AsyncWebServerRequest* req, uint8_t* data,
                              size_t len, size_t /*index*/, size_t /*total*/) {
    JsonDocument doc;
    if (deserializeJson(doc, (char*)data) != DeserializationError::Ok) {
        req->send(400, "application/json", "{\"error\":\"invalid json\"}"); return;
    }
    if (doc["rebootGateway"].as<int>() == 1) {
        req->send(200, "application/json", "{\"ok\":true}");
        xEventGroupSetBits(systemStateEvents, EVT_REBOOT);
        return;
    }
    if (doc["factoryReset"].as<int>() == 1) {
        req->send(200, "application/json", "{\"ok\":true}");
        xEventGroupSetBits(systemStateEvents, EVT_FACTORY_RESET | EVT_REBOOT);
        return;
    }
    DataStore::DtuCommand cmd = {};
    if (!doc["powerLimit"].isNull()) {
        cmd.setPowerLimit   = true;
        cmd.powerLimitValue = doc["powerLimit"].as<int>();
    }
    if (!doc["inverterOn"].isNull()) {
        cmd.setInverterOn   = true;
        cmd.inverterOnValue = doc["inverterOn"].as<bool>();
    }
    if (doc["rebootDtu"].as<int>()      == 1) cmd.rebootDtu      = true;
    if (doc["rebootInverter"].as<int>() == 1) cmd.rebootInverter = true;
    dsSetDtuCommand(cmd);
    req->send(200, "application/json", "{\"ok\":true}");
}

// --- GET /api/config  (flat keys  -  matches dashboard JS + appConfig.cpp) ------
static void handleApiConfigGet(AsyncWebServerRequest* req) {
    JsonDocument doc;
    doc["wifiSsid"]        = appConfig.wifiSsid;
    doc["wifiApFallback"]  = appConfig.wifiApFallback;
    // passwords never sent back
    doc["useStaticIp"] = appConfig.useStaticIp;
    doc["staticIp"]    = appConfig.staticIp;
    doc["subnet"]      = appConfig.subnet;
    doc["gateway"]     = appConfig.gateway;
    doc["dtuHost"]             = appConfig.dtuHost;
    doc["dtuPort"]             = appConfig.dtuPort;
    doc["dtuInterval"]         = appConfig.dtuInterval;
    doc["dtuCloudPause"]       = appConfig.dtuCloudPause;
    doc["dtuRebootAfterFails"] = appConfig.dtuRebootAfterFails;
    doc["powerLimitDefault"]   = appConfig.powerLimitDefault;
    doc["powerLimitTimeout"]   = appConfig.powerLimitTimeout;
    doc["mqttHost"]        = appConfig.mqttHost;
    doc["mqttPort"]        = appConfig.mqttPort;
    doc["mqttUser"]        = appConfig.mqttUser;
    doc["mqttTopic"]       = appConfig.mqttTopic;
    doc["mqttRetain"]      = appConfig.mqttRetain;
    doc["mqttHaDiscovery"] = appConfig.mqttHaDiscovery;
    doc["mqttOpenDtu"]     = appConfig.mqttOpenDtu;
    doc["relayPin"]        = appConfig.relay.pin;
    doc["relayInverted"]   = appConfig.relay.inverted;
    const char* ioKeys[] = {"io1","io2","io3"};
    for (int i = 0; i < 3; i++) {
        doc[ioKeys[i]]["pin"]         = appConfig.io[i].pin;
        doc[ioKeys[i]]["mode"]        = (int)appConfig.io[i].mode;
        doc[ioKeys[i]]["altFunction"] = appConfig.io[i].altFunction;
        doc[ioKeys[i]]["inverted"]    = appConfig.io[i].inverted;
        doc[ioKeys[i]]["pullup"]      = appConfig.io[i].pullup;
    }
    doc["ledPin"]        = appConfig.ledPin;
    doc["ledBrightness"] = appConfig.ledBrightness;
    doc["tzOffset"]        = appConfig.tzOffset;
    doc["ntpServer"]       = appConfig.ntpServer;
    doc["logLevel"]        = appConfig.logLevel;
    doc["otaManifestUrl"]  = appConfig.otaManifestUrl;
    doc["webAuthEnabled"] = appConfig.webAuthEnabled;
    doc["webUser"]        = appConfig.webUser;
    // webPass never sent back
    doc["webPort"]        = appConfig.webPort;
    String out; serializeJson(doc, out);
    req->send(200, "application/json", out);
}

// --- POST /api/config ---------------------------------------------------------
// Partial-update semantics: only JSON keys present in the body are applied to appConfig (missing keys keep their current value), then configSave() persists it. Always responds with reboot:true, but does not itself set EVT_REBOOT — the web GUI triggers the actual restart with a separate POST /api/dtu {rebootGateway:1} call.
static void handleApiConfigPost(AsyncWebServerRequest* req, uint8_t* data,
                                 size_t len, size_t index, size_t total) {
    // Accumulate body chunks  -  ESPAsyncWebServer calls this once per TCP segment
    static uint8_t bodyBuf[2048];
    if (index + len > sizeof(bodyBuf)) {
        if (index + len >= total)
            req->send(413, "application/json", "{\"error\":\"body too large\"}");
        return;
    }
    memcpy(bodyBuf + index, data, len);
    if (index + len < total) return;  // wait for remaining chunks
    size_t bodyLen = index + len;

    JsonDocument doc;
    if (deserializeJson(doc, bodyBuf, bodyLen) != DeserializationError::Ok) {
        req->send(400, "application/json", "{\"error\":\"invalid json\"}"); return;
    }

    // Flat keys  -  same layout as appConfig.cpp configLoad/configSave
    if (doc["wifiSsid"].is<const char*>())
        strlcpy(appConfig.wifiSsid, doc["wifiSsid"].as<const char*>(), sizeof(appConfig.wifiSsid));
    if (doc["wifiPass"].is<const char*>())
        strlcpy(appConfig.wifiPass, doc["wifiPass"].as<const char*>(), sizeof(appConfig.wifiPass));
    if (!doc["wifiApFallback"].isNull()) appConfig.wifiApFallback = doc["wifiApFallback"].as<bool>();

    if (!doc["useStaticIp"].isNull()) {
        const char* ip = doc["staticIp"].is<const char*>() ? doc["staticIp"].as<const char*>() : "";
        const char* sn = doc["subnet"].is<const char*>()   ? doc["subnet"].as<const char*>()   : "";
        const char* gw = doc["gateway"].is<const char*>()  ? doc["gateway"].as<const char*>()  : "";
        IPAddress tmp;
        bool valid = *ip && tmp.fromString(ip) && *sn && tmp.fromString(sn) && *gw && tmp.fromString(gw);
        appConfig.useStaticIp = doc["useStaticIp"].as<bool>() && valid;
        if (valid) {
            strlcpy(appConfig.staticIp, ip, sizeof(appConfig.staticIp));
            strlcpy(appConfig.subnet,   sn, sizeof(appConfig.subnet));
            strlcpy(appConfig.gateway,  gw, sizeof(appConfig.gateway));
        } else if (doc["useStaticIp"].as<bool>()) {
            req->send(400, "application/json", "{\"error\":\"invalid static IP/subnet/gateway\"}");
            return;
        }
    }

    if (doc["dtuHost"].is<const char*>())
        strlcpy(appConfig.dtuHost, doc["dtuHost"].as<const char*>(), sizeof(appConfig.dtuHost));
    if (!doc["dtuPort"].isNull())             appConfig.dtuPort             = doc["dtuPort"].as<int>();
    if (!doc["dtuInterval"].isNull()) {
        int iv = doc["dtuInterval"].as<int>();
        appConfig.dtuInterval = (iv < DTU_MIN_INTERVAL) ? DTU_MIN_INTERVAL : iv;
    }
    if (!doc["dtuCloudPause"].isNull())       appConfig.dtuCloudPause       = doc["dtuCloudPause"].as<int>();
    if (!doc["dtuRebootAfterFails"].isNull()) appConfig.dtuRebootAfterFails = doc["dtuRebootAfterFails"].as<int>();

    if (!doc["powerLimitDefault"].isNull()) appConfig.powerLimitDefault = doc["powerLimitDefault"].as<int>();
    if (!doc["powerLimitTimeout"].isNull()) appConfig.powerLimitTimeout = doc["powerLimitTimeout"].as<int>();

    if (doc["mqttHost"].is<const char*>())
        strlcpy(appConfig.mqttHost,  doc["mqttHost"].as<const char*>(),  sizeof(appConfig.mqttHost));
    if (!doc["mqttPort"].isNull())  appConfig.mqttPort = doc["mqttPort"].as<int>();
    if (doc["mqttUser"].is<const char*>())
        strlcpy(appConfig.mqttUser,  doc["mqttUser"].as<const char*>(),  sizeof(appConfig.mqttUser));
    if (doc["mqttPass"].is<const char*>())
        strlcpy(appConfig.mqttPass,  doc["mqttPass"].as<const char*>(),  sizeof(appConfig.mqttPass));
    if (doc["mqttTopic"].is<const char*>())
        strlcpy(appConfig.mqttTopic, doc["mqttTopic"].as<const char*>(), sizeof(appConfig.mqttTopic));
    if (!doc["mqttRetain"].isNull())      appConfig.mqttRetain      = doc["mqttRetain"].as<bool>();
    if (!doc["mqttHaDiscovery"].isNull()) appConfig.mqttHaDiscovery = doc["mqttHaDiscovery"].as<bool>();
    if (!doc["mqttOpenDtu"].isNull())     appConfig.mqttOpenDtu     = doc["mqttOpenDtu"].as<bool>();

    if (!doc["relayPin"].isNull())      appConfig.relay.pin      = doc["relayPin"].as<int>();
    if (!doc["relayInverted"].isNull()) appConfig.relay.inverted = doc["relayInverted"].as<bool>();

    const char* ioKeys[] = {"io1","io2","io3"};
    for (int i = 0; i < 3; i++) {
        if (!doc[ioKeys[i]]["pin"].isNull())      appConfig.io[i].pin      = doc[ioKeys[i]]["pin"].as<int>();
        if (!doc[ioKeys[i]]["mode"].isNull())     appConfig.io[i].mode     = (IoMode)doc[ioKeys[i]]["mode"].as<int>();
        if (doc[ioKeys[i]]["altFunction"].is<const char*>())
            strlcpy(appConfig.io[i].altFunction, doc[ioKeys[i]]["altFunction"].as<const char*>(), sizeof(appConfig.io[i].altFunction));
        if (!doc[ioKeys[i]]["inverted"].isNull()) appConfig.io[i].inverted = doc[ioKeys[i]]["inverted"].as<bool>();
        if (!doc[ioKeys[i]]["pullup"].isNull())   appConfig.io[i].pullup   = doc[ioKeys[i]]["pullup"].as<bool>();
    }

    if (!doc["ledPin"].isNull())        appConfig.ledPin        = doc["ledPin"].as<int>();
    if (!doc["ledBrightness"].isNull()) appConfig.ledBrightness = doc["ledBrightness"].as<int>();

    if (!doc["tzOffset"].isNull()) appConfig.tzOffset = doc["tzOffset"].as<int>();
    if (doc["ntpServer"].is<const char*>())
        strlcpy(appConfig.ntpServer, doc["ntpServer"].as<const char*>(), sizeof(appConfig.ntpServer));
    if (!doc["logLevel"].isNull()) appConfig.logLevel = doc["logLevel"].as<int>();
    if (doc["otaManifestUrl"].is<const char*>())
        strlcpy(appConfig.otaManifestUrl, doc["otaManifestUrl"].as<const char*>(), sizeof(appConfig.otaManifestUrl));

    if (!doc["webAuthEnabled"].isNull()) appConfig.webAuthEnabled = doc["webAuthEnabled"].as<bool>();
    if (doc["webUser"].is<const char*>())
        strlcpy(appConfig.webUser, doc["webUser"].as<const char*>(), sizeof(appConfig.webUser));
    if (doc["webPass"].is<const char*>())
        strlcpy(appConfig.webPass, doc["webPass"].as<const char*>(), sizeof(appConfig.webPass));
    if (!doc["webPort"].isNull()) {
        int wp = doc["webPort"].as<int>();
        if (wp >= 1 && wp <= 65535) appConfig.webPort = (uint16_t)wp;
    }
    if (appConfig.webAuthEnabled && strlen(appConfig.webPass) == 0) {
        req->send(400, "application/json", "{\"error\":\"auth enabled but no password set\"}");
        return;
    }

    configSave();
    LOG_I(MOD_WEB, "Config saved via API");
    req->send(200, "application/json", "{\"ok\":true,\"reboot\":true}");
}

// --- GET /api/config/backup  -  full config.json download (incl. passwords) --
static void handleApiConfigBackup(AsyncWebServerRequest* req) {
    if (!LittleFS.exists(CONFIG_FILE)) {
        req->send(404, "application/json", "{\"error\":\"no config saved yet\"}");
        return;
    }
    req->send(LittleFS, CONFIG_FILE, "application/json", true);
}

// --- POST /api/config/restore  -  upload a previously downloaded backup -----
static bool _restoreError = false;
// ESPAsyncWebServer multipart-upload callback, invoked repeatedly as the file streams in (index/len describe the current chunk, final marks the last one); accumulates into bodyBuf and only parses once final is true. The actual HTTP response is sent separately by handleApiConfigRestoreDone() after this returns.
static void handleApiConfigRestore(AsyncWebServerRequest* /*req*/, String /*filename*/,
                                    size_t index, uint8_t* data, size_t len, bool final) {
    static uint8_t bodyBuf[4096];
    if (index == 0) _restoreError = false;
    if (_restoreError) return;
    if (index + len > sizeof(bodyBuf)) {
        LOG_E(MOD_WEB, "Config restore: file too large");
        _restoreError = true;
        return;
    }
    memcpy(bodyBuf + index, data, len);
    if (!final) return;
    if (!configRestoreFromJson((const char*)bodyBuf, index + len)) _restoreError = true;
}

// Runs after the upload in handleApiConfigRestore() completes; reports the error flag it set and triggers a reboot on success so the restored config takes effect.
static void handleApiConfigRestoreDone(AsyncWebServerRequest* req) {
    if (_restoreError) {
        req->send(400, "application/json", "{\"error\":\"invalid config backup\"}");
        return;
    }
    req->send(200, "application/json", "{\"ok\":true,\"reboot\":true}");
    xEventGroupSetBits(systemStateEvents, EVT_REBOOT);
}

// --- OTA firmware upload ------------------------------------------------------
// ESPAsyncWebServer chunked-upload callback (one call per received chunk, index==0 starts a new Update, final flushes it); the HTTP response is sent afterwards by handleOtaDone().
static void handleOtaUpload(AsyncWebServerRequest* /*req*/, String filename,
                             size_t index, uint8_t* data, size_t len, bool final) {
    if (index == 0) {
        _otaFwError = false;
        LOG_I(MOD_OTA, "OTA start: %s", filename.c_str());
        xEventGroupSetBits(systemStateEvents, EVT_OTA_RUNNING);
        setLedState(LED_OTA);
        if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
            LOG_E(MOD_OTA, "Update.begin failed");
            _otaFwError = true;
            return;
        }
    }
    if (_otaFwError) return;
    if (Update.write(data, len) != len) LOG_E(MOD_OTA, "Write error at %zu", index);
    if (final) {
        if (Update.end(true)) {
            LOG_I(MOD_OTA, "OTA OK: %zu B", index + len);
            logOtaPartitionState("after FW write");
        } else {
            LOG_E(MOD_OTA, "OTA error: %s", Update.errorString());
        }
        xEventGroupClearBits(systemStateEvents, EVT_OTA_RUNNING);
    }
}

// Shared completion callback for both /update and /updatefs; reflects whatever error state Update.hasError() left behind from the preceding upload handler.
static void handleOtaDone(AsyncWebServerRequest* req) {
    if (Update.hasError()) req->send(500, "text/plain", String("OTA Error: ") + Update.errorString());
    else {
        req->send(200, "text/plain", "OTA OK  -  rebooting...");
        xEventGroupSetBits(systemStateEvents, EVT_REBOOT);
    }
}

// --- OTA filesystem upload ----------------------------------------------------
// Same chunked-upload pattern as handleOtaUpload(), but for U_SPIFFS; wraps the write in backupConfigBeforeFsOta()/restoreConfigAfterFsOta() since this overwrites the whole LittleFS partition including config.json.
static void handleFsUpload(AsyncWebServerRequest* /*req*/, String filename,
                            size_t index, uint8_t* data, size_t len, bool final) {
    if (index == 0) {
        _otaFsError = false;
        LOG_I(MOD_OTA, "FS OTA start: %s", filename.c_str());
        xEventGroupSetBits(systemStateEvents, EVT_OTA_RUNNING);
        setLedState(LED_OTA);
        backupConfigBeforeFsOta();
        if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_SPIFFS)) {
            LOG_E(MOD_OTA, "FS Update.begin failed");
            _otaFsError = true;
            return;
        }
    }
    if (_otaFsError) return;
    if (Update.write(data, len) != len) LOG_E(MOD_OTA, "FS write error at %zu", index);
    if (final) {
        if (Update.end(true)) {
            LOG_I(MOD_OTA, "FS OTA OK: %zu B", index + len);
            restoreConfigAfterFsOta();
        } else {
            LOG_E(MOD_OTA, "FS OTA error: %s", Update.errorString());
            restoreConfigAfterFsOta();  // partition left in an unknown state -  best effort
        }
        xEventGroupClearBits(systemStateEvents, EVT_OTA_RUNNING);
    }
}

// --- Internet OTA: Manifest-Check -------------------------------------------
static void doOtaCheck() {
    if (!strlen(appConfig.otaManifestUrl)) return;

    DataStore::OtaInfo info = {};
    info.checking = true;
    dsSetOtaInfo(info);
    LOG_I(MOD_OTA, "OTA check: %s", appConfig.otaManifestUrl);

    HTTPClient http;
    http.setTimeout(10000);
    if (!http.begin(appConfig.otaManifestUrl)) {
        LOG_E(MOD_OTA, "OTA check: http.begin failed");
        info.checking = false; dsSetOtaInfo(info); return;
    }
    int code = http.GET();
    if (code != HTTP_CODE_OK) {
        LOG_E(MOD_OTA, "OTA check: HTTP %d", code);
        http.end(); info.checking = false; dsSetOtaInfo(info); return;
    }
    String body = http.getString();
    http.end();

    JsonDocument doc;
    if (deserializeJson(doc, body) != DeserializationError::Ok) {
        LOG_E(MOD_OTA, "OTA check: JSON parse error");
        info.checking = false; dsSetOtaInfo(info); return;
    }

    info.checking     = false;
    info.lastCheckMs  = millis();
    info.buildNumber  = doc["buildNumber"].as<int>();
    info.available    = (info.buildNumber > BUILD_NUMBER);
    const char* ver   = doc["version"].as<const char*>();
    const char* url   = doc["url"].as<const char*>();
    const char* fsUrl = doc["fs_url"].as<const char*>();
    const char* md5   = doc["md5"].as<const char*>();
    const char* fsMd5 = doc["fs_md5"].as<const char*>();
    const char* notes = doc["notes"].as<const char*>();
    strlcpy(info.version, ver   ? ver   : "", sizeof(info.version));
    strlcpy(info.url,     url   ? url   : "", sizeof(info.url));
    strlcpy(info.fsUrl,   fsUrl ? fsUrl : "", sizeof(info.fsUrl));
    // Only accept well-formed 32-char hex MD5 values  -  anything else means "no check"
    strlcpy(info.md5,   (md5   && strlen(md5)   == 32) ? md5   : "", sizeof(info.md5));
    strlcpy(info.fsMd5, (fsMd5 && strlen(fsMd5) == 32) ? fsMd5 : "", sizeof(info.fsMd5));
    strlcpy(info.notes,   notes ? notes : "", sizeof(info.notes));
    dsSetOtaInfo(info);

    if (info.available)
        LOG_I(MOD_OTA, "Update available: v%s (build %d), current build %d",
              info.version, info.buildNumber, BUILD_NUMBER);
    else
        LOG_I(MOD_OTA, "Firmware up to date (build %d)", BUILD_NUMBER);
}

// --- GET /api/ota/check -------------------------------------------------------
static void handleApiOtaCheckGet(AsyncWebServerRequest* req) {
    DataStore::OtaInfo info = dsGetOtaInfo();
    JsonDocument doc;
    doc["checking"]       = info.checking;
    doc["available"]      = info.available;
    doc["version"]        = info.version;
    doc["buildNumber"]    = info.buildNumber;
    doc["url"]            = info.url;
    doc["fsUrl"]          = info.fsUrl;
    doc["md5"]            = info.md5;
    doc["fsMd5"]          = info.fsMd5;
    doc["notes"]          = info.notes;
    doc["lastCheckMs"]    = info.lastCheckMs;
    doc["currentVersion"] = FW_VERSION;
    doc["currentBuild"]   = BUILD_NUMBER;
    doc["manifestUrl"]    = appConfig.otaManifestUrl;
    String out; serializeJson(doc, out);
    req->send(200, "application/json", out);
}

// --- POST /api/ota/check (trigger manual check) ------------------------------
static void handleApiOtaCheckPost(AsyncWebServerRequest* req) {
    if (!strlen(appConfig.otaManifestUrl)) {
        req->send(400, "application/json", "{\"error\":\"no manifest url configured\"}");
        return;
    }
    _otaCheckPending = true;
    req->send(200, "application/json", "{\"ok\":true}");
}

// --- POST /api/ota/url  (Internet OTA  -  Spec §10.2) ------------------------
// Only validates and queues the URL(s)/MD5(s) for taskWebServer()'s loop to act on via doUrlOtaPartition() — the actual download/flash never happens inside this AsyncTCP callback.
static void handleApiOtaUrl(AsyncWebServerRequest* req, uint8_t* data,
                             size_t len, size_t index, size_t total) {
    static uint8_t bodyBuf[768];
    if (index + len > sizeof(bodyBuf)) {
        if (index + len >= total)
            req->send(413, "application/json", "{\"error\":\"url too long\"}");
        return;
    }
    memcpy(bodyBuf + index, data, len);
    if (index + len < total) return;

    JsonDocument doc;
    if (deserializeJson(doc, bodyBuf, index + len) != DeserializationError::Ok) {
        req->send(400, "application/json", "{\"error\":\"invalid json\"}"); return;
    }
    const char* url   = doc["url"].as<const char*>();
    const char* fsUrl = doc["fsUrl"].as<const char*>();
    const char* md5   = doc["md5"].as<const char*>();
    const char* fsMd5 = doc["fsMd5"].as<const char*>();
    if ((!url || strlen(url) == 0) && (!fsUrl || strlen(fsUrl) == 0)) {
        req->send(400, "application/json", "{\"error\":\"url missing\"}"); return;
    }
    // Set both flags before any LOG_I to avoid race with task loop on Core 1
    // (LOG_I blocks ~8ms at 115200 baud, enough for Core 1 to see a partial state)
    _otaMd5[0] = _otaFsMd5[0] = '\0';
    if (md5   && strlen(md5)   == 32) strlcpy(_otaMd5,   md5,   sizeof(_otaMd5));
    if (fsMd5 && strlen(fsMd5) == 32) strlcpy(_otaFsMd5, fsMd5, sizeof(_otaFsMd5));
    if (url && strlen(url)) {
        strlcpy(_otaUrl, url, sizeof(_otaUrl));
        _otaUrlPending = true;
    }
    if (fsUrl && strlen(fsUrl)) {
        strlcpy(_otaFsUrl, fsUrl, sizeof(_otaFsUrl));
        _otaFsUrlPending = true;
    }
    if (_otaUrlPending)   LOG_I(MOD_OTA, "FW-URL queued: %s%s", _otaUrl, _otaMd5[0] ? " (MD5 checked)" : "");
    if (_otaFsUrlPending) LOG_I(MOD_OTA, "FS-URL queued: %s%s", _otaFsUrl, _otaFsMd5[0] ? " (MD5 checked)" : "");
    req->send(200, "application/json", "{\"ok\":true}");
}

// Download a binary from URL and flash to given partition (U_FLASH or U_SPIFFS).
// expectedMd5 (32 hex chars, optional) is verified by Update.end() -  a corrupted
// download that still matches the expected byte count would otherwise "succeed"
// at the app level while producing an unbootable image (silently rejected by the
// bootloader's own checksum check, with no error visible to us).
// Returns true on success. Does not trigger reboot — caller is responsible.
static bool doUrlOtaPartition(const char* url, int partition, const char* expectedMd5 = nullptr) {
    const char* tag = (partition == U_SPIFFS) ? "FS" : "FW";
    LOG_I(MOD_OTA, "URL-OTA %s: %s", tag, url);

    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.setTimeout(30000);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    if (!http.begin(client, url)) {
        LOG_E(MOD_OTA, "URL-OTA %s http.begin failed", tag);
        return false;
    }
    int code = http.GET();
    if (code != HTTP_CODE_OK) {
        LOG_E(MOD_OTA, "URL-OTA %s HTTP %d", tag, code);
        http.end();
        return false;
    }

    int contentLen = http.getSize();
    LOG_I(MOD_OTA, "URL-OTA %s size: %d B", tag, contentLen);

    if (partition == U_SPIFFS) backupConfigBeforeFsOta();

    if (!Update.begin(contentLen > 0 ? (size_t)contentLen : UPDATE_SIZE_UNKNOWN, partition)) {
        LOG_E(MOD_OTA, "URL-OTA %s Update.begin failed: %s", tag, Update.errorString());
        http.end();
        return false;
    }

    if (expectedMd5 && strlen(expectedMd5) == 32) {
        Update.setMD5(expectedMd5);
        LOG_I(MOD_OTA, "URL-OTA %s MD5 check enabled: %s", tag, expectedMd5);
    }

    WiFiClient* stream = http.getStreamPtr();
    static uint8_t dlBuf[512];
    size_t written  = 0;
    int    lastPct  = -1;
    bool   writeErr = false;

    while (http.connected() || stream->available()) {
        size_t avail = stream->available();
        if (avail) {
            size_t toRead = min(avail, sizeof(dlBuf));
            int n = stream->readBytes(dlBuf, toRead);
            if (n > 0) {
                if (Update.write(dlBuf, (size_t)n) != (size_t)n) {
                    LOG_E(MOD_OTA, "URL-OTA %s write error at %zu B", tag, written);
                    writeErr = true; break;
                }
                written += (size_t)n;
                if (contentLen > 0) {
                    int pct = (int)((written * 100) / (size_t)contentLen);
                    if (pct / 10 != lastPct / 10) {
                        LOG_I(MOD_OTA, "URL-OTA %s: %d%% (%zu / %d B)", tag, pct, written, contentLen);
                        lastPct = pct;
                    }
                }
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(1));
        }
        if (contentLen > 0 && written >= (size_t)contentLen) break;
    }

    http.end();
    if (writeErr) {
        Update.abort();
        if (partition == U_SPIFFS) restoreConfigAfterFsOta();  // partition left in an unknown state -  best effort
        return false;
    }

    if (Update.end(true)) {
        LOG_I(MOD_OTA, "URL-OTA %s complete: %zu B", tag, written);
        if (partition == U_SPIFFS) restoreConfigAfterFsOta();
        else                       logOtaPartitionState("after FW write");
        return true;
    }
    LOG_E(MOD_OTA, "URL-OTA %s end failed: %s", tag, Update.errorString());
    if (partition == U_SPIFFS) restoreConfigAfterFsOta();
    return false;
}

// --- Captive portal redirect --------------------------------------------------
static void handleCaptive(AsyncWebServerRequest* req) {
    req->redirect("http://" AP_IP "/");
}

// --- Route registration -------------------------------------------------------
static void setupRoutes() {
    server = new AsyncWebServer(appConfig.webPort);

    if (appConfig.webAuthEnabled && strlen(appConfig.webPass)) {
        authMiddleware.setUsername(appConfig.webUser);
        authMiddleware.setPassword(appConfig.webPass);
        authMiddleware.setAuthType(AsyncAuthType::AUTH_BASIC);
        authMiddleware.setRealm("HMS-GW-S3");
        // Global middleware  -  covers every route incl. /api/*, /update, static
        // files and the captive portal, not just whichever handlers we remember
        // to guard individually.
        server->addMiddleware(&authMiddleware);
    }

    // API routes MUST be registered before serveStatic  -  match order is FIFO.
    // ESPAsyncWebServer's plain-string URI matching is "backward compatible":
    // a route "/api/config" also matches any "/api/config/..." sub-path (prefix
    // match with trailing slash), so more specific sub-paths like
    // /api/config/backup MUST be registered before the shorter /api/config,
    // or the broader handler intercepts them first.
    server->on("/api/config/backup",  HTTP_GET,  handleApiConfigBackup);
    server->on("/api/config/restore", HTTP_POST, handleApiConfigRestoreDone, handleApiConfigRestore);

    server->on("/api/data.json", HTTP_GET, handleApiData);
    server->on("/api/info.json", HTTP_GET, handleApiInfo);
    server->on("/api/gpio",      HTTP_GET, handleApiGpioGet);
    server->on("/api/dtu",       HTTP_GET, handleApiDtuGet);
    server->on("/api/config",    HTTP_GET, handleApiConfigGet);

    server->on("/api/gpio", HTTP_POST, [](AsyncWebServerRequest* r){}, nullptr,
        [](AsyncWebServerRequest* r, uint8_t* d, size_t l, size_t i, size_t t){ handleApiGpioPost(r,d,l,i,t); });

    server->on("/api/dtu", HTTP_POST, [](AsyncWebServerRequest* r){}, nullptr,
        [](AsyncWebServerRequest* r, uint8_t* d, size_t l, size_t i, size_t t){ handleApiDtuPost(r,d,l,i,t); });

    server->on("/api/config", HTTP_POST, [](AsyncWebServerRequest* r){}, nullptr,
        [](AsyncWebServerRequest* r, uint8_t* d, size_t l, size_t i, size_t t){ handleApiConfigPost(r,d,l,i,t); });

    server->on("/update",   HTTP_POST, handleOtaDone, handleOtaUpload);
    server->on("/updatefs", HTTP_POST, handleOtaDone, handleFsUpload);

    server->on("/api/ota/url", HTTP_POST, [](AsyncWebServerRequest* r){}, nullptr,
        [](AsyncWebServerRequest* r, uint8_t* d, size_t l, size_t i, size_t t){ handleApiOtaUrl(r,d,l,i,t); });

    server->on("/api/ota/check", HTTP_GET,  handleApiOtaCheckGet);
    server->on("/api/ota/check", HTTP_POST, handleApiOtaCheckPost);

    // Static files  -  registered LAST so API routes take priority
    // no-cache: forces revalidation so a Filesystem-OTA update is visible without
    // a hard browser refresh (Ctrl+Shift+R), while still allowing conditional caching
    server->serveStatic("/", LittleFS, "/www/").setDefaultFile("index.html").setCacheControl("no-cache");

    // Captive portal well-known URIs
    const char* captiveUris[] = {"/generate_204", "/hotspot-detect.html",
                                  "/connecttest.txt", "/ncsi.txt", "/wpad.dat"};
    for (auto u : captiveUris) server->on(u, HTTP_GET, handleCaptive);

    server->onNotFound([](AsyncWebServerRequest* req) {
        if (xEventGroupGetBits(systemStateEvents) & EVT_WIFI_AP_MODE) handleCaptive(req);
        else req->redirect("/");
    });

    DefaultHeaders::Instance().addHeader("Access-Control-Allow-Origin", "*");
    server->begin();
    LOG_I(MOD_WEB, "Web server started on port %d%s", appConfig.webPort,
          appConfig.webAuthEnabled ? " (auth enabled)" : "");
}

// --- Task ---------------------------------------------------------------------
// FreeRTOS task entry point (pvParameters unused); starts the web server once, then loops handling deferred reboot/OTA-check/URL-OTA work and the captive-portal DNS responder; never returns.
void taskWebServer(void* pvParameters) {
    LOG_I(MOD_WEB, "Task started (Core %d)", xPortGetCoreID());
    // LittleFS, config, and WiFi are already initialized in main.cpp
    logOtaPartitionState("at boot");
    setupRoutes();

    for (;;) {
        // Deferred reboot/factory-reset (set by API callbacks to avoid vTaskDelay on lwIP thread)
        EventBits_t evBits = xEventGroupGetBits(systemStateEvents);
        if (evBits & EVT_REBOOT) {
            // Config removal is owned solely by main.cpp's loop() (watches EVT_FACTORY_RESET);
            // the longer delay here just gives it time to run before the restart.
            uint32_t delayMs = (evBits & EVT_FACTORY_RESET) ? 3500 : 500;
            vTaskDelay(pdMS_TO_TICKS(delayMs));
            ESP.restart();
        }

        // Boot-time manifest check (once, after WiFi is connected)
        if (!_bootCheckDone && strlen(appConfig.otaManifestUrl) &&
            (xEventGroupGetBits(systemStateEvents) & EVT_WIFI_CONNECTED)) {
            _bootCheckDone = true;
            doOtaCheck();
        }

        // Manual manifest check triggered via POST /api/ota/check
        if (_otaCheckPending) {
            _otaCheckPending = false;
            doOtaCheck();
        }

        // URL-OTA: download and flash in task context (not in lwIP callback)
        if (_otaUrlPending || _otaFsUrlPending) {
            bool fwPending = _otaUrlPending;
            bool fsPending = _otaFsUrlPending;
            _otaUrlPending   = false;
            _otaFsUrlPending = false;
            setLedState(LED_OTA);
            xEventGroupSetBits(systemStateEvents, EVT_OTA_RUNNING);
            bool ok = true;
            if (fwPending) ok = doUrlOtaPartition(_otaUrl,   U_FLASH,  _otaMd5);
            if (ok && fsPending) ok = doUrlOtaPartition(_otaFsUrl, U_SPIFFS, _otaFsMd5);
            xEventGroupClearBits(systemStateEvents, EVT_OTA_RUNNING);
            if (ok) xEventGroupSetBits(systemStateEvents, EVT_REBOOT);
        }

        // Start DNS captive portal on first AP mode detection
        if (!_dnsStarted && (xEventGroupGetBits(systemStateEvents) & EVT_WIFI_AP_MODE)) {
            dnsServer.start(53, "*", IPAddress(192, 168, 4, 1));
            _dnsStarted = true;
            LOG_I(MOD_WEB, "DNS captive portal started");
        }
        if (_dnsStarted) dnsServer.processNextRequest();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
