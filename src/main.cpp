#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/event_groups.h>

#include "config.h"
#include "systemState.h"
#include "appConfig.h"
#include "taskDTU.h"
#include "taskMQTT.h"
#include "taskWebServer.h"
#include "taskNeoPixel.h"
#include "taskGPIO.h"
#include "taskSerial.h"
#include "taskSysMonitor.h"

// ─── Global FreeRTOS Objects ──────────────────────────────────────────────────
EventGroupHandle_t systemStateEvents;
QueueHandle_t      dtuDataQueue;
QueueHandle_t      gpioCommandQueue;
SemaphoreHandle_t  configMutex;
SemaphoreHandle_t  otaSemaphore;

// ─── Global shared state ──────────────────────────────────────────────────────
DtuData_t  latestDtuData = {};
bool       dtuDataValid  = false;
GpioState_t gpioState    = {};
AppConfig  appConfig;

void setup() {
    Serial.begin(SERIAL_BAUD);
    delay(500);

    // FreeRTOS primitives
    systemStateEvents = xEventGroupCreate();
    dtuDataQueue      = xQueueCreate(5,  sizeof(DtuData_t));
    gpioCommandQueue  = xQueueCreate(10, sizeof(GpioCommand_t));
    configMutex       = xSemaphoreCreateMutex();
    otaSemaphore      = xSemaphoreCreateBinary();
    xSemaphoreGive(otaSemaphore);

    // Boot LED immediately (before tasks start)
    // FastLED needs the task context, so we call it from the NeoPixel task.
    // Raw write for boot indication:
    Serial.printf("\n\n[HMS-GW-S3] Starting v%s (Build %d)\n", FW_VERSION, BUILD_NUMBER);

    // ── Core 1: WebServer starts first — loads config, starts WiFi ───────
    xTaskCreatePinnedToCore(taskWebServer,  "WebServer",  STACK_WEBSERVER,  NULL,
                            TASK_PRIO_WEBSERVER,  NULL, CORE_WEBSERVER);

    // ── Core 1: NeoPixel — needs to run immediately for boot animation ───
    xTaskCreatePinnedToCore(taskNeoPixel,   "NeoPixel",   STACK_NEOPIXEL,   NULL,
                            TASK_PRIO_NEOPIXEL,   NULL, CORE_NEOPIXEL);

    // ── Core 1: GPIO & Serial & SysMonitor ───────────────────────────────
    xTaskCreatePinnedToCore(taskGPIO,       "GPIO",       STACK_GPIO,       NULL,
                            TASK_PRIO_GPIO,       NULL, CORE_GPIO);
    xTaskCreatePinnedToCore(taskSerial,     "Serial",     STACK_SERIAL,     NULL,
                            TASK_PRIO_SERIAL,     NULL, CORE_SERIAL);
    xTaskCreatePinnedToCore(taskSysMonitor, "SysMonitor", STACK_SYSMONITOR, NULL,
                            TASK_PRIO_SYSMONITOR, NULL, CORE_SYSMONITOR);

    // ── Core 0: Network tasks (start after WebServer sets up WiFi) ───────
    xTaskCreatePinnedToCore(taskDTU,        "DTU",        STACK_DTU,        NULL,
                            TASK_PRIO_DTU,        NULL, CORE_DTU);
    xTaskCreatePinnedToCore(taskMQTT,       "MQTT",       STACK_MQTT,       NULL,
                            TASK_PRIO_MQTT,       NULL, CORE_MQTT);
}

void loop() {
    // All logic in FreeRTOS tasks — loop intentionally empty
    vTaskDelay(pdMS_TO_TICKS(10000));
}
