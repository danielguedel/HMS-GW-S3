#include "taskWebServer.h"
#include "config.h"
#include "appConfig.h"
#include "systemState.h"
#include "logger.h"
#include "taskNeoPixel.h"
#include "taskDTU.h"
#include "taskGPIO.h"
#include "taskMQTT.h"
#include <Arduino.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <AsyncTCP.h>

#include <LittleFS.h>
#include <ArduinoJson.h>
#include <Update.h>
#include <DNSServer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/event_groups.h>

static AsyncWebServer server(80);
static DNSServer      dnsServer;
static bool           _apMode = false;

// ─── Auth ─────────────────────────────────────────────────────────────────────
static bool checkAuth(AsyncWebServerRequest* req) {
    if (!appConfig.protectSettings) return true;
    if (!req->authenticate("admin", appConfig.wifiPassword)) {
        req->requestAuthentication(); return false;
    }
    return true;
}

// ─── GET /api/data.json ───────────────────────────────────────────────────────
static void handleApiData(AsyncWebServerRequest* req) {
    DtuData_t d = {};
    bool valid = false;
    if (xSemaphoreTake(configMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        memcpy(&d, &latestDtuData, sizeof(DtuData_t));
        valid = dtuDataValid;
        xSemaphoreGive(configMutex);
    }
    JsonDocument doc;
    doc["localtime"]     = (unsigned long)(millis() / 1000);
    doc["lastResponse"]  = d.lastResponse;
    doc["dtuConnState"]  = d.dtuConnState;
    doc["dtuErrorState"] = 0;

    doc["inverter"]["pLim"]    = d.powerLimit;
    doc["inverter"]["pLimSet"] = d.powerLimitSet;
    doc["inverter"]["temp"]    = serialized(String(d.temp, 1));
    doc["inverter"]["active"]  = d.inverterActive ? 1 : 0;
    doc["inverter"]["uptodate"]= valid ? 1 : 0;

    doc["grid"]["v"]  = serialized(String(d.grid_v,  1));
    doc["grid"]["i"]  = serialized(String(d.grid_i,  2));
    doc["grid"]["p"]  = serialized(String(d.grid_p,  1));
    doc["grid"]["dE"] = serialized(String(d.grid_dE, 3));
    doc["grid"]["tE"] = serialized(String(d.grid_tE, 3));
    doc["pv0"]["v"]   = serialized(String(d.pv0_v,  1));
    doc["pv0"]["i"]   = serialized(String(d.pv0_i,  2));
    doc["pv0"]["p"]   = serialized(String(d.pv0_p,  1));
    doc["pv0"]["dE"]  = serialized(String(d.pv0_dE, 3));
    doc["pv0"]["tE"]  = serialized(String(d.pv0_tE, 3));
    doc["pv1"]["v"]   = serialized(String(d.pv1_v,  1));
    doc["pv1"]["i"]   = serialized(String(d.pv1_i,  2));
    doc["pv1"]["p"]   = serialized(String(d.pv1_p,  1));
    doc["pv1"]["dE"]  = serialized(String(d.pv1_dE, 3));
    doc["pv1"]["tE"]  = serialized(String(d.pv1_tE, 3));

    String out; serializeJson(doc, out);
    req->send(200, "application/json", out);
}

// ─── GET /api/info.json ───────────────────────────────────────────────────────
static void handleApiInfo(AsyncWebServerRequest* req) {
    JsonDocument doc;
    doc["chipid"]   = (unsigned long)(ESP.getEfuseMac() & 0xFFFFFF);
    doc["chipType"] = "ESP32-S3";
    doc["host"]     = appConfig.hostname;
    doc["initMode"] = _apMode ? 1 : 0;
    doc["firmware"]["version"]     = FW_VERSION;
    doc["firmware"]["build"]       = BUILD_NUMBER;
    doc["firmware"]["versiondate"] = FW_DATE;
    doc["dtuConnection"]["dtuHostIpDomain"] = appConfig.dtuHost;
    doc["dtuConnection"]["dtuPort"]         = appConfig.dtuPort;
    doc["dtuConnection"]["dtuDataCycle"]    = appConfig.dtuInterval;
    doc["dtuConnection"]["dtuCloudPause"]   = appConfig.dtuCloudPause;
    if (xSemaphoreTake(configMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        doc["dtuConnection"]["deviceData"]["inverter_model"]          = latestDtuData.inverterModel;
        doc["dtuConnection"]["deviceData"]["inverter_serial"]         = latestDtuData.inverterSerial;
        doc["dtuConnection"]["deviceData"]["dtu_version_string"]      = latestDtuData.dtuVersionStr;
        doc["dtuConnection"]["deviceData"]["inverter_version_string"] = latestDtuData.inverterVersionStr;
        xSemaphoreGive(configMutex);
    }
    doc["wifiConnection"]["wifiSsid"] = WiFi.SSID();
    doc["wifiConnection"]["rssiGW"]   = (int)WiFi.RSSI();
    doc["wifiConnection"]["ip"]       = WiFi.localIP().toString();
    doc["system"]["uptime"]   = (unsigned long)(millis() / 1000);
    doc["system"]["freeHeap"] = (unsigned long)ESP.getFreeHeap();
    String out; serializeJson(doc, out);
    req->send(200, "application/json", out);
}

// ─── GET /api/gpio ────────────────────────────────────────────────────────────
static void handleApiGpioGet(AsyncWebServerRequest* req) {
    JsonDocument doc;
    doc["relay"]["state"] = gpioState.relay ? 1 : 0;
    doc["relay"]["mode"]  = "output";
    for (int i = 0; i < 4; i++) {
        char key[8]; snprintf(key, sizeof(key), "gpio%d", i+1);
        doc[key]["state"]  = gpioState.gpio[i] ? 1 : 0;
        doc[key]["mode"]   = appConfig.gp[i].isOutput ? "output" : "input";
        doc[key]["pullup"] = appConfig.gp[i].pullup;
    }
    String out; serializeJson(doc, out);
    req->send(200, "application/json", out);
}

// ─── POST /api/gpio ───────────────────────────────────────────────────────────
static void handleApiGpioPost(AsyncWebServerRequest* req, uint8_t* data,
                               size_t len, size_t index, size_t total) {
    JsonDocument doc;
    if (deserializeJson(doc, (char*)data) != DeserializationError::Ok) {
        req->send(400, "application/json", "{\"error\":\"invalid json\"}"); return;
    }
    if (!doc["relay"].isNull()) gpioSetRelay(doc["relay"].as<int>() == 1);
    for (int i = 1; i <= 4; i++) {
        char key[8]; snprintf(key, sizeof(key), "gpio%d", i);
        if (!doc[key].isNull()) gpioSetPin(i-1, doc[key].as<int>() == 1);
    }
    req->send(200, "application/json", "{\"ok\":true}");
}

// ─── POST /api/control ────────────────────────────────────────────────────────
static void handleApiControl(AsyncWebServerRequest* req, uint8_t* data,
                              size_t len, size_t index, size_t total) {
    if (!checkAuth(req)) return;
    JsonDocument doc;
    if (deserializeJson(doc, (char*)data) != DeserializationError::Ok) {
        req->send(400, "application/json", "{\"error\":\"invalid json\"}"); return;
    }
    if (!doc["powerLimit"].isNull())   dtuSetPowerLimit(doc["powerLimit"].as<int>());
    if (!doc["inverterOn"].isNull())   dtuSetInverterOn(doc["inverterOn"].as<bool>());
    if (!doc["rebootDTU"].isNull())    dtuRequestReboot();
    if (!doc["rebootGateway"].isNull()) {
        req->send(200, "application/json", "{\"ok\":true}");
        vTaskDelay(pdMS_TO_TICKS(500)); ESP.restart(); return;
    }
    req->send(200, "application/json", "{\"ok\":true}");
}

// ─── POST /api/config ─────────────────────────────────────────────────────────
static void handleApiConfigPost(AsyncWebServerRequest* req, uint8_t* data,
                                 size_t len, size_t index, size_t total) {
    if (!checkAuth(req)) return;
    JsonDocument doc;
    if (deserializeJson(doc, (char*)data) != DeserializationError::Ok) {
        req->send(400, "application/json", "{\"error\":\"invalid json\"}"); return;
    }
    if (xSemaphoreTake(configMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
        if (!doc["wifi"]["ssid"].isNull())
            strlcpy(appConfig.wifiSsid, doc["wifi"]["ssid"].as<const char*>(), sizeof(appConfig.wifiSsid));
        if (!doc["wifi"]["password"].isNull())
            strlcpy(appConfig.wifiPassword, doc["wifi"]["password"].as<const char*>(), sizeof(appConfig.wifiPassword));
        if (!doc["wifi"]["hostname"].isNull())
            strlcpy(appConfig.hostname, doc["wifi"]["hostname"].as<const char*>(), sizeof(appConfig.hostname));
        if (!doc["dtu"]["host"].isNull())
            strlcpy(appConfig.dtuHost, doc["dtu"]["host"].as<const char*>(), sizeof(appConfig.dtuHost));
        if (!doc["dtu"]["port"].isNull())     appConfig.dtuPort       = doc["dtu"]["port"].as<int>();
        if (!doc["dtu"]["interval"].isNull()) {
            int iv = doc["dtu"]["interval"].as<int>();
            appConfig.dtuInterval = (iv < DTU_MIN_INTERVAL) ? DTU_MIN_INTERVAL : iv;
        }
        if (!doc["dtu"]["cloudPause"].isNull()) appConfig.dtuCloudPause = doc["dtu"]["cloudPause"].as<int>();
        if (!doc["mqtt"]["host"].isNull())
            strlcpy(appConfig.mqttHost, doc["mqtt"]["host"].as<const char*>(), sizeof(appConfig.mqttHost));
        if (!doc["mqtt"]["port"].isNull())    appConfig.mqttPort        = doc["mqtt"]["port"].as<int>();
        if (!doc["mqtt"]["user"].isNull())
            strlcpy(appConfig.mqttUser, doc["mqtt"]["user"].as<const char*>(), sizeof(appConfig.mqttUser));
        if (!doc["mqtt"]["pass"].isNull())
            strlcpy(appConfig.mqttPass, doc["mqtt"]["pass"].as<const char*>(), sizeof(appConfig.mqttPass));
        if (!doc["mqtt"]["topic"].isNull())
            strlcpy(appConfig.mqttTopic, doc["mqtt"]["topic"].as<const char*>(), sizeof(appConfig.mqttTopic));
        if (!doc["mqtt"]["tls"].isNull())     appConfig.mqttTls         = doc["mqtt"]["tls"].as<bool>();
        if (!doc["mqtt"]["ha"].isNull())      appConfig.mqttHaDiscovery = doc["mqtt"]["ha"].as<bool>();
        if (!doc["mqtt"]["openDTU"].isNull()) appConfig.mqttOpenDtu     = doc["mqtt"]["openDTU"].as<bool>();
        if (!doc["system"]["tz"].isNull())            appConfig.tzOffset      = doc["system"]["tz"].as<int>();
        if (!doc["system"]["ledBrightness"].isNull()) appConfig.ledBrightness = doc["system"]["ledBrightness"].as<int>();
        if (!doc["system"]["logLevel"].isNull())      appConfig.logLevel      = doc["system"]["logLevel"].as<int>();
        if (!doc["system"]["protect"].isNull())       appConfig.protectSettings = doc["system"]["protect"].as<bool>();
        xSemaphoreGive(configMutex);
    }
    configSave();
    LOG_I(MOD_WEB, "Config saved via API");
    req->send(200, "application/json", "{\"ok\":true,\"reboot\":true}");
}

// ─── GET /api/config ──────────────────────────────────────────────────────────
static void handleApiConfigGet(AsyncWebServerRequest* req) {
    if (!checkAuth(req)) return;
    JsonDocument doc;
    doc["wifi"]["ssid"]     = appConfig.wifiSsid;
    doc["wifi"]["password"] = "***";
    doc["wifi"]["hostname"] = appConfig.hostname;
    doc["dtu"]["host"]             = appConfig.dtuHost;
    doc["dtu"]["port"]             = appConfig.dtuPort;
    doc["dtu"]["interval"]         = appConfig.dtuInterval;
    doc["dtu"]["cloudPause"]       = appConfig.dtuCloudPause;
    doc["dtu"]["rebootAfterFails"] = appConfig.dtuRebootAfterFails;
    doc["mqtt"]["host"]    = appConfig.mqttHost;
    doc["mqtt"]["port"]    = appConfig.mqttPort;
    doc["mqtt"]["user"]    = appConfig.mqttUser;
    doc["mqtt"]["pass"]    = "***";
    doc["mqtt"]["topic"]   = appConfig.mqttTopic;
    doc["mqtt"]["tls"]     = appConfig.mqttTls;
    doc["mqtt"]["ha"]      = appConfig.mqttHaDiscovery;
    doc["mqtt"]["openDTU"] = appConfig.mqttOpenDtu;
    doc["mqtt"]["qos"]     = appConfig.mqttQos;
    doc["mqtt"]["retain"]  = appConfig.mqttRetain;
    doc["gpio"]["relay"]["output"]   = appConfig.relay.isOutput;
    doc["gpio"]["relay"]["inverted"] = appConfig.relay.inverted;
    const char* gpKeys[] = {"gp1","gp2","gp3","gp4"};
    for (int i = 0; i < 4; i++) {
        doc["gpio"][gpKeys[i]]["output"]   = appConfig.gp[i].isOutput;
        doc["gpio"][gpKeys[i]]["pullup"]   = appConfig.gp[i].pullup;
        doc["gpio"][gpKeys[i]]["inverted"] = appConfig.gp[i].inverted;
    }
    doc["system"]["tz"]            = appConfig.tzOffset;
    doc["system"]["ledBrightness"] = appConfig.ledBrightness;
    doc["system"]["logLevel"]      = appConfig.logLevel;
    doc["system"]["protect"]       = appConfig.protectSettings;
    doc["system"]["webPort"]       = appConfig.webPort;
    doc["system"]["apSsid"]        = appConfig.apSsid;
    String out; serializeJson(doc, out);
    req->send(200, "application/json", out);
}

// ─── OTA upload ───────────────────────────────────────────────────────────────
static void handleOtaUpload(AsyncWebServerRequest* req, String filename,
                             size_t index, uint8_t* data, size_t len, bool final) {
    if (!checkAuth(req)) return;
    if (index == 0) {
        LOG_I(MOD_OTA, "OTA start: %s", filename.c_str());
        xEventGroupSetBits(systemStateEvents, EVT_OTA_RUNNING);
        setLedState(LED_OTA);
        xSemaphoreTake(otaSemaphore, portMAX_DELAY);
        if (!Update.begin(UPDATE_SIZE_UNKNOWN)) LOG_E(MOD_OTA, "Update.begin failed");
    }
    if (Update.write(data, len) != len) LOG_E(MOD_OTA, "Write error at %zu", index);
    if (final) {
        if (Update.end(true)) LOG_I(MOD_OTA, "OTA OK: %zu B", index + len);
        else                  LOG_E(MOD_OTA, "OTA error: %s", Update.errorString());
        xEventGroupClearBits(systemStateEvents, EVT_OTA_RUNNING);
        xSemaphoreGive(otaSemaphore);
    }
}

static void handleOtaDone(AsyncWebServerRequest* req) {
    if (Update.hasError()) req->send(500, "text/plain", String("OTA Error: ") + Update.errorString());
    else { req->send(200, "text/plain", "OTA OK. Rebooting..."); vTaskDelay(pdMS_TO_TICKS(500)); ESP.restart(); }
}

// ─── Captive portal ───────────────────────────────────────────────────────────
static void handleCaptive(AsyncWebServerRequest* req) { req->redirect("http://" AP_IP "/"); }

// ─── WiFi ─────────────────────────────────────────────────────────────────────
static void startWifi() {
    WiFi.setHostname(appConfig.hostname);
    if (strlen(appConfig.wifiSsid) == 0) {
        _apMode = true;
        char apSsid[40];
        snprintf(apSsid, sizeof(apSsid), "%s_%06llX", appConfig.apSsid,
                 (unsigned long long)(ESP.getEfuseMac() & 0xFFFFFF));
        WiFi.mode(WIFI_AP);
        WiFi.softAP(apSsid);
        WiFi.softAPConfig(IPAddress(192,168,4,1), IPAddress(192,168,4,1), IPAddress(255,255,255,0));
        xEventGroupSetBits(systemStateEvents, EVT_WIFI_AP_MODE);
        setLedState(LED_AP_MODE);
        dnsServer.start(53, "*", IPAddress(192,168,4,1));
        LOG_I(MOD_WIFI, "AP mode: %s  IP=192.168.4.1", apSsid);
        return;
    }
    WiFi.mode(WIFI_STA);
    WiFi.begin(appConfig.wifiSsid, appConfig.wifiPassword);
    setLedState(LED_WIFI_CONNECTING);
    LOG_I(MOD_WIFI, "Connecting to %s...", appConfig.wifiSsid);
    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && (millis() - t0) < 20000)
        vTaskDelay(pdMS_TO_TICKS(500));

    if (WiFi.status() == WL_CONNECTED) {
        WiFi.setSleep(false);  // disable power saving — prevents watchdog starvation
        xEventGroupSetBits(systemStateEvents, EVT_WIFI_CONNECTED);
        LOG_I(MOD_WIFI, "Connected! IP=%s  RSSI=%d dBm",
              WiFi.localIP().toString().c_str(), (int)WiFi.RSSI());
        WiFi.onEvent([](WiFiEvent_t ev, WiFiEventInfo_t info) {
            if (ev == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
                xEventGroupClearBits(systemStateEvents, EVT_WIFI_CONNECTED | EVT_DTU_ONLINE);
                LOG_W(MOD_WIFI, "Disconnected. Reconnecting...");
                WiFi.reconnect();
            } else if (ev == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
                xEventGroupSetBits(systemStateEvents, EVT_WIFI_CONNECTED);
                LOG_I(MOD_WIFI, "Reconnected. IP=%s", WiFi.localIP().toString().c_str());
            }
        });
    } else {
        LOG_W(MOD_WIFI, "Connection failed — starting AP fallback");
        _apMode = true;
        char apSsid[40];
        snprintf(apSsid, sizeof(apSsid), "%s_%06llX", appConfig.apSsid,
                 (unsigned long long)(ESP.getEfuseMac() & 0xFFFFFF));
        WiFi.mode(WIFI_AP);
        WiFi.softAP(apSsid);
        xEventGroupSetBits(systemStateEvents, EVT_WIFI_AP_MODE);
        setLedState(LED_AP_MODE);
        dnsServer.start(53, "*", IPAddress(192,168,4,1));
    }
}

// ─── Routes ───────────────────────────────────────────────────────────────────
static void setupRoutes() {
    // API routes MUST be registered before serveStatic —
    // ESPAsyncWebServer matches in registration order and serveStatic
    // would otherwise intercept /api/* requests first.
    server.on("/api/data.json", HTTP_GET,  handleApiData);
    server.on("/api/info.json", HTTP_GET,  handleApiInfo);
    server.on("/api/gpio",      HTTP_GET,  handleApiGpioGet);
    server.on("/api/config",    HTTP_GET,  handleApiConfigGet);

    server.on("/api/gpio", HTTP_POST,
        [](AsyncWebServerRequest* r){},
        nullptr,
        [](AsyncWebServerRequest* r, uint8_t* d, size_t l, size_t i, size_t t){ handleApiGpioPost(r,d,l,i,t); });

    server.on("/api/control", HTTP_POST,
        [](AsyncWebServerRequest* r){},
        nullptr,
        [](AsyncWebServerRequest* r, uint8_t* d, size_t l, size_t i, size_t t){ handleApiControl(r,d,l,i,t); });

    server.on("/api/config", HTTP_POST,
        [](AsyncWebServerRequest* r){},
        nullptr,
        [](AsyncWebServerRequest* r, uint8_t* d, size_t l, size_t i, size_t t){ handleApiConfigPost(r,d,l,i,t); });

    server.on("/update", HTTP_POST, handleOtaDone, handleOtaUpload);

    // Static files — registered LAST so API routes take priority
    server.serveStatic("/", LittleFS, "/www/").setDefaultFile("index.html");

    // Captive portal
    const char* captive[] = {"/generate_204","/hotspot-detect.html",
                              "/connecttest.txt","/ncsi.txt","/wpad.dat"};
    for (auto u : captive) server.on(u, HTTP_GET, handleCaptive);

    server.onNotFound([](AsyncWebServerRequest* req){
        if (_apMode) handleCaptive(req); else req->redirect("/");
    });

    DefaultHeaders::Instance().addHeader("Access-Control-Allow-Origin", "*");
    server.begin();
    LOG_I(MOD_WEB, "Web server started on port %d", appConfig.webPort);
}

// ─── Task ─────────────────────────────────────────────────────────────────────
void taskWebServer(void* pvParameters) {
    if (!LittleFS.begin(true)) {
        LOG_E(MOD_WEB, "LittleFS mount failed!");
        xEventGroupSetBits(systemStateEvents, EVT_ERROR);
        vTaskDelete(nullptr); return;
    }
    LOG_I(MOD_WEB, "LittleFS mounted. Used: %lu / %lu B",
          (unsigned long)LittleFS.usedBytes(), (unsigned long)LittleFS.totalBytes());
    configSetDefaults();
    configLoad();
    logInit();
    LOG_I(MOD_SYS, "HMS-GW-S3 v%s (Build %d, %s)", FW_VERSION, BUILD_NUMBER, FW_DATE);
    LOG_I(MOD_SYS, "Heap: %lu B  MAC: %llX",
          (unsigned long)ESP.getFreeHeap(), (unsigned long long)ESP.getEfuseMac());
    startWifi();
    setupRoutes();
    for (;;) {
        if (_apMode) dnsServer.processNextRequest();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
