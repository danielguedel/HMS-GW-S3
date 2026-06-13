#include "taskSysMonitor.h"
#include "config.h"
#include "appConfig.h"
#include "systemState.h"
#include "logger.h"
#include "taskNeoPixel.h"
#include <Arduino.h>
#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/event_groups.h>

// NOTE: Hardware WDT (esp_task_wdt) intentionally NOT used here.
// It requires all tasks to register and reset it within the timeout,
// which causes panic reboots during startup. Use software monitoring instead.

#define HEAP_WARN_THRESHOLD 20000   // warn if free heap < 20 KB
#define SYSMON_INTERVAL_MS  10000   // check every 10 s

void taskSysMonitor(void* pvParameters) {
    LOG_I(MOD_SYS, "SysMonitor started. HeapWarn=%dB", HEAP_WARN_THRESHOLD);

    LedState_t lastLedState = LED_BOOT;
    uint32_t   lastLog      = 0;

    for (;;) {
        EventBits_t bits = xEventGroupGetBits(systemStateEvents);

        // ── Determine correct LED state ────────────────────────────────────
        LedState_t newState;
        if      (bits & EVT_OTA_RUNNING)                              newState = LED_OTA;
        else if (bits & EVT_ERROR)                                    newState = LED_ERROR;
        else if (bits & EVT_FACTORY_RESET)                            newState = LED_FACTORY_RESET;
        else if (bits & EVT_WIFI_AP_MODE)                             newState = LED_AP_MODE;
        else if (!(bits & EVT_WIFI_CONNECTED))                        newState = LED_WIFI_CONNECTING;
        else if (!(bits & EVT_DTU_ONLINE))                            newState = LED_DTU_OFFLINE;
        else if (!(bits & EVT_MQTT_CONNECTED) &&
                 strlen(appConfig.mqttHost) > 0)                      newState = LED_NO_MQTT;
        else {
            bool standby = false;
            if (xSemaphoreTake(configMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                standby = dtuDataValid && (latestDtuData.grid_p < 1.0f);
                xSemaphoreGive(configMutex);
            }
            newState = standby ? LED_STANDBY : LED_OPERATIONAL;
        }

        if (newState != lastLedState) {
            setLedState(newState);
            lastLedState = newState;
        }

        // ── Periodic heap / WiFi logging ──────────────────────────────────
        uint32_t now = millis();
        if (now - lastLog >= SYSMON_INTERVAL_MS) {
            lastLog = now;
            uint32_t heap = ESP.getFreeHeap();
            LOG_D(MOD_SYS, "Heap: %lu B  WiFi: %d dBm  Uptime: %lu s",
                  (unsigned long)heap, (int)WiFi.RSSI(),
                  (unsigned long)(now / 1000));
            if (heap < HEAP_WARN_THRESHOLD)
                LOG_W(MOD_SYS, "Low heap: %lu B!", (unsigned long)heap);
        }

        vTaskDelay(pdMS_TO_TICKS(500));
    }
}
