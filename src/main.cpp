// main.cpp — v2
// Start sequence per Spec §3.2:
//   dsInit → Serial → LittleFS → configLoad → systemStateEvents → tasks

#include <Arduino.h>
#include <LittleFS.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>

#include "config.h"
#include "systemState.h"
#include "appConfig.h"
#include "dataStore.h"
#include "logger.h"

#include "taskWiFi.h"
#include "taskLED.h"
#include "taskNeoPixel.h"    // shim → taskLED
#include "taskGPIO.h"
#include "taskSerial.h"
#include "taskSysMonitor.h"
#include "taskDTU.h"
#include "taskMQTT.h"
#include "taskWebServer.h"

// Defined here, declared extern in systemState.h
EventGroupHandle_t systemStateEvents;

void setup() {
    // ── 1. DataStore — must be first; tasks may start immediately after ────────
    dsInit();

    // ── 2. Serial ──────────────────────────────────────────────────────────────
    Serial.begin(SERIAL_BAUD);
    delay(300);
    logInit();
    LOG_I(MOD_SYS, "HMS-GW-S3 starting  fw=%s  build=%d  date=%s",
          FW_VERSION, BUILD_NUMBER, FW_DATE);
    LOG_I(MOD_SYS, "Chip: %s  rev=%d  cores=%d  freq=%d MHz  flash=%lu B",
          ESP.getChipModel(), ESP.getChipRevision(),
          ESP.getChipCores(), ESP.getCpuFreqMHz(),
          (unsigned long)ESP.getFlashChipSize());

    // ── 3. LittleFS ───────────────────────────────────────────────────────────
    if (!LittleFS.begin(true)) {
        LOG_E(MOD_SYS, "LittleFS mount failed — formatting");
        LittleFS.format();
        LittleFS.begin();
    }
    LOG_I(MOD_SYS, "LittleFS: total=%lu B  used=%lu B",
          (unsigned long)LittleFS.totalBytes(),
          (unsigned long)LittleFS.usedBytes());

    // ── 4. Config ─────────────────────────────────────────────────────────────
    configLoad();
    LOG_I(MOD_SYS, "Config loaded: ssid=%s  dtu=%s:%d  mqtt=%s:%d",
          appConfig.wifiSsid, appConfig.dtuHost, appConfig.dtuPort,
          appConfig.mqttHost, appConfig.mqttPort);

    // ── 5. FreeRTOS synchronisation ───────────────────────────────────────────
    systemStateEvents = xEventGroupCreate();
    configASSERT(systemStateEvents);

    // ── 6–12. Tasks (order per Spec §3.2) ────────────────────────────────────
    // taskWiFi — first: all other net tasks wait on EVT_WIFI_CONNECTED
    xTaskCreatePinnedToCore(taskWiFi,       "WiFi",       STACK_WIFI,
                            NULL, TASK_PRIO_WIFI,       NULL, CORE_WIFI);
    // taskLED — early: boot animation starts immediately
    xTaskCreatePinnedToCore(taskLED,        "LED",        STACK_LED,
                            NULL, TASK_PRIO_LED,        NULL, CORE_LED);
    // Low-latency I/O tasks
    xTaskCreatePinnedToCore(taskGPIO,       "GPIO",       STACK_GPIO,
                            NULL, TASK_PRIO_GPIO,       NULL, CORE_GPIO);
    xTaskCreatePinnedToCore(taskSerial,     "Serial",     STACK_SERIAL,
                            NULL, TASK_PRIO_SERIAL,     NULL, CORE_SERIAL);
    xTaskCreatePinnedToCore(taskSysMonitor, "SysMonitor", STACK_SYSMONITOR,
                            NULL, TASK_PRIO_SYSMONITOR, NULL, CORE_SYSMONITOR);
    // Network tasks — wait internally on EVT_WIFI_CONNECTED
    xTaskCreatePinnedToCore(taskDTU,        "DTU",        STACK_DTU,
                            NULL, TASK_PRIO_DTU,        NULL, CORE_DTU);
    xTaskCreatePinnedToCore(taskMQTT,       "MQTT",       STACK_MQTT,
                            NULL, TASK_PRIO_MQTT,       NULL, CORE_MQTT);
    xTaskCreatePinnedToCore(taskWebServer,  "WebServer",  STACK_WEBSERVER,
                            NULL, TASK_PRIO_WEBSERVER,  NULL, CORE_WEBSERVER);

    LOG_I(MOD_SYS, "All tasks started — %lu B heap free",
          (unsigned long)ESP.getFreeHeap());
}

void loop() {
    // Factory reset: any task sets EVT_FACTORY_RESET; we erase config and
    // let taskSerial (or taskGPIO long-press handler) trigger the reboot.
    static bool _factoryResetHandled = false;
    if (!_factoryResetHandled &&
        (xEventGroupGetBits(systemStateEvents) & EVT_FACTORY_RESET)) {
        _factoryResetHandled = true;
        LOG_W(MOD_SYS, "Factory reset — removing %s", CONFIG_FILE);
        LittleFS.remove(CONFIG_FILE);
        // Reboot is triggered by the task that set EVT_FACTORY_RESET (3s delay)
    }

    vTaskDelay(pdMS_TO_TICKS(500));
}
