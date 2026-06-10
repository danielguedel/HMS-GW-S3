#include "appConfig.h"
#include "config.h"
#include "logger.h"
#include "systemState.h"
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <freertos/semphr.h>
#include <string.h>

AppConfig appConfig;

// ─── Defaults ─────────────────────────────────────────────────────────────────
void configSetDefaults() {
    memset(&appConfig, 0, sizeof(AppConfig));

    // WiFi
    strlcpy(appConfig.wifiSsid,     "",          sizeof(appConfig.wifiSsid));
    strlcpy(appConfig.wifiPassword, "",          sizeof(appConfig.wifiPassword));
    // hostname derived from chip ID at runtime
    snprintf(appConfig.hostname, sizeof(appConfig.hostname),
             "hmsgws3-%06llX", (unsigned long long)(ESP.getEfuseMac() & 0xFFFFFF));

    // DTU
    strlcpy(appConfig.dtuHost,      "192.168.1.100", sizeof(appConfig.dtuHost));
    appConfig.dtuPort              = DTU_DEFAULT_PORT;
    appConfig.dtuInterval          = DTU_DEFAULT_INTERVAL;
    appConfig.dtuCloudPause        = DTU_DEFAULT_CLOUD_PAUSE;
    appConfig.dtuRebootAfterFails  = DTU_REBOOT_AFTER_FAILS;

    // MQTT
    strlcpy(appConfig.mqttHost,  "", sizeof(appConfig.mqttHost));
    appConfig.mqttPort           = MQTT_DEFAULT_PORT;
    strlcpy(appConfig.mqttUser,  "", sizeof(appConfig.mqttUser));
    strlcpy(appConfig.mqttPass,  "", sizeof(appConfig.mqttPass));
    snprintf(appConfig.mqttTopic, sizeof(appConfig.mqttTopic),
             "hmsgws3_%06llX", (unsigned long long)(ESP.getEfuseMac() & 0xFFFFFF));
    appConfig.mqttTls            = false;
    appConfig.mqttHaDiscovery    = true;
    appConfig.mqttOpenDtu        = false;
    appConfig.mqttQos            = 0;
    appConfig.mqttRetain         = false;

    // GPIO — relay output, all GPIOs input with pullup
    appConfig.relay = { RELAY_PIN, true,  false, false };
    appConfig.gp[0] = { GPIO1_PIN, false, true,  false };
    appConfig.gp[1] = { GPIO2_PIN, false, true,  false };
    appConfig.gp[2] = { GPIO3_PIN, false, true,  false };
    appConfig.gp[3] = { GPIO4_PIN, false, true,  false };

    // System
    appConfig.tzOffset        = 3600;   // UTC+1
    appConfig.ledBrightness   = NEOPIXEL_BRIGHTNESS_DEF;
    appConfig.logLevel        = LOG_LEVEL_DEFAULT;
    appConfig.protectSettings = false;
    appConfig.webPort         = WEB_DEFAULT_PORT;
    strlcpy(appConfig.apSsid, AP_DEFAULT_SSID, sizeof(appConfig.apSsid));
}

// ─── Load from LittleFS ───────────────────────────────────────────────────────
bool configLoad() {
    if (!LittleFS.exists(CONFIG_FILE)) {
        LOG_W(MOD_CFG, "No config file found, using defaults");
        configSetDefaults();
        return false;
    }
    File f = LittleFS.open(CONFIG_FILE, "r");
    if (!f) {
        LOG_E(MOD_CFG, "Failed to open config file");
        configSetDefaults();
        return false;
    }

    DynamicJsonDocument doc(4096);
    DeserializationError err = deserializeJson(doc, f);
    f.close();

    if (err) {
        LOG_E(MOD_CFG, "JSON parse error: %s", err.c_str());
        configSetDefaults();
        return false;
    }

    // WiFi
    strlcpy(appConfig.wifiSsid,     doc["wifi"]["ssid"]     | "", sizeof(appConfig.wifiSsid));
    strlcpy(appConfig.wifiPassword, doc["wifi"]["password"] | "", sizeof(appConfig.wifiPassword));
    strlcpy(appConfig.hostname,     doc["wifi"]["hostname"] | appConfig.hostname, sizeof(appConfig.hostname));

    // DTU
    strlcpy(appConfig.dtuHost,     doc["dtu"]["host"]     | "192.168.1.100", sizeof(appConfig.dtuHost));
    appConfig.dtuPort             = doc["dtu"]["port"]             | DTU_DEFAULT_PORT;
    appConfig.dtuInterval         = doc["dtu"]["interval"]         | DTU_DEFAULT_INTERVAL;
    appConfig.dtuCloudPause       = doc["dtu"]["cloudPause"]       | (uint8_t)DTU_DEFAULT_CLOUD_PAUSE;
    appConfig.dtuRebootAfterFails = doc["dtu"]["rebootAfterFails"] | (uint8_t)DTU_REBOOT_AFTER_FAILS;

    // MQTT
    strlcpy(appConfig.mqttHost,  doc["mqtt"]["host"]  | "", sizeof(appConfig.mqttHost));
    appConfig.mqttPort           = doc["mqtt"]["port"]  | MQTT_DEFAULT_PORT;
    strlcpy(appConfig.mqttUser,  doc["mqtt"]["user"]  | "", sizeof(appConfig.mqttUser));
    strlcpy(appConfig.mqttPass,  doc["mqtt"]["pass"]  | "", sizeof(appConfig.mqttPass));
    strlcpy(appConfig.mqttTopic, doc["mqtt"]["topic"] | appConfig.mqttTopic, sizeof(appConfig.mqttTopic));
    appConfig.mqttTls         = doc["mqtt"]["tls"]         | false;
    appConfig.mqttHaDiscovery = doc["mqtt"]["ha"]          | true;
    appConfig.mqttOpenDtu     = doc["mqtt"]["openDTU"]     | false;
    appConfig.mqttQos         = doc["mqtt"]["qos"]         | (uint8_t)0;
    appConfig.mqttRetain      = doc["mqtt"]["retain"]      | false;

    // GPIO
    auto relay = doc["gpio"]["relay"];
    appConfig.relay.pin      = relay["pin"]      | (uint8_t)RELAY_PIN;
    appConfig.relay.isOutput = relay["output"]   | true;
    appConfig.relay.inverted = relay["inverted"] | false;

    const char* gpKeys[] = {"gp1","gp2","gp3","gp4"};
    const uint8_t gpPins[] = {GPIO1_PIN, GPIO2_PIN, GPIO3_PIN, GPIO4_PIN};
    for (int i = 0; i < 4; i++) {
        auto gp = doc["gpio"][gpKeys[i]];
        appConfig.gp[i].pin      = gp["pin"]      | gpPins[i];
        appConfig.gp[i].isOutput = gp["output"]   | false;
        appConfig.gp[i].pullup   = gp["pullup"]   | true;
        appConfig.gp[i].inverted = gp["inverted"] | false;
    }

    // System
    appConfig.tzOffset        = doc["system"]["tz"]             | (int32_t)3600;
    appConfig.ledBrightness   = doc["system"]["ledBrightness"]  | (uint8_t)NEOPIXEL_BRIGHTNESS_DEF;
    appConfig.logLevel        = doc["system"]["logLevel"]       | (uint8_t)LOG_LEVEL_DEFAULT;
    appConfig.protectSettings = doc["system"]["protect"]        | false;
    appConfig.webPort         = doc["system"]["webPort"]        | (uint16_t)WEB_DEFAULT_PORT;
    strlcpy(appConfig.apSsid, doc["system"]["apSsid"] | AP_DEFAULT_SSID, sizeof(appConfig.apSsid));

    // Enforce minimum DTU interval
    if (appConfig.dtuInterval < DTU_MIN_INTERVAL)
        appConfig.dtuInterval = DTU_MIN_INTERVAL;

    LOG_I(MOD_CFG, "Config loaded. SSID=%s  DTU=%s  MQTT=%s",
          appConfig.wifiSsid, appConfig.dtuHost, appConfig.mqttHost);
    return true;
}

// ─── Save to LittleFS ─────────────────────────────────────────────────────────
bool configSave() {
    DynamicJsonDocument doc(4096);

    doc["wifi"]["ssid"]     = appConfig.wifiSsid;
    doc["wifi"]["password"] = appConfig.wifiPassword;
    doc["wifi"]["hostname"] = appConfig.hostname;

    doc["dtu"]["host"]             = appConfig.dtuHost;
    doc["dtu"]["port"]             = appConfig.dtuPort;
    doc["dtu"]["interval"]         = appConfig.dtuInterval;
    doc["dtu"]["cloudPause"]       = appConfig.dtuCloudPause;
    doc["dtu"]["rebootAfterFails"] = appConfig.dtuRebootAfterFails;

    doc["mqtt"]["host"]    = appConfig.mqttHost;
    doc["mqtt"]["port"]    = appConfig.mqttPort;
    doc["mqtt"]["user"]    = appConfig.mqttUser;
    doc["mqtt"]["pass"]    = appConfig.mqttPass;
    doc["mqtt"]["topic"]   = appConfig.mqttTopic;
    doc["mqtt"]["tls"]     = appConfig.mqttTls;
    doc["mqtt"]["ha"]      = appConfig.mqttHaDiscovery;
    doc["mqtt"]["openDTU"] = appConfig.mqttOpenDtu;
    doc["mqtt"]["qos"]     = appConfig.mqttQos;
    doc["mqtt"]["retain"]  = appConfig.mqttRetain;

    doc["gpio"]["relay"]["pin"]      = appConfig.relay.pin;
    doc["gpio"]["relay"]["output"]   = appConfig.relay.isOutput;
    doc["gpio"]["relay"]["inverted"] = appConfig.relay.inverted;

    const char* gpKeys[] = {"gp1","gp2","gp3","gp4"};
    for (int i = 0; i < 4; i++) {
        doc["gpio"][gpKeys[i]]["pin"]      = appConfig.gp[i].pin;
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

    File f = LittleFS.open(CONFIG_FILE, "w");
    if (!f) {
        LOG_E(MOD_CFG, "Failed to open config for writing");
        return false;
    }
    serializeJson(doc, f);
    f.close();
    LOG_I(MOD_CFG, "Config saved");
    return true;
}
