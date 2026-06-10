#include "taskSerial.h"
#include "config.h"
#include "appConfig.h"
#include "systemState.h"
#include "logger.h"
#include "taskNeoPixel.h"
#include "taskDTU.h"
#include "taskGPIO.h"
#include <Arduino.h>
#include <WiFi.h>
#include <LittleFS.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static char   _lineBuf[128];
static int    _linePos = 0;

static void printHelp() {
    Serial.println("\n── HMS-GW-S3 Serial Console ─────────────────────────");
    Serial.println("  help                    This help");
    Serial.println("  status                  System status overview");
    Serial.println("  data                    Latest inverter data");
    Serial.println("  sysinfo                 Heap, tasks, uptime");
    Serial.println("  setPower <W>            Set power limit (W)");
    Serial.println("  setRelay <0|1>          Switch relay");
    Serial.println("  setGPIO <1-4> <0|1>     Set GPIO output");
    Serial.println("  getGPIO <1-4>           Read GPIO state");
    Serial.println("  setWifi <0|1>           Enable/disable WiFi");
    Serial.println("  rebootDTU 1             Request DTU reboot");
    Serial.println("  rebootInverter 1        Request inverter reboot");
    Serial.println("  reboot                  Restart gateway");
    Serial.println("  resetToFactory 1        Factory reset (deletes config)");
    Serial.println("  setInterval <s>         Set DTU poll interval (min 31)");
    Serial.println("  setLogLevel <0-3>       Set log level (0=ERR..3=DBG)");
    Serial.println("  ledTest                 Cycle through all LED states");
    Serial.println("  protectSettings <0|1>   Lock/unlock web config");
    Serial.println("─────────────────────────────────────────────────────\n");
}

static void printStatus() {
    EventBits_t bits = xEventGroupGetBits(systemStateEvents);
    Serial.println("\n── System Status ────────────────────────────────────");
    Serial.printf("  Firmware:     %s (Build %d, %s)\n", FW_VERSION, BUILD_NUMBER, FW_DATE);
    Serial.printf("  Uptime:       %lu s\n",   (unsigned long)(millis() / 1000));
    Serial.printf("  Free Heap:    %lu B\n",   (unsigned long)ESP.getFreeHeap());
    Serial.printf("  WiFi:         %s  SSID: %s  RSSI: %d dBm\n",
                  (bits & EVT_WIFI_CONNECTED) ? "CONNECTED" : "DISCONNECTED",
                  WiFi.SSID().c_str(), WiFi.RSSI());
    Serial.printf("  IP:           %s\n",      WiFi.localIP().toString().c_str());
    Serial.printf("  DTU:          %s (%s:%d)\n",
                  (bits & EVT_DTU_ONLINE) ? "ONLINE" : "OFFLINE",
                  appConfig.dtuHost, appConfig.dtuPort);
    Serial.printf("  MQTT:         %s (%s:%d)\n",
                  (bits & EVT_MQTT_CONNECTED) ? "CONNECTED" : "DISCONNECTED",
                  appConfig.mqttHost, appConfig.mqttPort);
    Serial.printf("  Relay:        %s\n",      gpioState.relay ? "ON" : "OFF");
    Serial.printf("  GPIO 1-4:     %d %d %d %d  (mode: %s %s %s %s)\n",
                  gpioState.gpio[0], gpioState.gpio[1],
                  gpioState.gpio[2], gpioState.gpio[3],
                  appConfig.gp[0].isOutput ? "OUT" : "IN",
                  appConfig.gp[1].isOutput ? "OUT" : "IN",
                  appConfig.gp[2].isOutput ? "OUT" : "IN",
                  appConfig.gp[3].isOutput ? "OUT" : "IN");
    Serial.println("─────────────────────────────────────────────────────\n");
}

static void printData() {
    if (!dtuDataValid) {
        Serial.println("[DATA] No valid data yet");
        return;
    }
    DtuData_t d;
    if (xSemaphoreTake(configMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        memcpy(&d, &latestDtuData, sizeof(DtuData_t));
        xSemaphoreGive(configMutex);
    } else return;

    Serial.println("\n── Inverter Data ────────────────────────────────────");
    Serial.printf("  Model:        %s  Serial: %s\n", d.inverterModel, d.inverterSerial);
    Serial.printf("  PV1:          %.1fV / %.2fA / %.0fW  Today: %.3f kWh\n",
                  d.pv0_v, d.pv0_i, d.pv0_p, d.pv0_dE);
    Serial.printf("  PV2:          %.1fV / %.2fA / %.0fW  Today: %.3f kWh\n",
                  d.pv1_v, d.pv1_i, d.pv1_p, d.pv1_dE);
    Serial.printf("  Grid:         %.1fV / %.2fA / %.0fW\n",
                  d.grid_v, d.grid_i, d.grid_p);
    Serial.printf("  Energy:       Today: %.3f kWh  Total: %.3f kWh\n",
                  d.grid_dE, d.grid_tE);
    Serial.printf("  Temperature:  %.1f°C\n", d.temp);
    Serial.printf("  Power Limit:  %d%%\n",   d.powerLimit);
    Serial.printf("  DTU RSSI:     %d%%\n",   d.wifiRssi);
    Serial.printf("  Warnings:     %d\n",     d.warningsActive);
    Serial.printf("  Last Update:  %lu\n",    (unsigned long)d.lastResponse);
    Serial.println("─────────────────────────────────────────────────────\n");
}

static void printSysInfo() {
    Serial.println("\n── System Info ──────────────────────────────────────");
    Serial.printf("  Free Heap:    %lu B\n",  (unsigned long)ESP.getFreeHeap());
    Serial.printf("  Min Heap:     %lu B\n",  (unsigned long)ESP.getMinFreeHeap());
    Serial.printf("  Chip Rev:     %d\n",     ESP.getChipRevision());
    Serial.printf("  CPU Freq:     %d MHz\n", ESP.getCpuFreqMHz());
    Serial.printf("  Flash Size:   %lu B\n",  (unsigned long)ESP.getFlashChipSize());
    Serial.printf("  Uptime:       %lu s\n",  (unsigned long)(millis() / 1000));

    // FreeRTOS task list
    char taskBuf[800];
    vTaskList(taskBuf);
    Serial.println("  Tasks (Name / State / Prio / Stack / Core):");
    Serial.println(taskBuf);
    Serial.println("─────────────────────────────────────────────────────\n");
}

static void ledTestSequence() {
    const LedState_t states[] = {
        LED_BOOT, LED_WIFI_CONNECTING, LED_AP_MODE, LED_DTU_OFFLINE,
        LED_NO_MQTT, LED_OPERATIONAL, LED_DATA_FLASH, LED_OTA,
        LED_ERROR, LED_STANDBY
    };
    const char* names[] = {
        "BOOT","WIFI_CONNECTING","AP_MODE","DTU_OFFLINE",
        "NO_MQTT","OPERATIONAL","DATA_FLASH","OTA",
        "ERROR","STANDBY"
    };
    Serial.println("[LED] Starting LED test sequence...");
    for (int i = 0; i < 10; i++) {
        Serial.printf("[LED] State: %s\n", names[i]);
        setLedState(states[i]);
        vTaskDelay(pdMS_TO_TICKS(2500));
    }
    setLedState(LED_OPERATIONAL);
    Serial.println("[LED] Test done.");
}

// ─── Command parser ───────────────────────────────────────────────────────────
static void processCommand(const char* line) {
    char cmd[32] = {};
    char arg1[32] = {};
    char arg2[32] = {};
    sscanf(line, "%31s %31s %31s", cmd, arg1, arg2);

    if (strcmp(cmd, "help") == 0) {
        printHelp();
    } else if (strcmp(cmd, "status") == 0) {
        printStatus();
    } else if (strcmp(cmd, "data") == 0) {
        printData();
    } else if (strcmp(cmd, "sysinfo") == 0) {
        printSysInfo();
    } else if (strcmp(cmd, "reboot") == 0) {
        Serial.println("[SYS] Rebooting...");
        vTaskDelay(pdMS_TO_TICKS(300));
        ESP.restart();
    } else if (strcmp(cmd, "resetToFactory") == 0 && strcmp(arg1,"1")==0) {
        Serial.println("[SYS] Factory reset! Deleting config and rebooting...");
        LittleFS.remove(CONFIG_FILE);
        vTaskDelay(pdMS_TO_TICKS(300));
        ESP.restart();
    } else if (strcmp(cmd, "setPower") == 0) {
        int w = atoi(arg1);
        dtuSetPowerLimit(w);
        Serial.printf("[DTU] Power limit set to %d\n", w);
    } else if (strcmp(cmd, "setRelay") == 0) {
        bool s = (atoi(arg1) == 1);
        gpioSetRelay(s);
        Serial.printf("[GPIO] Relay -> %s\n", s ? "ON" : "OFF");
    } else if (strcmp(cmd, "setGPIO") == 0) {
        int idx = atoi(arg1) - 1;
        bool s  = (atoi(arg2) == 1);
        if (idx >= 0 && idx < 4) {
            gpioSetPin(idx, s);
            Serial.printf("[GPIO] GP%d -> %s\n", idx+1, s ? "HIGH" : "LOW");
        } else {
            Serial.println("[GPIO] Invalid GPIO number (1-4)");
        }
    } else if (strcmp(cmd, "getGPIO") == 0) {
        int idx = atoi(arg1) - 1;
        if (idx >= 0 && idx < 4) {
            Serial.printf("[GPIO] GP%d = %s\n", idx+1,
                          gpioGetPin(idx) ? "HIGH" : "LOW");
        }
    } else if (strcmp(cmd, "rebootDTU") == 0 && strcmp(arg1,"1")==0) {
        dtuRequestReboot();
        Serial.println("[DTU] Reboot requested");
    } else if (strcmp(cmd, "rebootInverter") == 0 && strcmp(arg1,"1")==0) {
        dtuRequestInverterReboot();
        Serial.println("[DTU] Inverter reboot requested");
    } else if (strcmp(cmd, "setInterval") == 0) {
        int s = atoi(arg1);
        if (s < DTU_MIN_INTERVAL) s = DTU_MIN_INTERVAL;
        if (xSemaphoreTake(configMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
            appConfig.dtuInterval = s;
            xSemaphoreGive(configMutex);
        }
        configSave();
        Serial.printf("[DTU] Interval set to %d s\n", s);
    } else if (strcmp(cmd, "setLogLevel") == 0) {
        int l = atoi(arg1);
        if (l >= 0 && l <= 3) {
            appConfig.logLevel = l;
            Serial.printf("[SYS] Log level -> %d\n", l);
        }
    } else if (strcmp(cmd, "setWifi") == 0) {
        if (atoi(arg1) == 0) { WiFi.disconnect(); Serial.println("[WIFI] Disabled"); }
        else                 { WiFi.reconnect();   Serial.println("[WIFI] Reconnecting"); }
    } else if (strcmp(cmd, "protectSettings") == 0) {
        bool p = (atoi(arg1) == 1);
        appConfig.protectSettings = p;
        configSave();
        Serial.printf("[SYS] protectSettings -> %s\n", p ? "ON" : "OFF");
    } else if (strcmp(cmd, "ledTest") == 0) {
        ledTestSequence();
    } else if (strlen(cmd) > 0) {
        Serial.printf("[SYS] Unknown command: '%s'  (type 'help')\n", cmd);
    }
}

// ─── Task ─────────────────────────────────────────────────────────────────────
void taskSerial(void* pvParameters) {
    Serial.println("\n[SYS] Serial console ready. Type 'help' for commands.");

    for (;;) {
        while (Serial.available()) {
            char c = Serial.read();
            if (c == '\r') continue;
            if (c == '\n') {
                _lineBuf[_linePos] = '\0';
                if (_linePos > 0) {
                    processCommand(_lineBuf);
                }
                _linePos = 0;
            } else if (_linePos < (int)sizeof(_lineBuf) - 1) {
                _lineBuf[_linePos++] = c;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
