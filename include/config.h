#pragma once

// ─── Firmware Version ────────────────────────────────────────────────────────
#ifndef FW_VERSION
  #define FW_VERSION "0.1.0"
#endif
#ifndef FW_DATE
  #define FW_DATE __DATE__
#endif

// ─── GPIO Pin Definitions ─────────────────────────────────────────────────────
#ifndef NEOPIXEL_PIN
  #define NEOPIXEL_PIN    48
#endif
#ifndef RELAY_PIN
  #define RELAY_PIN        1
#endif
#ifndef GPIO1_PIN
  #define GPIO1_PIN        2
#endif
#ifndef GPIO2_PIN
  #define GPIO2_PIN        3
#endif
#ifndef GPIO3_PIN
  #define GPIO3_PIN        4
#endif
#ifndef GPIO4_PIN
  #define GPIO4_PIN        5
#endif

// ─── NeoPixel ────────────────────────────────────────────────────────────────
#define NEOPIXEL_COUNT      1
#define NEOPIXEL_BRIGHTNESS 80   // 0–255, default

// ─── Serial Console ───────────────────────────────────────────────────────────
#define SERIAL_BAUD        115200

// ─── DTU Defaults ─────────────────────────────────────────────────────────────
#define DTU_DEFAULT_PORT   10081
#define DTU_MIN_INTERVAL   31     // seconds
#define DTU_DEFAULT_INTERVAL 31

// ─── Web Server ───────────────────────────────────────────────────────────────
#define WEB_DEFAULT_PORT   80
#define AP_DEFAULT_SSID    "HMS-GW-S3"

// ─── MQTT Defaults ────────────────────────────────────────────────────────────
#define MQTT_DEFAULT_PORT  1883
#define MQTT_TLS_PORT      8883

// ─── FreeRTOS Task Priorities ─────────────────────────────────────────────────
#define TASK_PRIO_DTU          5
#define TASK_PRIO_MQTT         4
#define TASK_PRIO_WEBSERVER    3
#define TASK_PRIO_GPIO         3
#define TASK_PRIO_NEOPIXEL     2
#define TASK_PRIO_SERIAL       2
#define TASK_PRIO_SYSMONITOR   1

// ─── FreeRTOS Stack Sizes (in words) ──────────────────────────────────────────
#define STACK_DTU          6144
#define STACK_MQTT         4096
#define STACK_WEBSERVER    6144
#define STACK_NEOPIXEL     2048
#define STACK_GPIO         2048
#define STACK_SERIAL       3072
#define STACK_SYSMONITOR   2048

// ─── FreeRTOS Core Assignment ─────────────────────────────────────────────────
#define CORE_DTU           0
#define CORE_MQTT          0
#define CORE_WEBSERVER     1
#define CORE_NEOPIXEL      1
#define CORE_GPIO          1
#define CORE_SERIAL        1
#define CORE_SYSMONITOR    1

// ─── Log Levels ───────────────────────────────────────────────────────────────
#define LOG_LEVEL_ERROR    0
#define LOG_LEVEL_WARN     1
#define LOG_LEVEL_INFO     2
#define LOG_LEVEL_DEBUG    3
#define LOG_LEVEL_DEFAULT  LOG_LEVEL_INFO
