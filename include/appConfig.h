#pragma once
#include <Arduino.h>
#include "config.h"

// IO-Pin-Modus (Spec §8)
enum IoMode : uint8_t {
    IO_OUTPUT = 0,
    IO_INPUT,
    IO_RESERVED         // Pin für eine Zusatzfunktion reserviert (siehe altFunction)
};

// --- Application Configuration ------------------------------------------------
struct AppConfig {
    // WiFi
    char wifiSsid[33];
    char wifiPass[65];
    bool wifiApFallback;        // AP-Modus wenn WiFi nicht erreichbar

    // WiFi  -  Static IP (useStaticIp=false -> DHCP, default)
    bool useStaticIp;
    char staticIp[16];          // z.B. "192.168.1.50"
    char subnet[16];            // default: "255.255.255.0"
    char gateway[16];           // z.B. "192.168.1.1"  -  wird auch als DNS-Server verwendet

    // DTU
    char     dtuHost[40];
    uint16_t dtuPort;           // default: 10081
    int      dtuInterval;       // Abfrage-Intervall [s], default: 31
    int      dtuCloudPause;     // Wartezeit bei Cloud-Sync [s], default: 30
    int      dtuRebootAfterFails; // Reconnect nach N Fehlern, default: 3

    // Power Limit
    int  powerLimitDefault;     // Rückfall-Wert [%], default: 100
    int  powerLimitTimeout;     // Timeout [s], 0 = deaktiviert

    // MQTT
    char     mqttHost[40];
    uint16_t mqttPort;          // default: 1883
    char     mqttUser[33];
    char     mqttPass[65];
    char     mqttTopic[33];     // default: "hmsgws3"
    bool     mqttRetain;
    bool     mqttHaDiscovery;   // HA Auto-Discovery aktivieren
    bool     mqttOpenDtu;       // OpenDTU-kompatible Topics

    // GPIO  -  Default-Pinbelegung (anpassbar im Web-GUI)
    struct {
        uint8_t pin;            // default: GPIO1
        bool    inverted;
    } relay;

    struct {
        uint8_t pin;             // defaults: GPIO2, GPIO3, GPIO4
        IoMode  mode;            // IO_OUTPUT / IO_INPUT / IO_RESERVED
        char    altFunction[16]; // rein informativ, z.B. "I2C_SDA"  -  ändert kein Verhalten
        bool    inverted;
        bool    pullup;
    } io[3];

    // LED
    uint8_t ledPin;             // default: GPIO38 (WS2812B onboard)
    uint8_t ledBrightness;      // 0–255, default: 80

    // System
    int  tzOffset;              // Zeitzone-Offset [s], default: 3600 (UTC+1)
    char ntpServer[65];         // default: "pool.ntp.org"
    int  logLevel;              // 0=ERROR, 1=WARN, 2=INFO, 3=DEBUG

    // Internet OTA
    char otaManifestUrl[256];   // URL zum Versions-Manifest (leer = deaktiviert)

    // Web-Server
    bool     webAuthEnabled;    // Benutzername/Passwort-Schutz für die Web-GUI, default: false
    char     webUser[33];       // Benutzername, default: "admin"
    char     webPass[65];       // Passwort
    uint16_t webPort;           // Web-GUI Port, default: 80
};

extern AppConfig appConfig;

void configLoad();
void configSave();
void configSetDefaults();

// Restores config from an uploaded backup JSON (web GUI). Validates,
// applies (same rules as configLoad()) and persists via configSave().
// Returns false without changing anything if the JSON is invalid or
// doesn't look like a config backup.
bool configRestoreFromJson(const char* json, size_t len);
