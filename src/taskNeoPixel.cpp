// taskLED (taskNeoPixel.cpp)  -  v2 (DataStore-driven state derivation)
// LED state is auto-derived from DataStore + EventGroup each cycle.
// setLedState() is only used for one-shot transients (LED_DATA_FLASH).
//
// Colour semantics:
//   Weiss   → Neutral        (BOOT)
//   Blau    → Wartend        (WIFI_CONNECTING, AP_MODE)
//   Orange  → Teilausfall    (DTU_OFFLINE, DATA_FLASH)
//   Cyan    → Nebenkanal     (NO_MQTT)
//   Grün    → OK             (OPERATIONAL, STANDBY)
//   Magenta → Systemvorgang  (OTA)
//   Rot     → Kritisch       (ERROR, FACTORY_RESET)

#include "taskLED.h"
#include "dataStore.h"
#include "appConfig.h"
#include "systemState.h"
#include "config.h"
#include "logger.h"
#include <FastLED.h>

static CRGB leds[LED_COUNT];

// --- Colours ------------------------------------------------------------------
static const CRGB COL_WHITE   = CRGB(255, 255, 255);
static const CRGB COL_BLUE    = CRGB(  0,  80, 255);
static const CRGB COL_ORANGE  = CRGB(255,  80,   0);
static const CRGB COL_CYAN    = CRGB(  0, 200, 255);
static const CRGB COL_GREEN   = CRGB(  0, 220,  30);
static const CRGB COL_MAGENTA = CRGB(255,   0, 180);
static const CRGB COL_RED     = CRGB(255,   0,   0);

// --- Transient override  -  LED_DATA_FLASH only ---------------------------------
static volatile bool       _dataFlash    = false;
static volatile LedState_t _currentState = LED_BOOT;

void setLedState(LedState_t state) {
    if (state == LED_DATA_FLASH) { _dataFlash = true; }
    else                         { _currentState = state; }
}

// --- Derive background state from DataStore + EventGroup ---------------------
static LedState_t deriveState() {
    EventBits_t bits = xEventGroupGetBits(systemStateEvents);
    if (bits & EVT_FACTORY_RESET) return LED_FACTORY_RESET;
    if (bits & EVT_OTA_RUNNING)   return LED_OTA;

    DataStore::SystemStatus sys = dsGetSystem();
    DataStore::PvData       pv  = dsGetPv();

    if (sys.wifiApMode)                return LED_AP_MODE;
    if (!sys.wifiConnected)            return LED_WIFI_CONNECTING;
    if (!sys.dtuOnline)                return LED_DTU_OFFLINE;
    if (!sys.mqttConnected)            return LED_NO_MQTT;
    if (pv.valid && pv.grid_p < 1.0f)  return LED_STANDBY;
    return LED_OPERATIONAL;
}

// Abort when DataStore state changed or explicit override received
static bool shouldAbort(LedState_t runningFor) {
    if (_dataFlash)                   return true;
    if (_currentState != runningFor)  return true;
    return deriveState() != runningFor;
}

// --- Colour output ------------------------------------------------------------
static void setColor(CRGB c, uint8_t brightnessOverride = 0) {
    uint8_t b = brightnessOverride > 0
        ? brightnessOverride
        : (appConfig.ledBrightness > 0 ? appConfig.ledBrightness : LED_BRIGHTNESS_DEFAULT);
    leds[0] = CRGB((uint16_t)c.r * b / 255,
                   (uint16_t)c.g * b / 255,
                   (uint16_t)c.b * b / 255);
    FastLED.show();
}

static void ledOff() {
    leds[0] = CRGB::Black;
    FastLED.show();
}

// --- Animation primitives -----------------------------------------------------

// Single blink: on onMs, off offMs
static bool blinkOnce(CRGB c, uint32_t onMs, uint32_t offMs, LedState_t s,
                      uint8_t brightnessOverride = 0) {
    if (shouldAbort(s)) return true;
    setColor(c, brightnessOverride); vTaskDelay(pdMS_TO_TICKS(onMs));
    if (shouldAbort(s)) return true;
    ledOff();                        vTaskDelay(pdMS_TO_TICKS(offMs));
    return false;
}

// Triple-blink + long pause  -  signals "needs user action" (AP mode)
static bool tripleBlink(CRGB c, LedState_t s) {
    for (int i = 0; i < 3; i++) {
        if (shouldAbort(s)) return true;
        setColor(c); vTaskDelay(pdMS_TO_TICKS(150));
        if (shouldAbort(s)) return true;
        ledOff();    vTaskDelay(pdMS_TO_TICKS(150));
    }
    if (shouldAbort(s)) return true;
    vTaskDelay(pdMS_TO_TICKS(900));
    return false;
}

// Double-blink: pulse–gap–pulse–long-pause
static bool doubleBlink(CRGB c, uint32_t pulseMs, uint32_t pauseMs, LedState_t s) {
    if (shouldAbort(s)) return true;
    setColor(c); vTaskDelay(pdMS_TO_TICKS(pulseMs));
    if (shouldAbort(s)) return true;
    ledOff();    vTaskDelay(pdMS_TO_TICKS(pulseMs));
    if (shouldAbort(s)) return true;
    setColor(c); vTaskDelay(pdMS_TO_TICKS(pulseMs));
    if (shouldAbort(s)) return true;
    ledOff();    vTaskDelay(pdMS_TO_TICKS(pauseMs));
    return false;
}

// Smooth pulse: 0 → max → 0 over periodMs, optional brightness cap
static bool pulse(CRGB base, uint32_t periodMs, LedState_t s, uint8_t brightnessCap = 0) {
    const uint32_t steps  = 40;
    const uint32_t stepMs = periodMs / (2 * steps);
    uint8_t maxB = brightnessCap > 0
        ? brightnessCap
        : (appConfig.ledBrightness > 0 ? appConfig.ledBrightness : LED_BRIGHTNESS_DEFAULT);
    for (uint32_t i = 0; i < steps; i++) {
        if (shouldAbort(s)) return true;
        uint8_t scale = (uint8_t)((uint32_t)maxB * i / steps);
        leds[0] = CRGB((uint16_t)base.r * scale / 255,
                       (uint16_t)base.g * scale / 255,
                       (uint16_t)base.b * scale / 255);
        FastLED.show();
        vTaskDelay(pdMS_TO_TICKS(stepMs));
    }
    for (uint32_t i = steps; i > 0; i--) {
        if (shouldAbort(s)) return true;
        uint8_t scale = (uint8_t)((uint32_t)maxB * i / steps);
        leds[0] = CRGB((uint16_t)base.r * scale / 255,
                       (uint16_t)base.g * scale / 255,
                       (uint16_t)base.b * scale / 255);
        FastLED.show();
        vTaskDelay(pdMS_TO_TICKS(stepMs));
    }
    return false;
}

// Heartbeat: blink–blink–long-dark
static bool heartbeat(CRGB c, uint32_t periodMs, LedState_t s) {
    if (shouldAbort(s)) return true;
    setColor(c); vTaskDelay(pdMS_TO_TICKS(80));
    if (shouldAbort(s)) return true;
    ledOff();    vTaskDelay(pdMS_TO_TICKS(80));
    if (shouldAbort(s)) return true;
    setColor(c); vTaskDelay(pdMS_TO_TICKS(80));
    if (shouldAbort(s)) return true;
    ledOff();    vTaskDelay(pdMS_TO_TICKS(periodMs > 240 ? periodMs - 240 : 100));
    return false;
}

// --- Task ---------------------------------------------------------------------
void taskLED(void* pvParameters) {
    FastLED.addLeds<WS2812B, LED_PIN, GRB>(leds, LED_COUNT);
    FastLED.setBrightness(LED_BRIGHTNESS_DEFAULT);
    ledOff();
    LOG_I(MOD_LED, "LED task started on GPIO%d  brightness=%d",
          LED_PIN, appConfig.ledBrightness);

    // Boot animation: 3× white flash
    for (int i = 0; i < 3; i++) {
        setColor(COL_WHITE); vTaskDelay(pdMS_TO_TICKS(120));
        ledOff();            vTaskDelay(pdMS_TO_TICKS(120));
    }
    vTaskDelay(pdMS_TO_TICKS(500));
    _currentState = deriveState();

    for (;;) {
        // -- One-shot data flash (orange, 80 ms) ------------------------------
        if (_dataFlash) {
            _dataFlash = false;
            setColor(COL_ORANGE);
            vTaskDelay(pdMS_TO_TICKS(80));
            ledOff();
            _currentState = deriveState();
            continue;
        }

        // -- Auto-derive background state --------------------------------------
        LedState_t state = deriveState();
        if (state != _currentState) {
            LOG_D(MOD_LED, "LED state -> %d", (int)state);
            _currentState = state;
        }

        // -- Animations per state ----------------------------------------------
        switch (_currentState) {

            case LED_BOOT:
                // Weiss  -  1 Hz blink (fallback, boot sequence runs above)
                blinkOnce(COL_WHITE, 250, 250, _currentState);
                break;

            case LED_WIFI_CONNECTING:
                // Blau  -  1 Hz blink (ruhig, wartend)
                blinkOnce(COL_BLUE, 250, 250, _currentState);
                break;

            case LED_AP_MODE:
                // Blau  -  3× kurz + Pause (dringlicher: braucht Nutzeraktion)
                tripleBlink(COL_BLUE, _currentState);
                break;

            case LED_DTU_OFFLINE:
                // Orange  -  Doppelblink (Teilausfall)
                doubleBlink(COL_ORANGE, 150, 700, _currentState);
                break;

            case LED_NO_MQTT:
                // Cyan  -  langsamer Puls 4s (unkritisch, läuft weiter)
                pulse(COL_CYAN, 4000, _currentState);
                break;

            case LED_OPERATIONAL:
                // Grün  -  Herzschlag 5s (alles gut, ruhig)
                heartbeat(COL_GREEN, 5000, _currentState);
                break;

            case LED_STANDBY:
                // Grün  -  sehr langer Puls 10s, 10% Helligkeit (Nacht, kaum sichtbar)
                pulse(COL_GREEN, 10000, _currentState,
                      (uint8_t)(appConfig.ledBrightness * 10 / 100 + 1));
                break;

            case LED_DATA_FLASH:
                // Handled above  -  should not reach here
                _currentState = deriveState();
                break;

            case LED_OTA:
                // Magenta  -  schnell 5 Hz (Systemvorgang, unverwechselbar)
                blinkOnce(COL_MAGENTA, 100, 100, _currentState);
                break;

            case LED_ERROR:
                // Rot  -  schnell 4 Hz (kritisch, dringend)
                blinkOnce(COL_RED, 125, 125, _currentState);
                break;

            case LED_FACTORY_RESET:
                // Rot  -  dauerhaft an (irreversibler Vorgang)
                setColor(COL_RED);
                vTaskDelay(pdMS_TO_TICKS(100));
                break;

            default:
                vTaskDelay(pdMS_TO_TICKS(100));
                break;
        }
    }
}
