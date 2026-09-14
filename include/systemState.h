#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>

// --- System EventGroup Bits (Spec §3.3) --------------------------------------
// WiFi connectivity state
#define EVT_WIFI_CONNECTED    BIT0
#define EVT_WIFI_AP_MODE      BIT1
#define EVT_WIFI_FORCE_AP     BIT2

// Peripheral / service connectivity
#define EVT_DTU_ONLINE        BIT3
#define EVT_MQTT_CONNECTED    BIT4
#define EVT_DATA_RECEIVED     BIT5

// System operations
#define EVT_OTA_RUNNING       BIT6
#define EVT_REBOOT            BIT7
#define EVT_FACTORY_RESET     BIT8

extern EventGroupHandle_t systemStateEvents;

// --- LED States (Spec §7) -----------------------------------------------------
typedef enum {
    LED_BOOT = 0,
    LED_WIFI_CONNECTING,
    LED_AP_MODE,
    LED_DTU_OFFLINE,
    LED_NO_MQTT,
    LED_OPERATIONAL,
    LED_STANDBY,
    LED_DATA_FLASH,
    LED_OTA,
    LED_ERROR,
    LED_FACTORY_RESET
} LedState_t;

// setLedState() is declared in taskLED.h  -  include that header directly
