#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/event_groups.h>

#include "config.h"
#include "systemState.h"
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

// ─────────────────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(SERIAL_BAUD);
    delay(500);
    Serial.printf("\n\n[HMS-GW-S3] Firmware %s (%s) — Build %d\n",
                  FW_VERSION, FW_DATE, BUILD_NUMBER);

    // Create FreeRTOS primitives
    systemStateEvents = xEventGroupCreate();
    dtuDataQueue      = xQueueCreate(5,  sizeof(DtuData_t));
    gpioCommandQueue  = xQueueCreate(10, sizeof(GpioCommand_t));
    configMutex       = xSemaphoreCreateMutex();
    otaSemaphore      = xSemaphoreCreateBinary();
    xSemaphoreGive(otaSemaphore);

    // Start tasks
    // Core 0 — network-critical
    xTaskCreatePinnedToCore(taskDTU,       "taskDTU",       STACK_DTU,       NULL, TASK_PRIO_DTU,       NULL, CORE_DTU);
    xTaskCreatePinnedToCore(taskMQTT,      "taskMQTT",      STACK_MQTT,      NULL, TASK_PRIO_MQTT,      NULL, CORE_MQTT);

    // Core 1 — UI / output / GPIO
    xTaskCreatePinnedToCore(taskWebServer, "taskWebServer", STACK_WEBSERVER, NULL, TASK_PRIO_WEBSERVER, NULL, CORE_WEBSERVER);
    xTaskCreatePinnedToCore(taskNeoPixel,  "taskNeoPixel",  STACK_NEOPIXEL,  NULL, TASK_PRIO_NEOPIXEL,  NULL, CORE_NEOPIXEL);
    xTaskCreatePinnedToCore(taskGPIO,      "taskGPIO",      STACK_GPIO,      NULL, TASK_PRIO_GPIO,      NULL, CORE_GPIO);
    xTaskCreatePinnedToCore(taskSerial,    "taskSerial",    STACK_SERIAL,    NULL, TASK_PRIO_SERIAL,    NULL, CORE_SERIAL);
    xTaskCreatePinnedToCore(taskSysMonitor,"taskSysMonitor",STACK_SYSMONITOR,NULL, TASK_PRIO_SYSMONITOR,NULL, CORE_SYSMONITOR);
}

void loop() {
    // All logic runs in FreeRTOS tasks — loop intentionally empty
    vTaskDelay(pdMS_TO_TICKS(10000));
}
