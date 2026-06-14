// taskSerial.cpp — v2 (Konsolen-Kommandos, DataStore pattern)
// Reads commands from Serial line by line, dispatches via DataStore API.
// All command output goes to Serial directly (not routed through the logger).

#include "taskSerial.h"
#include "dataStore.h"
#include "appConfig.h"
#include "systemState.h"
#include "config.h"
#include "logger.h"
#include "taskLED.h"
#include <Arduino.h>

// ─── Output helpers ───────────────────────────────────────────────────────────

static void printf_(const char* fmt, ...) {
    char buf[256]; va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
    Serial.print(buf);
}

static void prompt() { Serial.print("\r\n> "); }

// ─── Command handlers ─────────────────────────────────────────────────────────

static void cmdHelp() {
    Serial.println("Available commands:");
    Serial.println("  help              This list");
    Serial.println("  version           Firmware version");
    Serial.println("  status            System status (WiFi, DTU, MQTT, heap)");
    Serial.println("  wifi              WiFi detail");
    Serial.println("  dtu               DTU status + last PV data");
    Serial.println("  mqtt              MQTT status");
    Serial.println("  gpio              GPIO states");
    Serial.println("  config            Current configuration");
    Serial.println("  relay on|off      Set relay");
    Serial.println("  gpio1 on|off      Set GP1");
    Serial.println("  gpio2 on|off      Set GP2");
    Serial.println("  gpio3 on|off      Set GP3");
    Serial.println("  gpio4 on|off      Set GP4");
    Serial.println("  loglevel error|warn|info|debug");
    Serial.println("  restart           Reboot gateway");
    Serial.println("  reset             Factory reset (clears config.json)");
    Serial.println("  ledtest           Cycle through all LED states");
}

static void cmdVersion() {
    printf_("HMS-GW-S3  fw=%s  build=%d  date=%s\n",
            FW_VERSION, BUILD_NUMBER, FW_DATE);
}

static void cmdStatus() {
    DataStore::SystemStatus sys  = dsGetSystem();
    EventBits_t             bits = xEventGroupGetBits(systemStateEvents);

    printf_("Uptime   : %lu s\n",     (unsigned long)sys.uptimeS);
    printf_("Heap     : %lu bytes\n", (unsigned long)sys.freeHeap);
    printf_("WiFi     : %s  IP=%s  RSSI=%d dBm\n",
            sys.wifiConnected ? "connected" : (sys.wifiApMode ? "AP mode" : "offline"),
            sys.wifiIp, (int)sys.wifiRssi);
    printf_("NTP      : %s  time=%lu\n",
            sys.ntpTime > 0 ? "synced" : "not synced",
            (unsigned long)sys.ntpTime);
    printf_("DTU      : %s\n",        sys.dtuOnline      ? "online"    : "offline");
    printf_("MQTT     : %s\n",        sys.mqttConnected  ? "connected" : "disconnected");
    printf_("EventBits: 0x%02X\n",   (unsigned)bits);
}

static void cmdWifi() {
    DataStore::SystemStatus sys = dsGetSystem();
    printf_("Mode     : %s\n",       sys.wifiApMode    ? "AP"  : "STA");
    printf_("Connected: %s\n",       sys.wifiConnected ? "yes" : "no");
    printf_("IP       : %s\n",       sys.wifiIp);
    printf_("RSSI     : %d dBm\n",   (int)sys.wifiRssi);
    printf_("MAC      : %s\n",       sys.macAddress.c_str());
    printf_("SSID     : %s\n",       appConfig.wifiSsid);
}

static void cmdDtu() {
    DataStore::SystemStatus sys = dsGetSystem();
    DataStore::PvData       pv  = dsGetPv();

    printf_("Host     : %s:%d\n",  appConfig.dtuHost, appConfig.dtuPort);
    printf_("Online   : %s\n",     sys.dtuOnline     ? "yes" : "no");
    printf_("Cloud    : %s\n",     sys.dtuCloudBusy  ? "busy (paused)" : "free");
    if (!pv.valid) { Serial.println("PV data  : not yet received"); return; }
    printf_("PV data  : ts=%lu\n", (unsigned long)pv.timestamp);
    printf_("  Grid   : %.1f V  %.2f A  %.1f W  today=%.3f kWh  total=%.3f kWh\n",
            pv.grid_v, pv.grid_i, pv.grid_p, pv.grid_dE, pv.grid_tE);
    printf_("  PV1    : %.1f V  %.2f A  %.1f W  today=%.3f kWh  total=%.3f kWh\n",
            pv.pv0_v,  pv.pv0_i,  pv.pv0_p,  pv.pv0_dE,  pv.pv0_tE);
    printf_("  PV2    : %.1f V  %.2f A  %.1f W  today=%.3f kWh  total=%.3f kWh\n",
            pv.pv1_v,  pv.pv1_i,  pv.pv1_p,  pv.pv1_dE,  pv.pv1_tE);
    printf_("  Temp   : %.1f C   Limit: %d%%   Active: %s\n",
            pv.temp, pv.powerLimit, pv.inverterActive ? "yes" : "no");
    printf_("  DTU RSSI: %d dBm\n", pv.wifiRssi);
}

static void cmdMqtt() {
    DataStore::SystemStatus sys = dsGetSystem();
    printf_("Broker   : %s:%d\n", appConfig.mqttHost, appConfig.mqttPort);
    printf_("Topic    : %s\n",    appConfig.mqttTopic);
    printf_("User     : %s\n",    strlen(appConfig.mqttUser) ? appConfig.mqttUser : "(none)");
    printf_("Connected: %s\n",    sys.mqttConnected     ? "yes" : "no");
    printf_("HA disco : %s\n",    appConfig.mqttHaDiscovery ? "on" : "off");
    printf_("OpenDTU  : %s\n",    appConfig.mqttOpenDtu     ? "on" : "off");
    printf_("Retain   : %s\n",    appConfig.mqttRetain      ? "on" : "off");
}

static void cmdGpio() {
    DataStore::GpioState gpio = dsGetGpio();
    const char* modes[]       = {"OUTPUT", "INPUT", "I2C_RESERVED"};
    printf_("Relay  (GPIO%d, inv=%d): %s\n",
            appConfig.relay.pin, appConfig.relay.inverted,
            gpio.relay ? "ON" : "OFF");
    for (int i = 0; i < 4; i++) {
        uint8_t m = (uint8_t)appConfig.gp[i].mode;
        if (m > 2) m = 2;
        printf_("GP%d    (GPIO%d, mode=%s, inv=%d): %s\n",
                i + 1, appConfig.gp[i].pin, modes[m],
                appConfig.gp[i].inverted, gpio.gpio[i] ? "ON" : "OFF");
    }
}

static void cmdConfig() {
    Serial.println("─── WiFi ───────────────────────────────");
    printf_("  ssid        : %s\n",   appConfig.wifiSsid);
    printf_("  apFallback  : %s\n",   appConfig.wifiApFallback ? "yes" : "no");
    Serial.println("─── DTU ────────────────────────────────");
    printf_("  host        : %s\n",   appConfig.dtuHost);
    printf_("  port        : %d\n",   appConfig.dtuPort);
    printf_("  interval    : %d s\n", appConfig.dtuInterval);
    printf_("  cloudPause  : %d s\n", appConfig.dtuCloudPause);
    printf_("  rebootFails : %d\n",   appConfig.dtuRebootAfterFails);
    Serial.println("─── Power ──────────────────────────────");
    printf_("  limitDefault: %d%%\n", appConfig.powerLimitDefault);
    printf_("  limitTimeout: %d s\n", appConfig.powerLimitTimeout);
    Serial.println("─── MQTT ───────────────────────────────");
    printf_("  host        : %s\n",   appConfig.mqttHost);
    printf_("  port        : %d\n",   appConfig.mqttPort);
    printf_("  topic       : %s\n",   appConfig.mqttTopic);
    printf_("  haDiscovery : %s\n",   appConfig.mqttHaDiscovery ? "on" : "off");
    printf_("  openDtu     : %s\n",   appConfig.mqttOpenDtu     ? "on" : "off");
    printf_("  retain      : %s\n",   appConfig.mqttRetain      ? "on" : "off");
    Serial.println("─── LED ────────────────────────────────");
    printf_("  pin         : GPIO%d\n", appConfig.ledPin);
    printf_("  brightness  : %d\n",     appConfig.ledBrightness);
    Serial.println("─── NTP ────────────────────────────────");
    printf_("  server      : %s\n",   appConfig.ntpServer);
    printf_("  tzOffset    : %d s\n", appConfig.tzOffset);
    Serial.println("─── Misc ───────────────────────────────");
    printf_("  logLevel    : %d\n",   appConfig.logLevel);
}

static void cmdRelaySet(const char* arg) {
    if (!arg || (strcmp(arg, "on") != 0 && strcmp(arg, "off") != 0)) {
        Serial.println("Usage: relay on|off"); return;
    }
    dsSetGpioCommand(0, strcmp(arg, "on") == 0);
    printf_("Relay -> %s\n", arg);
}

static void cmdGpioSet(int gpIdx, const char* arg) {
    if (!arg || (strcmp(arg, "on") != 0 && strcmp(arg, "off") != 0)) {
        printf_("Usage: gpio%d on|off\n", gpIdx); return;
    }
    dsSetGpioCommand(gpIdx, strcmp(arg, "on") == 0);
    printf_("GPIO%d -> %s\n", gpIdx, arg);
}

static void cmdLoglevel(const char* arg) {
    if (!arg) {
        printf_("Log level: %d  (0=error 1=warn 2=info 3=debug)\n", appConfig.logLevel);
        return;
    }
    int lvl = -1;
    if      (strcmp(arg, "error") == 0) lvl = LOG_LEVEL_ERROR;
    else if (strcmp(arg, "warn")  == 0) lvl = LOG_LEVEL_WARN;
    else if (strcmp(arg, "info")  == 0) lvl = LOG_LEVEL_INFO;
    else if (strcmp(arg, "debug") == 0) lvl = LOG_LEVEL_DEBUG;
    else { Serial.println("Usage: loglevel error|warn|info|debug"); return; }
    appConfig.logLevel = lvl;
    configSave();
    printf_("Log level -> %d (%s)\n", lvl, arg);
}

static void cmdLedTest() {
    const struct { LedState_t state; const char* name; } seq[] = {
        { LED_BOOT,            "BOOT"            },
        { LED_WIFI_CONNECTING, "WIFI_CONNECTING" },
        { LED_AP_MODE,         "AP_MODE"         },
        { LED_DTU_OFFLINE,     "DTU_OFFLINE"     },
        { LED_NO_MQTT,         "NO_MQTT"         },
        { LED_OPERATIONAL,     "OPERATIONAL"     },
        { LED_DATA_FLASH,      "DATA_FLASH"      },
        { LED_OTA,             "OTA"             },
        { LED_ERROR,           "ERROR"           },
        { LED_STANDBY,         "STANDBY"         },
    };
    Serial.println("LED test — 2.5s per state...");
    for (auto& s : seq) {
        printf_("  -> %s\n", s.name);
        setLedState(s.state);
        vTaskDelay(pdMS_TO_TICKS(2500));
    }
    Serial.println("LED test done.");
}

static void cmdReset() {
    Serial.println("Factory reset — setting EVT_FACTORY_RESET and rebooting...");
    vTaskDelay(pdMS_TO_TICKS(100));
    xEventGroupSetBits(systemStateEvents, EVT_FACTORY_RESET);
    vTaskDelay(pdMS_TO_TICKS(3000));    // allow LED to show red; main handles erase
    ESP.restart();
}

static void cmdRestart() {
    Serial.println("Rebooting...");
    vTaskDelay(pdMS_TO_TICKS(200));
    ESP.restart();
}

// ─── Command dispatcher ───────────────────────────────────────────────────────
static void dispatch(char* line) {
    while (*line == ' ') line++;
    if (*line == '\0') return;

    char* verb = line;
    char* arg  = nullptr;
    for (char* p = line; *p; p++) {
        if (*p == ' ') {
            *p = '\0';
            arg = p + 1;
            while (*arg == ' ') arg++;
            break;
        }
    }
    if (arg && *arg == '\0') arg = nullptr;

    if      (strcmp(verb, "help")     == 0) cmdHelp();
    else if (strcmp(verb, "version")  == 0) cmdVersion();
    else if (strcmp(verb, "status")   == 0) cmdStatus();
    else if (strcmp(verb, "wifi")     == 0) cmdWifi();
    else if (strcmp(verb, "dtu")      == 0) cmdDtu();
    else if (strcmp(verb, "mqtt")     == 0) cmdMqtt();
    else if (strcmp(verb, "gpio")     == 0 && !arg) cmdGpio();
    else if (strcmp(verb, "config")   == 0) cmdConfig();
    else if (strcmp(verb, "relay")    == 0) cmdRelaySet(arg);
    else if (strcmp(verb, "gpio1")    == 0) cmdGpioSet(1, arg);
    else if (strcmp(verb, "gpio2")    == 0) cmdGpioSet(2, arg);
    else if (strcmp(verb, "gpio3")    == 0) cmdGpioSet(3, arg);
    else if (strcmp(verb, "gpio4")    == 0) cmdGpioSet(4, arg);
    else if (strcmp(verb, "loglevel") == 0) cmdLoglevel(arg);
    else if (strcmp(verb, "ledtest")  == 0) cmdLedTest();
    else if (strcmp(verb, "restart")  == 0) cmdRestart();
    else if (strcmp(verb, "reset")    == 0) cmdReset();
    else printf_("Unknown command: '%s'  (type 'help')\n", verb);
}

// ─── Task ─────────────────────────────────────────────────────────────────────
void taskSerial(void* pvParameters) {
    static char    buf[128];
    static uint8_t pos = 0;

    LOG_I(MOD_SYS, "Serial console ready at %d baud", SERIAL_BAUD);
    Serial.print("\r\nHMS-GW-S3 serial console — type 'help'\r\n> ");

    for (;;) {
        while (Serial.available()) {
            char c = (char)Serial.read();

            if (c == '\r') continue;

            if (c == '\n') {
                buf[pos] = '\0';
                Serial.println();
                if (pos > 0) dispatch(buf);
                pos = 0;
                prompt();
                continue;
            }

            // Backspace / DEL
            if (c == 0x7F || c == '\b') {
                if (pos > 0) { pos--; Serial.print("\b \b"); }
                continue;
            }

            if (pos < sizeof(buf) - 1) {
                buf[pos++] = c;
                Serial.print(c);    // local echo
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
