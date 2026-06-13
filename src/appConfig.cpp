#include "appConfig.h"
#include "config.h"
#include "logger.h"
#include "systemState.h"

#include <LittleFS.h>
#include <ArduinoJson.h>
#include <freertos/semphr.h>
#include <string.h>

AppConfig appConfig;

void configSetDefaults() {
    memset(&appConfig, 0, sizeof(AppConfig));
    strlcpy(appConfig.wifiSsid,     "",              sizeof(appConfig.wifiSsid));
    strlcpy(appConfig.wifiPassword, "",              sizeof(appConfig.wifiPassword));
    snprintf(appConfig.hostname, sizeof(appConfig.hostname),
             "hmsgws3-%06llX", (unsigned long long)(ESP.getEfuseMac() & 0xFFFFFF));
    strlcpy(appConfig.dtuHost,   "192.168.1.100",   sizeof(appConfig.dtuHost));
    appConfig.dtuPort             = DTU_DEFAULT_PORT;
    appConfig.dtuInterval         = DTU_DEFAULT_INTERVAL;
    appConfig.dtuCloudPause       = DTU_DEFAULT_CLOUD_PAUSE;
    appConfig.dtuRebootAfterFails = DTU_REBOOT_AFTER_FAILS;
    strlcpy(appConfig.mqttHost,  "",                sizeof(appConfig.mqttHost));
    appConfig.mqttPort            = MQTT_DEFAULT_PORT;
    strlcpy(appConfig.mqttUser,  "",                sizeof(appConfig.mqttUser));
    strlcpy(appConfig.mqttPass,  "",                sizeof(appConfig.mqttPass));
    snprintf(appConfig.mqttTopic, sizeof(appConfig.mqttTopic),
             "hmsgws3_%06llX", (unsigned long long)(ESP.getEfuseMac() & 0xFFFFFF));
    appConfig.mqttTls          = false;
    appConfig.mqttHaDiscovery  = true;
    appConfig.mqttOpenDtu      = false;
    appConfig.mqttQos          = 0;
    appConfig.mqttRetain       = false;
    appConfig.relay = { RELAY_PIN, true,  false, false };
    appConfig.gp[0] = { GPIO1_PIN, false, true,  false };
    appConfig.gp[1] = { GPIO2_PIN, false, true,  false };
    appConfig.gp[2] = { GPIO3_PIN, false, true,  false };
    appConfig.gp[3] = { GPIO4_PIN, false, true,  false };
    appConfig.tzOffset        = 3600;
    appConfig.ledBrightness   = NEOPIXEL_BRIGHTNESS_DEF;
    appConfig.logLevel        = LOG_LEVEL_DEBUG;   // debug for DTU troubleshooting
    appConfig.protectSettings = false;
    appConfig.webPort         = WEB_DEFAULT_PORT;
    strlcpy(appConfig.apSsid, AP_DEFAULT_SSID, sizeof(appConfig.apSsid));
}

bool configLoad() {
    // LittleFS must be mounted before calling configLoad()
    if (!LittleFS.exists(CONFIG_FILE)) {
        LOG_W(MOD_CFG, "No config.json found (first boot or empty FS) — using defaults");
        configSetDefaults();
        return false;
    }
    File f = LittleFS.open(CONFIG_FILE, "r");
    if (!f) {
        LOG_W(MOD_CFG, "Cannot open config.json — using defaults");
        configSetDefaults();
        return false;
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (err) { LOG_E(MOD_CFG, "JSON parse: %s", err.c_str()); configSetDefaults(); return false; }

    strlcpy(appConfig.wifiSsid,     doc["wifi"]["ssid"]     | "", sizeof(appConfig.wifiSsid));
    strlcpy(appConfig.wifiPassword, doc["wifi"]["password"] | "", sizeof(appConfig.wifiPassword));
    strlcpy(appConfig.hostname,     doc["wifi"]["hostname"] | appConfig.hostname, sizeof(appConfig.hostname));

    strlcpy(appConfig.dtuHost, doc["dtu"]["host"] | "192.168.1.100", sizeof(appConfig.dtuHost));
    appConfig.dtuPort             = doc["dtu"]["port"]             | (int)DTU_DEFAULT_PORT;
    appConfig.dtuInterval         = doc["dtu"]["interval"]         | (int)DTU_DEFAULT_INTERVAL;
    appConfig.dtuCloudPause       = doc["dtu"]["cloudPause"]       | (int)DTU_DEFAULT_CLOUD_PAUSE;
    appConfig.dtuRebootAfterFails = doc["dtu"]["rebootAfterFails"] | (int)DTU_REBOOT_AFTER_FAILS;

    strlcpy(appConfig.mqttHost,  doc["mqtt"]["host"]  | "", sizeof(appConfig.mqttHost));
    appConfig.mqttPort            = doc["mqtt"]["port"]  | (int)MQTT_DEFAULT_PORT;
    strlcpy(appConfig.mqttUser,  doc["mqtt"]["user"]  | "", sizeof(appConfig.mqttUser));
    strlcpy(appConfig.mqttPass,  doc["mqtt"]["pass"]  | "", sizeof(appConfig.mqttPass));
    strlcpy(appConfig.mqttTopic, doc["mqtt"]["topic"] | appConfig.mqttTopic, sizeof(appConfig.mqttTopic));
    appConfig.mqttTls         = doc["mqtt"]["tls"]    | false;
    appConfig.mqttHaDiscovery = doc["mqtt"]["ha"]     | true;
    appConfig.mqttOpenDtu     = doc["mqtt"]["openDTU"]| false;
    appConfig.mqttQos         = doc["mqtt"]["qos"]    | 0;
    appConfig.mqttRetain      = doc["mqtt"]["retain"] | false;

    // GPIO — use JsonObjectConst to avoid copy-constructor issue
    JsonObjectConst relay = doc["gpio"]["relay"];
    if (!relay.isNull()) {
        appConfig.relay.pin      = relay["pin"]      | (int)RELAY_PIN;
        appConfig.relay.isOutput = relay["output"]   | true;
        appConfig.relay.inverted = relay["inverted"] | false;
    }
    const char* gpKeys[] = {"gp1","gp2","gp3","gp4"};
    const uint8_t gpPins[] = {GPIO1_PIN, GPIO2_PIN, GPIO3_PIN, GPIO4_PIN};
    for (int i = 0; i < 4; i++) {
        JsonObjectConst gp = doc["gpio"][gpKeys[i]];
        if (!gp.isNull()) {
            appConfig.gp[i].pin      = gp["pin"]      | (int)gpPins[i];
            appConfig.gp[i].isOutput = gp["output"]   | false;
            appConfig.gp[i].pullup   = gp["pullup"]   | true;
            appConfig.gp[i].inverted = gp["inverted"] | false;
        }
    }

    appConfig.tzOffset        = doc["system"]["tz"]            | 3600;
    appConfig.ledBrightness   = doc["system"]["ledBrightness"] | (int)NEOPIXEL_BRIGHTNESS_DEF;
    appConfig.logLevel        = doc["system"]["logLevel"]      | (int)LOG_LEVEL_DEFAULT;
    appConfig.protectSettings = doc["system"]["protect"]       | false;
    appConfig.webPort         = doc["system"]["webPort"]       | (int)WEB_DEFAULT_PORT;
    strlcpy(appConfig.apSsid, doc["system"]["apSsid"] | AP_DEFAULT_SSID, sizeof(appConfig.apSsid));

    if (appConfig.dtuInterval < DTU_MIN_INTERVAL) appConfig.dtuInterval = DTU_MIN_INTERVAL;

    LOG_I(MOD_CFG, "Config loaded. SSID=%s  DTU=%s  MQTT=%s",
          appConfig.wifiSsid, appConfig.dtuHost, appConfig.mqttHost);
    return true;
}

bool configSave() {
    JsonDocument doc;

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
    if (!f) { LOG_E(MOD_CFG, "Cannot write config"); return false; }
    serializeJson(doc, f);
    f.close();
    LOG_I(MOD_CFG, "Config saved");
    return true;
}
