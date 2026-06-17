// appConfig.cpp  -  v2 (flat JSON format, v2 AppConfig fields)
// config.json uses the same flat keys as the web API for consistency.

#include "appConfig.h"
#include "config.h"
#include "logger.h"
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <Arduino.h>

AppConfig appConfig;

// ESP32-S3 exposes GPIO0–GPIO48  -  reject anything outside that range
// (catches e.g. an unset/garbage "255" pin from a malformed config) and fall
// back to the build default instead.
static uint8_t clampPin(int v, uint8_t def) {
    return (v >= 0 && v <= 48) ? (uint8_t)v : def;
}

void configSetDefaults() {
    memset(&appConfig, 0, sizeof(AppConfig));

    // WiFi
    strlcpy(appConfig.wifiSsid,  "", sizeof(appConfig.wifiSsid));
    strlcpy(appConfig.wifiPass,  "", sizeof(appConfig.wifiPass));
    appConfig.wifiApFallback = true;

    // DTU
    strlcpy(appConfig.dtuHost, "192.168.1.100", sizeof(appConfig.dtuHost));
    appConfig.dtuPort             = DTU_DEFAULT_PORT;
    appConfig.dtuInterval         = DTU_DEFAULT_INTERVAL;
    appConfig.dtuCloudPause       = DTU_DEFAULT_CLOUD_PAUSE;
    appConfig.dtuRebootAfterFails = DTU_REBOOT_AFTER_FAILS;

    // Power Limit
    appConfig.powerLimitDefault = POWER_LIMIT_DEFAULT;
    appConfig.powerLimitTimeout = POWER_LIMIT_TIMEOUT;

    // MQTT
    strlcpy(appConfig.mqttHost,  "",                 sizeof(appConfig.mqttHost));
    appConfig.mqttPort = MQTT_DEFAULT_PORT;
    strlcpy(appConfig.mqttUser,  "",                 sizeof(appConfig.mqttUser));
    strlcpy(appConfig.mqttPass,  "",                 sizeof(appConfig.mqttPass));
    snprintf(appConfig.mqttTopic, sizeof(appConfig.mqttTopic),
             "hmsgws3_%06llX", (unsigned long long)(ESP.getEfuseMac() & 0xFFFFFF));
    appConfig.mqttRetain      = false;
    appConfig.mqttHaDiscovery = true;
    appConfig.mqttOpenDtu     = false;

    // GPIO  -  Relay
    appConfig.relay.pin      = RELAY_PIN;
    appConfig.relay.inverted = false;

    // GPIO  -  IO1–IO3 (generisch, Default-Modus immer OUTPUT)
    appConfig.io[0].pin      = IO1_PIN;
    appConfig.io[0].mode     = IO_OUTPUT;
    strlcpy(appConfig.io[0].altFunction, "I2C_SDA", sizeof(appConfig.io[0].altFunction));
    appConfig.io[0].inverted = false;
    appConfig.io[0].pullup   = false;

    appConfig.io[1].pin      = IO2_PIN;
    appConfig.io[1].mode     = IO_OUTPUT;
    strlcpy(appConfig.io[1].altFunction, "I2C_SCL", sizeof(appConfig.io[1].altFunction));
    appConfig.io[1].inverted = false;
    appConfig.io[1].pullup   = false;

    appConfig.io[2].pin      = IO3_PIN;
    appConfig.io[2].mode     = IO_OUTPUT;
    strlcpy(appConfig.io[2].altFunction, "ADC1_CH3", sizeof(appConfig.io[2].altFunction));
    appConfig.io[2].inverted = false;
    appConfig.io[2].pullup   = false;

    // LED
    appConfig.ledPin        = LED_PIN;
    appConfig.ledBrightness = LED_BRIGHTNESS_DEFAULT;

    // System
    appConfig.tzOffset  = 3600;                 // UTC+1
    strlcpy(appConfig.ntpServer, "pool.ntp.org", sizeof(appConfig.ntpServer));
    appConfig.logLevel  = LOG_LEVEL_INFO;
    strlcpy(appConfig.otaManifestUrl,
            "https://raw.githubusercontent.com/danielguedel/HMS-GW-S3/main/release/manifest.json",
            sizeof(appConfig.otaManifestUrl));
}

void configLoad() {
    if (!LittleFS.exists(CONFIG_FILE)) {
        if (LittleFS.exists("/config.tmp")) {
            LOG_W(MOD_CFG, "Recovering config from /config.tmp");
            LittleFS.rename("/config.tmp", CONFIG_FILE);
        } else {
            LOG_W(MOD_CFG, "No %s  -  using defaults", CONFIG_FILE);
            configSetDefaults();
            return;
        }
    }
    File f = LittleFS.open(CONFIG_FILE, "r");
    if (!f) {
        LOG_W(MOD_CFG, "Cannot open %s  -  using defaults", CONFIG_FILE);
        configSetDefaults();
        return;
    }
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (err) {
        LOG_E(MOD_CFG, "JSON parse error: %s  -  using defaults", err.c_str());
        configSetDefaults();
        return;
    }

    // Start from defaults so missing keys fall back gracefully
    configSetDefaults();

    // WiFi
    if (doc["wifiSsid"].is<const char*>())
        strlcpy(appConfig.wifiSsid, doc["wifiSsid"].as<const char*>(), sizeof(appConfig.wifiSsid));
    if (doc["wifiPass"].is<const char*>())
        strlcpy(appConfig.wifiPass, doc["wifiPass"].as<const char*>(), sizeof(appConfig.wifiPass));
    if (!doc["wifiApFallback"].isNull())
        appConfig.wifiApFallback = doc["wifiApFallback"].as<bool>();

    // DTU
    if (doc["dtuHost"].is<const char*>())
        strlcpy(appConfig.dtuHost, doc["dtuHost"].as<const char*>(), sizeof(appConfig.dtuHost));
    if (!doc["dtuPort"].isNull())             appConfig.dtuPort             = doc["dtuPort"].as<int>();
    if (!doc["dtuInterval"].isNull()) {
        int iv = doc["dtuInterval"].as<int>();
        appConfig.dtuInterval = (iv < DTU_MIN_INTERVAL) ? DTU_MIN_INTERVAL : iv;
    }
    if (!doc["dtuCloudPause"].isNull())       appConfig.dtuCloudPause       = doc["dtuCloudPause"].as<int>();
    if (!doc["dtuRebootAfterFails"].isNull()) appConfig.dtuRebootAfterFails = doc["dtuRebootAfterFails"].as<int>();

    // Power Limit
    if (!doc["powerLimitDefault"].isNull()) {
        int pl = doc["powerLimitDefault"].as<int>();
        appConfig.powerLimitDefault = (pl < 0) ? 0 : (pl > 100 ? 100 : pl);
    }
    if (!doc["powerLimitTimeout"].isNull()) appConfig.powerLimitTimeout = doc["powerLimitTimeout"].as<int>();

    // MQTT
    if (doc["mqttHost"].is<const char*>())
        strlcpy(appConfig.mqttHost,  doc["mqttHost"].as<const char*>(),  sizeof(appConfig.mqttHost));
    if (!doc["mqttPort"].isNull()) {
        int mp = doc["mqttPort"].as<int>();
        appConfig.mqttPort = (mp >= 1 && mp <= 65535) ? (uint16_t)mp : MQTT_DEFAULT_PORT;
    }
    if (doc["mqttUser"].is<const char*>())
        strlcpy(appConfig.mqttUser,  doc["mqttUser"].as<const char*>(),  sizeof(appConfig.mqttUser));
    if (doc["mqttPass"].is<const char*>())
        strlcpy(appConfig.mqttPass,  doc["mqttPass"].as<const char*>(),  sizeof(appConfig.mqttPass));
    if (doc["mqttTopic"].is<const char*>())
        strlcpy(appConfig.mqttTopic, doc["mqttTopic"].as<const char*>(), sizeof(appConfig.mqttTopic));
    if (!doc["mqttRetain"].isNull())      appConfig.mqttRetain      = doc["mqttRetain"].as<bool>();
    if (!doc["mqttHaDiscovery"].isNull()) appConfig.mqttHaDiscovery = doc["mqttHaDiscovery"].as<bool>();
    if (!doc["mqttOpenDtu"].isNull())     appConfig.mqttOpenDtu     = doc["mqttOpenDtu"].as<bool>();

    // GPIO Relay
    if (!doc["relayPin"].isNull())
        appConfig.relay.pin = clampPin(doc["relayPin"].as<int>(), RELAY_PIN);
    if (!doc["relayInverted"].isNull()) appConfig.relay.inverted = doc["relayInverted"].as<bool>();

    // GPIO io1–io3
    const char*   keys[]     = {"io1","io2","io3"};
    const uint8_t ioDefPin[] = {IO1_PIN, IO2_PIN, IO3_PIN};
    for (int i = 0; i < 3; i++) {
        JsonObjectConst io = doc[keys[i]];
        if (io.isNull()) continue;
        if (!io["pin"].isNull())
            appConfig.io[i].pin = clampPin(io["pin"].as<int>(), ioDefPin[i]);
        if (!io["mode"].isNull()) {
            int m = io["mode"].as<int>();
            appConfig.io[i].mode = (m >= 0 && m <= (int)IO_RESERVED) ? (IoMode)m : IO_INPUT;
        }
        if (io["altFunction"].is<const char*>())
            strlcpy(appConfig.io[i].altFunction, io["altFunction"].as<const char*>(), sizeof(appConfig.io[i].altFunction));
        if (!io["inverted"].isNull()) appConfig.io[i].inverted = io["inverted"].as<bool>();
        if (!io["pullup"].isNull())   appConfig.io[i].pullup   = io["pullup"].as<bool>();
    }

    // LED
    if (!doc["ledPin"].isNull())
        appConfig.ledPin = clampPin(doc["ledPin"].as<int>(), LED_PIN);
    if (!doc["ledBrightness"].isNull()) {
        int lb = doc["ledBrightness"].as<int>();
        appConfig.ledBrightness = (uint8_t)(lb < 0 ? 0 : (lb > 255 ? 255 : lb));
    }

    // System
    if (!doc["tzOffset"].isNull())  appConfig.tzOffset  = doc["tzOffset"].as<int>();
    if (doc["ntpServer"].is<const char*>())
        strlcpy(appConfig.ntpServer, doc["ntpServer"].as<const char*>(), sizeof(appConfig.ntpServer));
    if (!doc["logLevel"].isNull()) appConfig.logLevel = doc["logLevel"].as<int>();
    if (doc["otaManifestUrl"].is<const char*>())
        strlcpy(appConfig.otaManifestUrl, doc["otaManifestUrl"].as<const char*>(), sizeof(appConfig.otaManifestUrl));

    LOG_I(MOD_CFG, "Config loaded: ssid=%s  dtu=%s:%d  mqtt=%s:%d",
          appConfig.wifiSsid, appConfig.dtuHost, appConfig.dtuPort,
          appConfig.mqttHost, appConfig.mqttPort);
}

void configSave() {
    JsonDocument doc;

    doc["wifiSsid"]        = appConfig.wifiSsid;
    doc["wifiPass"]        = appConfig.wifiPass;
    doc["wifiApFallback"]  = appConfig.wifiApFallback;

    doc["dtuHost"]             = appConfig.dtuHost;
    doc["dtuPort"]             = appConfig.dtuPort;
    doc["dtuInterval"]         = appConfig.dtuInterval;
    doc["dtuCloudPause"]       = appConfig.dtuCloudPause;
    doc["dtuRebootAfterFails"] = appConfig.dtuRebootAfterFails;

    doc["powerLimitDefault"] = appConfig.powerLimitDefault;
    doc["powerLimitTimeout"] = appConfig.powerLimitTimeout;

    doc["mqttHost"]        = appConfig.mqttHost;
    doc["mqttPort"]        = appConfig.mqttPort;
    doc["mqttUser"]        = appConfig.mqttUser;
    doc["mqttPass"]        = appConfig.mqttPass;
    doc["mqttTopic"]       = appConfig.mqttTopic;
    doc["mqttRetain"]      = appConfig.mqttRetain;
    doc["mqttHaDiscovery"] = appConfig.mqttHaDiscovery;
    doc["mqttOpenDtu"]     = appConfig.mqttOpenDtu;

    doc["relayPin"]      = appConfig.relay.pin;
    doc["relayInverted"] = appConfig.relay.inverted;

    const char* keys[] = {"io1","io2","io3"};
    for (int i = 0; i < 3; i++) {
        doc[keys[i]]["pin"]         = appConfig.io[i].pin;
        doc[keys[i]]["mode"]        = (int)appConfig.io[i].mode;
        doc[keys[i]]["altFunction"] = appConfig.io[i].altFunction;
        doc[keys[i]]["inverted"]    = appConfig.io[i].inverted;
        doc[keys[i]]["pullup"]      = appConfig.io[i].pullup;
    }

    doc["ledPin"]        = appConfig.ledPin;
    doc["ledBrightness"] = appConfig.ledBrightness;

    doc["tzOffset"]  = appConfig.tzOffset;
    doc["ntpServer"] = appConfig.ntpServer;
    doc["logLevel"]       = appConfig.logLevel;
    doc["otaManifestUrl"] = appConfig.otaManifestUrl;

    // Write to temp file first, then rename  -  protects against config corruption on reset
    static const char* TMP_FILE = "/config.tmp";
    File f = LittleFS.open(TMP_FILE, "w");
    if (!f) { LOG_E(MOD_CFG, "Cannot write %s", TMP_FILE); return; }
    serializeJson(doc, f);
    f.close();
    LittleFS.remove(CONFIG_FILE);
    LittleFS.rename(TMP_FILE, CONFIG_FILE);
    LOG_I(MOD_CFG, "Config saved");
}
