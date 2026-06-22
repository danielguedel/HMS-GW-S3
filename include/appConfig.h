#pragma once
#include <Arduino.h>
#include "config.h"

// IO pin mode (Spec §8)
enum IoMode : uint8_t {
    IO_OUTPUT = 0,
    IO_INPUT,
    IO_RESERVED         // pin reserved for an additional function (see altFunction)
};

// --- Application Configuration ------------------------------------------------
struct AppConfig {
    // WiFi
    char wifiSsid[33];
    char wifiPass[65];
    bool wifiApFallback;        // fall back to AP mode if WiFi is unreachable

    // WiFi  -  Static IP (useStaticIp=false -> DHCP, default)
    bool useStaticIp;
    char staticIp[16];          // e.g. "192.168.1.50"
    char subnet[16];            // default: "255.255.255.0"
    char gateway[16];           // e.g. "192.168.1.1"  -  also used as the DNS server

    // DTU
    char     dtuHost[40];
    uint16_t dtuPort;           // default: 10081
    int      dtuInterval;       // poll interval [s], default: 31
    int      dtuCloudPause;     // wait time during cloud sync [s], default: 30
    int      dtuRebootAfterFails; // reconnect after N failures, default: 3

    // Power Limit
    int  powerLimitDefault;     // fallback value [%], default: 100
    int  powerLimitTimeout;     // timeout [s], 0 = disabled

    // MQTT
    char     mqttHost[40];
    uint16_t mqttPort;          // default: 1883
    char     mqttUser[33];
    char     mqttPass[65];
    char     mqttTopic[33];     // default: "hmsgws3"
    bool     mqttRetain;
    bool     mqttHaDiscovery;   // enable HA auto-discovery
    bool     mqttOpenDtu;       // OpenDTU-compatible topics

    // GPIO  -  default pin assignment (configurable via the web GUI)
    struct {
        uint8_t pin;            // default: GPIO1
        bool    inverted;
    } relay;

    struct {
        uint8_t pin;             // defaults: GPIO2, GPIO3, GPIO4
        IoMode  mode;            // IO_OUTPUT / IO_INPUT / IO_RESERVED
        char    altFunction[16]; // purely informational, e.g. "I2C_SDA"  -  does not change behavior
        bool    inverted;
        bool    pullup;
    } io[3];

    // LED
    uint8_t ledPin;             // default: GPIO38 (WS2812B onboard)
    uint8_t ledBrightness;      // 0-255, default: 80

    // System
    int  tzOffset;              // timezone offset [s], default: 3600 (UTC+1)
    char ntpServer[65];         // default: "pool.ntp.org"
    int  logLevel;              // 0=ERROR, 1=WARN, 2=INFO, 3=DEBUG

    // Internet OTA
    char otaManifestUrl[256];   // URL to the version manifest (empty = disabled)

    // Web-Server
    bool     webAuthEnabled;    // username/password protection for the web GUI, default: false
    char     webUser[33];       // username, default: "admin"
    char     webPass[65];       // password
    uint16_t webPort;           // web GUI port, default: 80

    // Web GUI  -  purely cosmetic, never read by firmware logic
    char uiLang[3];             // dashboard display language, "en" or "de", default: "en"
};

extern AppConfig appConfig;

// Reads CONFIG_FILE into appConfig, falling back to configSetDefaults() if it's missing or invalid.
void configLoad();
// Persists the current appConfig to CONFIG_FILE.
void configSave();
// Resets appConfig in memory to the build defaults; does not touch CONFIG_FILE.
void configSetDefaults();

// Restores config from an uploaded backup JSON (web GUI). Validates,
// applies (same rules as configLoad()) and persists via configSave().
// Returns false without changing anything if the JSON is invalid or
// doesn't look like a config backup.
bool configRestoreFromJson(const char* json, size_t len);
