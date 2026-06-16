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

// ─── Apply IO output ──────────────────────────────────────────────────────────
static void applyIo(int idx, bool state, DataStore::GpioState& gpio) {
    bool out = appConfig.io[idx].inverted ? !state : state;
    digitalWrite(appConfig.io[idx].pin, out ? HIGH : LOW);
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

    for (int i = 0; i < 3; i++) {
        switch (appConfig.io[i].mode) {
            case IO_OUTPUT:
                pinMode(appConfig.io[i].pin, OUTPUT);
                applyIo(i, false, gpio);
                break;
            case IO_INPUT:
                pinMode(appConfig.io[i].pin, appConfig.io[i].pullup ? INPUT_PULLUP : INPUT);
                gpio.gpio[i] = (digitalRead(appConfig.io[i].pin) == HIGH);
                if (appConfig.io[i].inverted) gpio.gpio[i] = !gpio.gpio[i];
                break;
            case IO_RESERVED:
                // Initialize as high-impedance input — siehe altFunction für vorgesehenen Zweck
                pinMode(appConfig.io[i].pin, INPUT);
                break;
        }
    }
    pinMode(BOOT_PIN, INPUT_PULLUP);

    dsSetGpio(gpio);

    LOG_I(MOD_GPIO, "GPIO ready — Relay=GPIO%d  IO1-3=GPIO%d,%d,%d",
          appConfig.relay.pin,
          appConfig.io[0].pin, appConfig.io[1].pin, appConfig.io[2].pin);

    // Debounce state for INPUT pins
    bool     lastRaw[3]      = {};
    uint32_t lastChangeMs[3] = {};
    for (int i = 0; i < 3; i++) lastRaw[i] = gpio.gpio[i];

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
                } else if (target >= 1 && target <= 3) {
                    int idx = target - 1;
                    if (appConfig.io[idx].mode == IO_OUTPUT) {
                        applyIo(idx, state, gpio);
                        LOG_I(MOD_GPIO, "io%d -> %s", target, state ? "HIGH" : "LOW");
                    } else {
                        LOG_W(MOD_GPIO, "io%d command ignored (not OUTPUT)", target);
                    }
                }
                dsSetGpio(gpio);
            }
        }

        // ── Debounce INPUT pins → update DataStore on change ─────────────────
        bool inputChanged = false;
        for (int i = 0; i < 3; i++) {
            if (appConfig.io[i].mode != IO_INPUT) continue;
            bool raw = (digitalRead(appConfig.io[i].pin) == HIGH);
            if (appConfig.io[i].inverted) raw = !raw;
            if (raw != lastRaw[i]) {
                lastRaw[i]      = raw;
                lastChangeMs[i] = millis();
            }
            if ((millis() - lastChangeMs[i]) >= DEBOUNCE_MS && raw != gpio.gpio[i]) {
                gpio.gpio[i] = raw;
                inputChanged  = true;
                LOG_D(MOD_GPIO, "io%d input -> %s", i + 1, raw ? "HIGH" : "LOW");
            }
        }
        if (inputChanged) dsSetGpio(gpio);

        // ── Factory reset check ───────────────────────────────────────────────
        checkFactoryReset();

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
