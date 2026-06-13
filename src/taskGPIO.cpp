#include "taskGPIO.h"
#include "config.h"
#include "appConfig.h"
#include "systemState.h"
#include "logger.h"
#include <Arduino.h>
#include <LittleFS.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/event_groups.h>

GpioState_t gpioState = {};

static void applyRelay(bool state) {
    bool out = appConfig.relay.inverted ? !state : state;
    digitalWrite(appConfig.relay.pin, out ? HIGH : LOW);
    gpioState.relay = state;
}

static void applyGpio(int idx, bool state) {
    if (!appConfig.gp[idx].isOutput) return;
    bool out = appConfig.gp[idx].inverted ? !state : state;
    digitalWrite(appConfig.gp[idx].pin, out ? HIGH : LOW);
    gpioState.gpio[idx] = state;
}

// ─── Factory reset via long BOOT press ───────────────────────────────────────
static void checkFactoryReset() {
    if (digitalRead(BOOT_PIN) != LOW) return;
    uint32_t pressStart = millis();
    while (digitalRead(BOOT_PIN) == LOW) {
        vTaskDelay(pdMS_TO_TICKS(100));
        if ((millis() - pressStart) >= FACTORY_RESET_HOLD_MS) {
            LOG_W(MOD_GPIO, "Factory reset triggered!");
            xEventGroupSetBits(systemStateEvents, EVT_FACTORY_RESET);
            setLedState(LED_FACTORY_RESET);
            vTaskDelay(pdMS_TO_TICKS(1500));
            LittleFS.remove(CONFIG_FILE);
            ESP.restart();
        }
    }
}

void taskGPIO(void* pvParameters) {
    // ── Wait for config to be loaded by taskWebServer ─────────────────────
    // taskWebServer sets EVT_WIFI_CONNECTED or EVT_WIFI_AP_MODE after config load
    LOG_I(MOD_GPIO, "GPIO task waiting for config...");
    xEventGroupWaitBits(systemStateEvents,
                        EVT_WIFI_CONNECTED | EVT_WIFI_AP_MODE,
                        pdFALSE, pdFALSE,          // wait for ANY bit
                        pdMS_TO_TICKS(15000));      // max 15 s

    // ── Pin setup ─────────────────────────────────────────────────────────
    pinMode(appConfig.relay.pin, OUTPUT);
    applyRelay(false);

    for (int i = 0; i < 4; i++) {
        if (appConfig.gp[i].isOutput) {
            pinMode(appConfig.gp[i].pin, OUTPUT);
            applyGpio(i, false);
        } else {
            pinMode(appConfig.gp[i].pin,
                    appConfig.gp[i].pullup ? INPUT_PULLUP : INPUT);
            gpioState.gpio[i] = (digitalRead(appConfig.gp[i].pin) == HIGH);
        }
    }
    pinMode(BOOT_PIN, INPUT_PULLUP);

    LOG_I(MOD_GPIO, "GPIO ready. Relay=GPIO%d  GP1-4=GPIO%d,%d,%d,%d",
          appConfig.relay.pin,
          appConfig.gp[0].pin, appConfig.gp[1].pin,
          appConfig.gp[2].pin, appConfig.gp[3].pin);

    // Debounce state
    bool     lastGpioState[4];
    uint32_t lastChangeTime[4] = {0,0,0,0};
    const uint32_t DEBOUNCE_MS = 50;
    for (int i = 0; i < 4; i++) lastGpioState[i] = gpioState.gpio[i];

    for (;;) {
        // ── Process commands ──────────────────────────────────────────────
        GpioCommand_t cmd;
        while (xQueueReceive(gpioCommandQueue, &cmd, 0) == pdTRUE) {
            if (cmd.target == 0) {
                applyRelay(cmd.state);
                LOG_I(MOD_GPIO, "Relay -> %s", cmd.state ? "ON" : "OFF");
            } else if (cmd.target >= 1 && cmd.target <= 4) {
                int idx = cmd.target - 1;
                if (appConfig.gp[idx].isOutput) {
                    applyGpio(idx, cmd.state);
                    LOG_I(MOD_GPIO, "GP%d -> %s", cmd.target, cmd.state ? "HIGH" : "LOW");
                }
            }
        }

        // ── Debounce inputs ───────────────────────────────────────────────
        for (int i = 0; i < 4; i++) {
            if (appConfig.gp[i].isOutput) continue;
            bool raw = (digitalRead(appConfig.gp[i].pin) == HIGH);
            if (appConfig.gp[i].inverted) raw = !raw;
            if (raw != lastGpioState[i]) {
                lastGpioState[i] = raw;
                lastChangeTime[i] = millis();
            }
            if ((millis() - lastChangeTime[i]) >= DEBOUNCE_MS && raw != gpioState.gpio[i]) {
                gpioState.gpio[i] = raw;
                LOG_D(MOD_GPIO, "GP%d input -> %s", i+1, raw ? "HIGH" : "LOW");
            }
        }

        // ── Factory reset check ───────────────────────────────────────────
        checkFactoryReset();

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void gpioSetRelay(bool state) {
    GpioCommand_t cmd = {0, state};
    xQueueSend(gpioCommandQueue, &cmd, pdMS_TO_TICKS(100));
}

void gpioSetPin(int index, bool state) {
    GpioCommand_t cmd = {index + 1, state};
    xQueueSend(gpioCommandQueue, &cmd, pdMS_TO_TICKS(100));
}

bool gpioGetPin(int index)  { return gpioState.gpio[index]; }
bool gpioGetRelay()         { return gpioState.relay; }
