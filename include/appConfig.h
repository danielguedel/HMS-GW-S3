#pragma once
#include <Arduino.h>

// ─── GPIO pin config ──────────────────────────────────────────────────────────
struct GpioPinConfig {
    uint8_t pin;
    bool    isOutput;
    bool    pullup;
    bool    inverted;
};

// ─── Full application configuration ──────────────────────────────────────────
struct AppConfig {
    // WiFi
    char wifiSsid[64];
    char wifiPassword[64];
    char hostname[32];

    // DTU
    char    dtuHost[64];
    uint16_t dtuPort;
    uint16_t dtuInterval;       // seconds, min 31
    uint8_t  dtuCloudPause;     // seconds
    uint8_t  dtuRebootAfterFails;

    // MQTT
    char     mqttHost[64];
    uint16_t mqttPort;
    char     mqttUser[64];
    char     mqttPass[64];
    char     mqttTopic[64];
    bool     mqttTls;
    bool     mqttHaDiscovery;
    bool     mqttOpenDtu;
    uint8_t  mqttQos;
    bool     mqttRetain;

    // GPIO
    GpioPinConfig relay;
    GpioPinConfig gp[4];        // gp[0]=GP1 … gp[3]=GP4

    // System
    int32_t  tzOffset;          // seconds from UTC
    uint8_t  ledBrightness;     // 0–255
    uint8_t  logLevel;          // 0–3
    bool     protectSettings;
    uint16_t webPort;
    char     apSsid[32];
};

// Global config instance (access via configMutex)
extern AppConfig appConfig;

// Load/save
bool configLoad();
bool configSave();
void configSetDefaults();
