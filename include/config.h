#pragma once

// ─── Firmware Version ─────────────────────────────────────────────────────────
#ifndef FW_VERSION
  #define FW_VERSION "0.2.0"
#endif
#ifndef FW_DATE
  #define FW_DATE __DATE__
#endif
#ifndef BUILD_NUMBER
  #define BUILD_NUMBER 0
#endif

// ─── GPIO Pin Defaults (anpassbar im Web-GUI, gespeichert in config.json) ─────
// Relay
#ifndef RELAY_PIN
  #define RELAY_PIN        1    // GPIO1
#endif
// GP1–GP4
#ifndef GP1_PIN
  #define GP1_PIN          0    // GPIO0 — INPUT
#endif
#ifndef GP2_PIN
  #define GP2_PIN          2    // GPIO2 — I2C_RESERVED (SDA)
#endif
#ifndef GP3_PIN
  #define GP3_PIN          3    // GPIO3 — I2C_RESERVED (SCL)
#endif
// GP4 — kein Default-Pin zugewiesen (konfigurierbar im Web-GUI)

// LED (WS2812B onboard)
#ifndef LED_PIN
  #define LED_PIN         38    // GPIO38
#endif
#define NEOPIXEL_PIN      LED_PIN   // Rückwärts-Kompatibilität

// Boot / Factory Reset
#define BOOT_PIN           0        // BOOT-Taste (GPIO0, auch GP1 im Default)

// ─── NeoPixel / LED ───────────────────────────────────────────────────────────
#define LED_COUNT               1
#define LED_BRIGHTNESS_DEFAULT  80   // 0–255
#define NEOPIXEL_COUNT          LED_COUNT
#define NEOPIXEL_BRIGHTNESS_DEF LED_BRIGHTNESS_DEFAULT

// ─── Serial Console ───────────────────────────────────────────────────────────
#define SERIAL_BAUD        115200

// ─── DTU Defaults ─────────────────────────────────────────────────────────────
#define DTU_DEFAULT_PORT        10081
#define DTU_MIN_INTERVAL        31
#define DTU_DEFAULT_INTERVAL    31
#define DTU_CONNECT_TIMEOUT_MS  5000
#define DTU_RESPONSE_TIMEOUT_MS 5000
#define DTU_REBOOT_AFTER_FAILS  3
#define DTU_DEFAULT_CLOUD_PAUSE 30

// ─── Power Limit ──────────────────────────────────────────────────────────────
#define POWER_LIMIT_DEFAULT     100  // Rückfall-Wert [%]
#define POWER_LIMIT_TIMEOUT     0    // Timeout [s], 0 = deaktiviert

// ─── Web Server ───────────────────────────────────────────────────────────────
#define WEB_DEFAULT_PORT   80
#define AP_DEFAULT_SSID    "HMS-GW-S3"
#define AP_IP              "192.168.4.1"
#define MDNS_NAME          "hmsgws3"

// ─── MQTT Defaults ────────────────────────────────────────────────────────────
#define MQTT_DEFAULT_PORT  1883
#define MQTT_TLS_PORT      8883
#define MQTT_RECONNECT_MS  5000
#define MQTT_KEEPALIVE_S   60
#define MQTT_DEFAULT_TOPIC "hmsgws3"

// ─── Factory Reset ────────────────────────────────────────────────────────────
#define FACTORY_RESET_HOLD_MS  5000   // BOOT-Taste diese Dauer halten

// ─── FreeRTOS Task Priorities ─────────────────────────────────────────────────
#define TASK_PRIO_WIFI         5
#define TASK_PRIO_DTU          4
#define TASK_PRIO_GPIO         4
#define TASK_PRIO_MQTT         3
#define TASK_PRIO_WEBSERVER    3
#define TASK_PRIO_LED          2
#define TASK_PRIO_NEOPIXEL     TASK_PRIO_LED   // Rückwärts-Kompatibilität
#define TASK_PRIO_SERIAL       2
#define TASK_PRIO_SYSMONITOR   1

// ─── FreeRTOS Stack Sizes (Bytes) ─────────────────────────────────────────────
#define STACK_WIFI          6144
#define STACK_DTU           8192
#define STACK_MQTT          6144
#define STACK_WEBSERVER     8192
#define STACK_GPIO          4096
#define STACK_LED           3072
#define STACK_NEOPIXEL      STACK_LED           // Rückwärts-Kompatibilität
#define STACK_SERIAL        4096
#define STACK_SYSMONITOR    3072

// ─── FreeRTOS Core Assignment ─────────────────────────────────────────────────
// Core 0: ausschliesslich WiFi-Stack (lwIP)
// Core 1: alle User-Tasks
#define CORE_WIFI          1
#define CORE_DTU           1
#define CORE_MQTT          1
#define CORE_WEBSERVER     1
#define CORE_GPIO          1
#define CORE_LED           1
#define CORE_NEOPIXEL      CORE_LED             // Rückwärts-Kompatibilität
#define CORE_SERIAL        1
#define CORE_SYSMONITOR    1

// ─── Log Levels ───────────────────────────────────────────────────────────────
#define LOG_LEVEL_ERROR    0
#define LOG_LEVEL_WARN     1
#define LOG_LEVEL_INFO     2
#define LOG_LEVEL_DEBUG    3
#define LOG_LEVEL_DEFAULT  LOG_LEVEL_INFO

// ─── Config File ──────────────────────────────────────────────────────────────
#define CONFIG_FILE        "/config.json"
