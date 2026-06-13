#include "taskNeoPixel.h"
#include "config.h"
#include "appConfig.h"
#include "logger.h"
#include <FastLED.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// ─── LED array ────────────────────────────────────────────────────────────────
static CRGB leds[NEOPIXEL_COUNT];

// ─── Current state (set from any task via setLedState) ───────────────────────
static volatile LedState_t _currentState = LED_BOOT;
static volatile LedState_t _pendingState = LED_BOOT;
static volatile bool       _stateChanged = false;

void setLedState(LedState_t state) {
    _pendingState = state;
    _stateChanged = true;
}

// ─── Colour helpers ───────────────────────────────────────────────────────────
static inline CRGB scaled(CRGB c) {
    uint8_t b = appConfig.ledBrightness;
    return CRGB(
        (uint16_t)c.r * b / 255,
        (uint16_t)c.g * b / 255,
        (uint16_t)c.b * b / 255
    );
}

static void setColor(CRGB c) {
    leds[0] = scaled(c);
    FastLED.show();
}

static void off() {
    leds[0] = CRGB::Black;
    FastLED.show();
}

// ─── Animation primitives ─────────────────────────────────────────────────────
// Single blink: on for onMs, off for offMs, repeated count times
static bool blinkN(CRGB c, uint32_t onMs, uint32_t offMs, int count) {
    for (int i = 0; i < count; i++) {
        if (_stateChanged) return true;  // abort early on state change
        setColor(c);
        vTaskDelay(pdMS_TO_TICKS(onMs));
        if (_stateChanged) return true;
        off();
        vTaskDelay(pdMS_TO_TICKS(offMs));
    }
    return false;
}

// Double-blink pattern: blink-blink-pause
static bool doubleBlink(CRGB c, uint32_t pulseMs, uint32_t pauseMs) {
    if (_stateChanged) return true;
    setColor(c); vTaskDelay(pdMS_TO_TICKS(pulseMs));
    if (_stateChanged) return true;
    off();        vTaskDelay(pdMS_TO_TICKS(pulseMs));
    if (_stateChanged) return true;
    setColor(c); vTaskDelay(pdMS_TO_TICKS(pulseMs));
    if (_stateChanged) return true;
    off();        vTaskDelay(pdMS_TO_TICKS(pauseMs));
    return false;
}

// Smooth pulse (brightness 0→max→0 over periodMs)
static bool pulse(CRGB baseColor, uint32_t periodMs) {
    uint32_t steps = 40;
    uint32_t stepMs = periodMs / (2 * steps);
    for (uint32_t i = 0; i < steps; i++) {
        if (_stateChanged) return true;
        uint8_t bright = (uint8_t)(255 * i / steps);
        uint8_t scale  = (uint16_t)bright * appConfig.ledBrightness / 255;
        leds[0] = CRGB(
            (uint16_t)baseColor.r * scale / 255,
            (uint16_t)baseColor.g * scale / 255,
            (uint16_t)baseColor.b * scale / 255
        );
        FastLED.show();
        vTaskDelay(pdMS_TO_TICKS(stepMs));
    }
    for (uint32_t i = steps; i > 0; i--) {
        if (_stateChanged) return true;
        uint8_t bright = (uint8_t)(255 * i / steps);
        uint8_t scale  = (uint16_t)bright * appConfig.ledBrightness / 255;
        leds[0] = CRGB(
            (uint16_t)baseColor.r * scale / 255,
            (uint16_t)baseColor.g * scale / 255,
            (uint16_t)baseColor.b * scale / 255
        );
        FastLED.show();
        vTaskDelay(pdMS_TO_TICKS(stepMs));
    }
    return false;
}

// Heartbeat: short double-flash then long dark
static bool heartbeat(CRGB c, uint32_t periodMs) {
    if (_stateChanged) return true;
    setColor(c); vTaskDelay(pdMS_TO_TICKS(80));
    if (_stateChanged) return true;
    off();        vTaskDelay(pdMS_TO_TICKS(80));
    if (_stateChanged) return true;
    setColor(c); vTaskDelay(pdMS_TO_TICKS(80));
    if (_stateChanged) return true;
    off();        vTaskDelay(pdMS_TO_TICKS(periodMs - 240));
    return false;
}

// SOS: · · · − − − · · ·
static bool sos(CRGB c) {
    auto dot  = [&]() -> bool {
        if (_stateChanged) return true;
        setColor(c); vTaskDelay(pdMS_TO_TICKS(150));
        off();        vTaskDelay(pdMS_TO_TICKS(150));
        return false;
    };
    auto dash = [&]() -> bool {
        if (_stateChanged) return true;
        setColor(c); vTaskDelay(pdMS_TO_TICKS(450));
        off();        vTaskDelay(pdMS_TO_TICKS(150));
        return false;
    };
    for (int i = 0; i < 3; i++) if (dot())  return true;
    vTaskDelay(pdMS_TO_TICKS(150));
    for (int i = 0; i < 3; i++) if (dash()) return true;
    vTaskDelay(pdMS_TO_TICKS(150));
    for (int i = 0; i < 3; i++) if (dot())  return true;
    vTaskDelay(pdMS_TO_TICKS(2000));
    return false;
}

// ─── Task ─────────────────────────────────────────────────────────────────────
void taskNeoPixel(void* pvParameters) {
    FastLED.addLeds<WS2812B, NEOPIXEL_PIN, GRB>(leds, NEOPIXEL_COUNT);
    // Use safe default brightness — appConfig may not be loaded yet at this point
    uint8_t initBright = NEOPIXEL_BRIGHTNESS_DEF > 0 ? NEOPIXEL_BRIGHTNESS_DEF : 80;
    FastLED.setBrightness(initBright);
    off();
    LOG_I(MOD_LED, "NeoPixel ready on GPIO%d  brightness=%d", NEOPIXEL_PIN, initBright);

    for (;;) {
        // Pick up state changes
        if (_stateChanged) {
            _currentState = _pendingState;
            _stateChanged = false;
            LOG_D(MOD_LED, "LED state -> %d", (int)_currentState);
        }

        bool abort = false;

        switch (_currentState) {
            case LED_BOOT:
                // White, 2 Hz blink
                abort = blinkN(CRGB(255,255,255), 250, 250, 1);
                break;

            case LED_WIFI_CONNECTING:
                // Blue, 4 Hz blink
                abort = blinkN(CRGB(0,0,255), 125, 125, 1);
                break;

            case LED_AP_MODE:
                // Yellow, 0.5 Hz blink
                abort = blinkN(CRGB(255,200,0), 1000, 1000, 1);
                break;

            case LED_DTU_OFFLINE:
                // Orange, double-blink 1 Hz
                abort = doubleBlink(CRGB(255,100,0), 150, 700);
                break;

            case LED_NO_MQTT:
                // Cyan, pulsing 0.5 Hz
                abort = pulse(CRGB(0,200,255), 2000);
                break;

            case LED_OPERATIONAL:
                // Green heartbeat 0.2 Hz (5 s period)
                abort = heartbeat(CRGB(0,255,0), 5000);
                break;

            case LED_DATA_FLASH:
                // Bright green flash 100 ms, then back to operational
                setColor(CRGB(0,255,100));
                vTaskDelay(pdMS_TO_TICKS(100));
                setLedState(LED_OPERATIONAL);
                abort = false;
                break;

            case LED_OTA:
                // Magenta, 5 Hz blink
                abort = blinkN(CRGB(255,0,200), 100, 100, 1);
                break;

            case LED_ERROR:
                // Red SOS
                abort = sos(CRGB(255,0,0));
                break;

            case LED_FACTORY_RESET:
                // Red 10x fast blink
                abort = blinkN(CRGB(255,0,0), 50, 50, 10);
                if (!abort) {
                    vTaskDelay(pdMS_TO_TICKS(1000));
                }
                break;

            case LED_STANDBY:
                // Dark blue, steady
                setColor(CRGB(0,0,30));
                vTaskDelay(pdMS_TO_TICKS(500));
                break;

            default:
                vTaskDelay(pdMS_TO_TICKS(100));
                break;
        }

        // Update FastLED brightness if config changed (guard against 0)
        uint8_t b = appConfig.ledBrightness > 0 ? appConfig.ledBrightness : 80;
        FastLED.setBrightness(b);

        if (!abort) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
}
