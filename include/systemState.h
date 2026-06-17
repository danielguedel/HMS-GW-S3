#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>

// --- System EventGroup Bits (Spec §3.3) --------------------------------------
#define EVT_WIFI_CONNECTED    BIT0
#define EVT_WIFI_AP_MODE      BIT1
#define EVT_DTU_ONLINE        BIT2
#define EVT_MQTT_CONNECTED    BIT3
#define EVT_DATA_RECEIVED     BIT4
#define EVT_OTA_RUNNING       BIT5
#define EVT_FACTORY_RESET     BIT6
#define EVT_REBOOT            BIT7

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
