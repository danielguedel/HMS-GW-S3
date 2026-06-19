#pragma once

// --- Firmware Version ---------------------------------------------------------
#ifndef FW_VERSION
  #define FW_VERSION "0.2.0"
#endif
#ifndef FW_DATE
  #define FW_DATE __DATE__
#endif
#ifndef BUILD_NUMBER
  #define BUILD_NUMBER 0
#endif

// --- GPIO Pin Defaults (configurable via the web GUI, persisted in config.json) -
// Relay
#ifndef RELAY_PIN
  #define RELAY_PIN        1    // GPIO1
#endif
// IO1-IO3 (generic IO pins, freely configurable via the web GUI)
#ifndef IO1_PIN
  #define IO1_PIN          2    // GPIO2  -  suitable for I2C SDA per the ESP32-S3 datasheet
#endif
#ifndef IO2_PIN
  #define IO2_PIN          3    // GPIO3  -  suitable for I2C SCL per the ESP32-S3 datasheet
#endif
#ifndef IO3_PIN
  #define IO3_PIN          4    // GPIO4  -  suitable for ADC1_CH3 per the ESP32-S3 datasheet
#endif

// LED (WS2812B onboard)
#ifndef LED_PIN
  #define LED_PIN         38    // GPIO38
#endif
#define NEOPIXEL_PIN      LED_PIN   // backward compatibility

// Boot / Factory Reset
#define BOOT_PIN           0        // BOOT button (GPIO0, internal  -  not part of the IO array)

// --- NeoPixel / LED -----------------------------------------------------------
#define LED_COUNT               1
#define LED_BRIGHTNESS_DEFAULT  80   // 0–255
#define NEOPIXEL_COUNT          LED_COUNT
#define NEOPIXEL_BRIGHTNESS_DEF LED_BRIGHTNESS_DEFAULT

// --- Serial Console -----------------------------------------------------------
#define SERIAL_BAUD        115200

// --- DTU Defaults -------------------------------------------------------------
#define DTU_DEFAULT_PORT        10081
#define DTU_MIN_INTERVAL        31
#define DTU_DEFAULT_INTERVAL    31
#define DTU_CONNECT_TIMEOUT_MS  5000
#define DTU_RESPONSE_TIMEOUT_MS 5000
#define DTU_REBOOT_AFTER_FAILS  3
#define DTU_DEFAULT_CLOUD_PAUSE 30

// --- Power Limit --------------------------------------------------------------
#define POWER_LIMIT_DEFAULT     100  // fallback value [%]
#define POWER_LIMIT_TIMEOUT     0    // timeout [s], 0 = disabled

// --- Web Server ---------------------------------------------------------------
#define WEB_DEFAULT_PORT   80
#define AP_DEFAULT_SSID    "HMS-GW-S3"
#define AP_IP              "192.168.4.1"
#define MDNS_NAME          "hmsgws3"

// --- MQTT Defaults ------------------------------------------------------------
#define MQTT_DEFAULT_PORT  1883
#define MQTT_TLS_PORT      8883
#define MQTT_RECONNECT_MS  5000
#define MQTT_KEEPALIVE_S   60
#define MQTT_DEFAULT_TOPIC "hmsgws3"

// --- Factory Reset ------------------------------------------------------------
#define FACTORY_RESET_HOLD_MS  5000   // hold the BOOT button for this duration

// --- FreeRTOS Task Priorities -------------------------------------------------
#define TASK_PRIO_WIFI         5
#define TASK_PRIO_DTU          4
#define TASK_PRIO_GPIO         4
#define TASK_PRIO_MQTT         3
#define TASK_PRIO_WEBSERVER    3
#define TASK_PRIO_LED          2
#define TASK_PRIO_NEOPIXEL     TASK_PRIO_LED   // backward compatibility
#define TASK_PRIO_SERIAL       2
#define TASK_PRIO_SYSMONITOR   1

// --- FreeRTOS Stack Sizes (Bytes) ---------------------------------------------
#define STACK_WIFI          6144
#define STACK_DTU           8192
#define STACK_MQTT          6144
#define STACK_WEBSERVER     8192
#define STACK_GPIO          4096
#define STACK_LED           3072
#define STACK_NEOPIXEL      STACK_LED           // backward compatibility
#define STACK_SERIAL        4096
#define STACK_SYSMONITOR    3072

// --- FreeRTOS Core Assignment -------------------------------------------------
// Core 0: exclusively the WiFi stack (lwIP)
// Core 1: all user tasks
#define CORE_WIFI          1
#define CORE_DTU           1
#define CORE_MQTT          1
#define CORE_WEBSERVER     1
#define CORE_GPIO          1
#define CORE_LED           1
#define CORE_NEOPIXEL      CORE_LED             // backward compatibility
#define CORE_SERIAL        1
#define CORE_SYSMONITOR    1

// --- Log Levels ---------------------------------------------------------------
#define LOG_LEVEL_ERROR    0
#define LOG_LEVEL_WARN     1
#define LOG_LEVEL_INFO     2
#define LOG_LEVEL_DEBUG    3
#define LOG_LEVEL_DEFAULT  LOG_LEVEL_INFO

// --- Config File --------------------------------------------------------------
#define CONFIG_FILE        "/config.json"
