// taskWebServer.cpp — v2 (DataStore pattern)
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

static AsyncWebServer server(WEB_DEFAULT_PORT);
static DNSServer      dnsServer;
static bool           _dnsStarted = false;

// ─── GET /api/data.json  (Spec §6.2) ─────────────────────────────────────────
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
    doc["inverter"]["temp"]       = serialized(String(pv.temp, 1));
    doc["inverter"]["powerLimit"] = pv.powerLimit;
    doc["inverter"]["active"]     = pv.inverterActive;
    doc["timestamp"]              = pv.timestamp;
    doc["valid"]                  = pv.valid;
    String out; serializeJson(doc, out);
    req->send(200, "application/json", out);
}

// ─── GET /api/info.json  (Spec §6.3) ─────────────────────────────────────────
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

// ─── GET /api/gpio ────────────────────────────────────────────────────────────
static void handleApiGpioGet(AsyncWebServerRequest* req) {
    DataStore::GpioState gpio = dsGetGpio();
    JsonDocument doc;
    doc["relay"] = gpio.relay ? 1 : 0;
    for (int i = 0; i < 4; i++) {
        char key[8]; snprintf(key, sizeof(key), "gpio%d", i + 1);
        doc[key] = gpio.gpio[i] ? 1 : 0;
    }
    String out; serializeJson(doc, out);
    req->send(200, "application/json", out);
}

// ─── POST /api/gpio ───────────────────────────────────────────────────────────
static void handleApiGpioPost(AsyncWebServerRequest* req, uint8_t* data,
                               size_t len, size_t /*index*/, size_t /*total*/) {
    JsonDocument doc;
    if (deserializeJson(doc, (char*)data) != DeserializationError::Ok) {
        req->send(400, "application/json", "{\"error\":\"invalid json\"}"); return;
    }
    if (!doc["relay"].isNull()) dsSetGpioCommand(0, doc["relay"].as<int>() == 1);
    for (int i = 1; i <= 4; i++) {
        char key[8]; snprintf(key, sizeof(key), "gpio%d", i);
        if (!doc[key].isNull()) dsSetGpioCommand(i, doc[key].as<int>() == 1);
    }
    req->send(200, "application/json", "{\"ok\":true}");
}

// ─── GET /api/dtu ─────────────────────────────────────────────────────────────
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

// ─── POST /api/dtu  (DTU & system commands) ──────────────────────────────────
static void handleApiDtuPost(AsyncWebServerRequest* req, uint8_t* data,
                              size_t len, size_t /*index*/, size_t /*total*/) {
    JsonDocument doc;
    if (deserializeJson(doc, (char*)data) != DeserializationError::Ok) {
        req->send(400, "application/json", "{\"error\":\"invalid json\"}"); return;
    }
    if (doc["rebootGateway"].as<int>() == 1) {
        req->send(200, "application/json", "{\"ok\":true}");
        vTaskDelay(pdMS_TO_TICKS(500)); ESP.restart(); return;
    }
    if (doc["factoryReset"].as<int>() == 1) {
        req->send(200, "application/json", "{\"ok\":true}");
        xEventGroupSetBits(systemStateEvents, EVT_FACTORY_RESET);
        vTaskDelay(pdMS_TO_TICKS(3500)); ESP.restart(); return;
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

// ─── GET /api/config  (flat keys — matches dashboard JS + appConfig.cpp) ──────
static void handleApiConfigGet(AsyncWebServerRequest* req) {
    JsonDocument doc;
    doc["wifiSsid"]        = appConfig.wifiSsid;
    doc["wifiApFallback"]  = appConfig.wifiApFallback;
    // passwords never sent back
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
    const char* gpKeys[] = {"gp1","gp2","gp3","gp4"};
    for (int i = 0; i < 4; i++) {
        doc[gpKeys[i]]["pin"]      = appConfig.gp[i].pin;
        doc[gpKeys[i]]["mode"]     = (int)appConfig.gp[i].mode;
        doc[gpKeys[i]]["inverted"] = appConfig.gp[i].inverted;
        doc[gpKeys[i]]["pullup"]   = appConfig.gp[i].pullup;
    }
    doc["ledPin"]        = appConfig.ledPin;
    doc["ledBrightness"] = appConfig.ledBrightness;
    doc["tzOffset"]      = appConfig.tzOffset;
    doc["ntpServer"]     = appConfig.ntpServer;
    doc["logLevel"]      = appConfig.logLevel;
    String out; serializeJson(doc, out);
    req->send(200, "application/json", out);
}

// ─── POST /api/config ─────────────────────────────────────────────────────────
static void handleApiConfigPost(AsyncWebServerRequest* req, uint8_t* data,
                                 size_t len, size_t /*index*/, size_t /*total*/) {
    JsonDocument doc;
    if (deserializeJson(doc, (char*)data) != DeserializationError::Ok) {
        req->send(400, "application/json", "{\"error\":\"invalid json\"}"); return;
    }

    // Flat keys — same layout as appConfig.cpp configLoad/configSave
    if (doc["wifiSsid"].is<const char*>())
        strlcpy(appConfig.wifiSsid, doc["wifiSsid"].as<const char*>(), sizeof(appConfig.wifiSsid));
    if (doc["wifiPass"].is<const char*>())
        strlcpy(appConfig.wifiPass, doc["wifiPass"].as<const char*>(), sizeof(appConfig.wifiPass));
    if (!doc["wifiApFallback"].isNull()) appConfig.wifiApFallback = doc["wifiApFallback"].as<bool>();

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

    const char* gpKeys[] = {"gp1","gp2","gp3","gp4"};
    for (int i = 0; i < 4; i++) {
        if (!doc[gpKeys[i]]["pin"].isNull())      appConfig.gp[i].pin      = doc[gpKeys[i]]["pin"].as<int>();
        if (!doc[gpKeys[i]]["mode"].isNull())     appConfig.gp[i].mode     = (GpMode)doc[gpKeys[i]]["mode"].as<int>();
        if (!doc[gpKeys[i]]["inverted"].isNull()) appConfig.gp[i].inverted = doc[gpKeys[i]]["inverted"].as<bool>();
        if (!doc[gpKeys[i]]["pullup"].isNull())   appConfig.gp[i].pullup   = doc[gpKeys[i]]["pullup"].as<bool>();
    }

    if (!doc["ledPin"].isNull())        appConfig.ledPin        = doc["ledPin"].as<int>();
    if (!doc["ledBrightness"].isNull()) appConfig.ledBrightness = doc["ledBrightness"].as<int>();

    if (!doc["tzOffset"].isNull()) appConfig.tzOffset = doc["tzOffset"].as<int>();
    if (doc["ntpServer"].is<const char*>())
        strlcpy(appConfig.ntpServer, doc["ntpServer"].as<const char*>(), sizeof(appConfig.ntpServer));
    if (!doc["logLevel"].isNull()) appConfig.logLevel = doc["logLevel"].as<int>();

    configSave();
    LOG_I(MOD_WEB, "Config saved via API");
    req->send(200, "application/json", "{\"ok\":true,\"reboot\":true}");
}

// ─── OTA firmware upload ──────────────────────────────────────────────────────
static void handleOtaUpload(AsyncWebServerRequest* /*req*/, String filename,
                             size_t index, uint8_t* data, size_t len, bool final) {
    if (index == 0) {
        LOG_I(MOD_OTA, "OTA start: %s", filename.c_str());
        xEventGroupSetBits(systemStateEvents, EVT_OTA_RUNNING);
        setLedState(LED_OTA);
        if (!Update.begin(UPDATE_SIZE_UNKNOWN)) LOG_E(MOD_OTA, "Update.begin failed");
    }
    if (Update.write(data, len) != len) LOG_E(MOD_OTA, "Write error at %zu", index);
    if (final) {
        if (Update.end(true)) LOG_I(MOD_OTA, "OTA OK: %zu B", index + len);
        else                  LOG_E(MOD_OTA, "OTA error: %s", Update.errorString());
        xEventGroupClearBits(systemStateEvents, EVT_OTA_RUNNING);
    }
}

static void handleOtaDone(AsyncWebServerRequest* req) {
    if (Update.hasError()) req->send(500, "text/plain", String("OTA Error: ") + Update.errorString());
    else { req->send(200, "text/plain", "OTA OK — rebooting..."); vTaskDelay(pdMS_TO_TICKS(500)); ESP.restart(); }
}

// ─── OTA filesystem upload ────────────────────────────────────────────────────
static void handleFsUpload(AsyncWebServerRequest* /*req*/, String filename,
                            size_t index, uint8_t* data, size_t len, bool final) {
    if (index == 0) {
        LOG_I(MOD_OTA, "FS OTA start: %s", filename.c_str());
        xEventGroupSetBits(systemStateEvents, EVT_OTA_RUNNING);
        setLedState(LED_OTA);
        if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_SPIFFS)) LOG_E(MOD_OTA, "FS Update.begin failed");
    }
    if (Update.write(data, len) != len) LOG_E(MOD_OTA, "FS write error at %zu", index);
    if (final) {
        if (Update.end(true)) LOG_I(MOD_OTA, "FS OTA OK: %zu B", index + len);
        else                  LOG_E(MOD_OTA, "FS OTA error: %s", Update.errorString());
        xEventGroupClearBits(systemStateEvents, EVT_OTA_RUNNING);
    }
}

// ─── Captive portal redirect ──────────────────────────────────────────────────
static void handleCaptive(AsyncWebServerRequest* req) {
    req->redirect("http://" AP_IP "/");
}

// ─── Route registration ───────────────────────────────────────────────────────
static void setupRoutes() {
    // API routes MUST be registered before serveStatic — match order is FIFO
    server.on("/api/data.json", HTTP_GET, handleApiData);
    server.on("/api/info.json", HTTP_GET, handleApiInfo);
    server.on("/api/gpio",      HTTP_GET, handleApiGpioGet);
    server.on("/api/dtu",       HTTP_GET, handleApiDtuGet);
    server.on("/api/config",    HTTP_GET, handleApiConfigGet);

    server.on("/api/gpio", HTTP_POST, [](AsyncWebServerRequest* r){}, nullptr,
        [](AsyncWebServerRequest* r, uint8_t* d, size_t l, size_t i, size_t t){ handleApiGpioPost(r,d,l,i,t); });

    server.on("/api/dtu", HTTP_POST, [](AsyncWebServerRequest* r){}, nullptr,
        [](AsyncWebServerRequest* r, uint8_t* d, size_t l, size_t i, size_t t){ handleApiDtuPost(r,d,l,i,t); });

    server.on("/api/config", HTTP_POST, [](AsyncWebServerRequest* r){}, nullptr,
        [](AsyncWebServerRequest* r, uint8_t* d, size_t l, size_t i, size_t t){ handleApiConfigPost(r,d,l,i,t); });

    server.on("/update",   HTTP_POST, handleOtaDone, handleOtaUpload);
    server.on("/updatefs", HTTP_POST, handleOtaDone, handleFsUpload);

    // Static files — registered LAST so API routes take priority
    server.serveStatic("/", LittleFS, "/www/").setDefaultFile("index.html");

    // Captive portal well-known URIs
    const char* captiveUris[] = {"/generate_204", "/hotspot-detect.html",
                                  "/connecttest.txt", "/ncsi.txt", "/wpad.dat"};
    for (auto u : captiveUris) server.on(u, HTTP_GET, handleCaptive);

    server.onNotFound([](AsyncWebServerRequest* req) {
        if (xEventGroupGetBits(systemStateEvents) & EVT_WIFI_AP_MODE) handleCaptive(req);
        else req->redirect("/");
    });

    DefaultHeaders::Instance().addHeader("Access-Control-Allow-Origin", "*");
    server.begin();
    LOG_I(MOD_WEB, "Web server started on port %d", WEB_DEFAULT_PORT);
}

// ─── Task ─────────────────────────────────────────────────────────────────────
void taskWebServer(void* pvParameters) {
    LOG_I(MOD_WEB, "Task started (Core %d)", xPortGetCoreID());
    // LittleFS, config, and WiFi are already initialized in main.cpp
    setupRoutes();

    for (;;) {
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
