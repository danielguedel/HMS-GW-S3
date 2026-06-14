// taskGPIO.cpp — v2 (DataStore pattern)

#include "taskGPIO.h"
#include "dataStore.h"
#include "appConfig.h"
#include "systemState.h"
#include "taskLED.h"
#include "config.h"
#include "logger.h"
#include <Arduino.h>
#include <LittleFS.h>

static const uint32_t DEBOUNCE_MS = 50;

// ─── Apply relay ──────────────────────────────────────────────────────────────
static void applyRelay(bool state, DataStore::GpioState& gpio) {
    bool out = appConfig.relay.inverted ? !state : state;
    digitalWrite(appConfig.relay.pin, out ? HIGH : LOW);
    gpio.relay = state;
}

// ─── Apply GP output ──────────────────────────────────────────────────────────
static void applyGp(int idx, bool state, DataStore::GpioState& gpio) {
    bool out = appConfig.gp[idx].inverted ? !state : state;
    digitalWrite(appConfig.gp[idx].pin, out ? HIGH : LOW);
    gpio.gpio[idx] = state;
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

// ─── Task ─────────────────────────────────────────────────────────────────────
void taskGPIO(void* pvParameters) {
    // Config is loaded in main.cpp before tasks start — no wait needed here

    // ── Pin setup ─────────────────────────────────────────────────────────────
    DataStore::GpioState gpio = {};

    pinMode(appConfig.relay.pin, OUTPUT);
    applyRelay(false, gpio);

    for (int i = 0; i < 4; i++) {
        switch (appConfig.gp[i].mode) {
            case GP_OUTPUT:
                pinMode(appConfig.gp[i].pin, OUTPUT);
                applyGp(i, false, gpio);
                break;
            case GP_INPUT:
                pinMode(appConfig.gp[i].pin, appConfig.gp[i].pullup ? INPUT_PULLUP : INPUT);
                gpio.gpio[i] = (digitalRead(appConfig.gp[i].pin) == HIGH);
                if (appConfig.gp[i].inverted) gpio.gpio[i] = !gpio.gpio[i];
                break;
            case GP_I2C_RESERVED:
                // Initialize as high-impedance input — I2C init in future version
                pinMode(appConfig.gp[i].pin, INPUT);
                break;
        }
    }
    pinMode(BOOT_PIN, INPUT_PULLUP);

    dsSetGpio(gpio);

    LOG_I(MOD_GPIO, "GPIO ready — Relay=GPIO%d  GP1-4=GPIO%d,%d,%d,%d",
          appConfig.relay.pin,
          appConfig.gp[0].pin, appConfig.gp[1].pin,
          appConfig.gp[2].pin, appConfig.gp[3].pin);

    // Debounce state for INPUT pins
    bool     lastRaw[4]      = {};
    uint32_t lastChangeMs[4] = {};
    for (int i = 0; i < 4; i++) lastRaw[i] = gpio.gpio[i];

    for (;;) {
        // ── Process pending GPIO command from DataStore ────────────────────────
        {
            xSemaphoreTake(ds.mutex, portMAX_DELAY);
            bool pending = ds.gpioCmd.pending;
            int  target  = ds.gpioCmd.target;
            bool state   = ds.gpioCmd.state;
            if (pending) ds.gpioCmd.pending = false;
            xSemaphoreGive(ds.mutex);

            if (pending) {
                if (target == 0) {
                    applyRelay(state, gpio);
                    LOG_I(MOD_GPIO, "Relay -> %s", state ? "ON" : "OFF");
                } else if (target >= 1 && target <= 4) {
                    int idx = target - 1;
                    if (appConfig.gp[idx].mode == GP_OUTPUT) {
                        applyGp(idx, state, gpio);
                        LOG_I(MOD_GPIO, "GP%d -> %s", target, state ? "HIGH" : "LOW");
                    } else {
                        LOG_W(MOD_GPIO, "GP%d command ignored (not OUTPUT)", target);
                    }
                }
                dsSetGpio(gpio);
            }
        }

        // ── Debounce INPUT pins → update DataStore on change ─────────────────
        bool inputChanged = false;
        for (int i = 0; i < 4; i++) {
            if (appConfig.gp[i].mode != GP_INPUT) continue;
            bool raw = (digitalRead(appConfig.gp[i].pin) == HIGH);
            if (appConfig.gp[i].inverted) raw = !raw;
            if (raw != lastRaw[i]) {
                lastRaw[i]      = raw;
                lastChangeMs[i] = millis();
            }
            if ((millis() - lastChangeMs[i]) >= DEBOUNCE_MS && raw != gpio.gpio[i]) {
                gpio.gpio[i] = raw;
                inputChanged  = true;
                LOG_D(MOD_GPIO, "GP%d input -> %s", i + 1, raw ? "HIGH" : "LOW");
            }
        }
        if (inputChanged) dsSetGpio(gpio);

        // ── Factory reset check ───────────────────────────────────────────────
        checkFactoryReset();

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
