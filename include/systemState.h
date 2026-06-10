#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>

// ─── System State Event Bits ──────────────────────────────────────────────────
#define EVT_WIFI_CONNECTED     (1 << 0)
#define EVT_WIFI_AP_MODE       (1 << 1)
#define EVT_DTU_ONLINE         (1 << 2)
#define EVT_MQTT_CONNECTED     (1 << 3)
#define EVT_OTA_RUNNING        (1 << 4)
#define EVT_ERROR              (1 << 5)
#define EVT_FACTORY_RESET      (1 << 6)
#define EVT_DATA_RECEIVED      (1 << 7)

extern EventGroupHandle_t systemStateEvents;

// ─── LED States ───────────────────────────────────────────────────────────────
typedef enum {
    LED_BOOT,
    LED_WIFI_CONNECTING,
    LED_AP_MODE,
    LED_DTU_OFFLINE,
    LED_NO_MQTT,
    LED_OPERATIONAL,
    LED_DATA_FLASH,
    LED_OTA,
    LED_ERROR,
    LED_FACTORY_RESET,
    LED_STANDBY
} LedState_t;

void setLedState(LedState_t state);

// ─── DTU Data Structure ───────────────────────────────────────────────────────
typedef struct {
    float grid_v, grid_i, grid_p, grid_dE, grid_tE;
    float pv0_v,  pv0_i,  pv0_p,  pv0_dE;
    float pv1_v,  pv1_i,  pv1_p,  pv1_dE;
    float temp;
    int   powerLimit;
    int   powerLimitSet;
    int   wifiRssi;
    int   dtuConnState;
    int   warningsActive;
    bool  inverterActive;
    uint32_t timestamp;
    uint32_t lastResponse;
} DtuData_t;

// ─── GPIO State Structure ─────────────────────────────────────────────────────
typedef struct {
    bool relay;
    bool gpio[4];
} GpioState_t;

// ─── GPIO Command ─────────────────────────────────────────────────────────────
typedef struct {
    int  target;   // 0=relay, 1-4=gpio
    bool state;
} GpioCommand_t;

// ─── Shared Queues / Handles ──────────────────────────────────────────────────
extern QueueHandle_t dtuDataQueue;
extern QueueHandle_t gpioCommandQueue;
extern SemaphoreHandle_t configMutex;
extern SemaphoreHandle_t otaSemaphore;
