#pragma once
#include <Arduino.h>
#include "appConfig.h"

// Module name column width (change here to realign all log output)
#define LOG_MOD_WIDTH 4

// Modules
#define MOD_SYS    "SYS"
#define MOD_DTU    "DTU"
#define MOD_MQTT   "MQTT"
#define MOD_WEB    "WEB"
#define MOD_GPIO   "GPIO"
#define MOD_LED    "LED"
#define MOD_CFG    "CFG"
#define MOD_OTA    "OTA"
#define MOD_DATA   "DATA"
#define MOD_WIFI   "WIFI"

void logInit();
// level: 0=ERROR..3=DEBUG (see LOG_LEVEL_* in config.h); messages above appConfig.logLevel are dropped. Prefer the LOG_E/LOG_W/LOG_I/LOG_D macros below over calling this directly.
void logMsg(uint8_t level, const char* module, const char* fmt, ...);

// Convenience macros
#define LOG_E(mod, fmt, ...) logMsg(0, mod, fmt, ##__VA_ARGS__)
#define LOG_W(mod, fmt, ...) logMsg(1, mod, fmt, ##__VA_ARGS__)
#define LOG_I(mod, fmt, ...) logMsg(2, mod, fmt, ##__VA_ARGS__)
#define LOG_D(mod, fmt, ...) logMsg(3, mod, fmt, ##__VA_ARGS__)
