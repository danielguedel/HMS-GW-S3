// taskMQTT.cpp — v2 (esp-mqtt non-blocking, DataStore pattern)
// Uses ESP-IDF native esp_mqtt_client — no blocking connect(), no watchdog issues.
// Event handler runs in esp-mqtt's own thread; DataStore API is mutex-safe.

#include "taskMQTT.h"
#include "dataStore.h"
#include "appConfig.h"
#include "systemState.h"
#include "config.h"
#include "logger.h"
#include <Arduino.h>
#include "mqtt_client.h"
#include <ArduinoJson.h>

static esp_mqtt_client_handle_t _client = nullptr;

// Persistent config strings — must outlive the client handle
static char _uri[80];
static char _clientId[32];
static char _lwtTopic[80];

// ─── Topic helpers ────────────────────────────────────────────────────────────
static String T(const char* suffix) {
    return String(appConfig.mqttTopic) + "/" + suffix;
}

static void pub(const char* suffix, const char* payload, bool retain = false) {
    if (!_client) return;
    esp_mqtt_client_publish(_client, T(suffix).c_str(), payload, 0, 0, retain ? 1 : 0);
}

static void pubFloat(const char* suffix, float val, int dec, bool retain) {
    char buf[24]; dtostrf(val, 1, dec, buf);
    pub(suffix, buf, retain);
}

static void pubInt(const char* suffix, int val, bool retain = false) {
    char buf[16]; snprintf(buf, sizeof(buf), "%d", val);
    pub(suffix, buf, retain);
}

// ─── Publish PV data (Spec §5.3) ─────────────────────────────────────────────
static void publishPvData(const DataStore::PvData& pv) {
    bool r = appConfig.mqttRetain;

    if (appConfig.mqttOpenDtu) {
        String base = String(appConfig.mqttTopic) + "/";
        auto opub = [&](const char* path, float val, int dec) {
            char buf[24]; dtostrf(val, 1, dec, buf);
            esp_mqtt_client_publish(_client, (base + path).c_str(), buf, 0, 0, r ? 1 : 0);
        };
        opub("0/power",      pv.grid_p, 1); opub("0/voltage",    pv.grid_v, 1);
        opub("0/current",    pv.grid_i, 2); opub("0/yieldday",   pv.grid_dE,3);
        opub("0/yieldtotal", pv.grid_tE,3); opub("0/temperatur", pv.temp,   1);
        opub("1/power",      pv.pv0_p,  1); opub("1/voltage",    pv.pv0_v,  1);
        opub("1/current",    pv.pv0_i,  2); opub("1/yieldday",   pv.pv0_dE, 3);
        opub("1/yieldtotal", pv.pv0_tE, 3);
        opub("2/power",      pv.pv1_p,  1); opub("2/voltage",    pv.pv1_v,  1);
        opub("2/current",    pv.pv1_i,  2); opub("2/yieldday",   pv.pv1_dE, 3);
        opub("2/yieldtotal", pv.pv1_tE, 3);
        char buf[16];
        snprintf(buf, sizeof(buf), "%d", pv.powerLimit);
        esp_mqtt_client_publish(_client, (base+"status/limit_relative").c_str(), buf, 0, 0, r?1:0);
        snprintf(buf, sizeof(buf), "%d", pv.wifiRssi);
        esp_mqtt_client_publish(_client, (base+"dtu/rssi").c_str(), buf, 0, 0, 0);
    } else {
        char buf[16];
        snprintf(buf, sizeof(buf), "%lu", (unsigned long)pv.timestamp);
        pub("timestamp", buf, false);
        pubFloat("grid/U",           pv.grid_v,  1, r);
        pubFloat("grid/I",           pv.grid_i,  2, r);
        pubFloat("grid/P",           pv.grid_p,  1, r);
        pubFloat("grid/dailyEnergy", pv.grid_dE, 3, r);
        pubFloat("grid/totalEnergy", pv.grid_tE, 3, r);
        pubFloat("pv0/U",            pv.pv0_v,   1, r);
        pubFloat("pv0/I",            pv.pv0_i,   2, r);
        pubFloat("pv0/P",            pv.pv0_p,   1, r);
        pubFloat("pv0/dailyEnergy",  pv.pv0_dE,  3, r);
        pubFloat("pv0/totalEnergy",  pv.pv0_tE,  3, r);
        pubFloat("pv1/U",            pv.pv1_v,   1, r);
        pubFloat("pv1/I",            pv.pv1_i,   2, r);
        pubFloat("pv1/P",            pv.pv1_p,   1, r);
        pubFloat("pv1/dailyEnergy",  pv.pv1_dE,  3, r);
        pubFloat("pv1/totalEnergy",  pv.pv1_tE,  3, r);
        pubFloat("inverter/Temp",    pv.temp,     1, r);
        pubInt  ("inverter/PowerLimit",    pv.powerLimit,     r);
        pubInt  ("inverter/warningsActive",pv.warningsActive, false);
    }
    LOG_D(MOD_MQTT, "Published PV data (ts=%lu)", (unsigned long)pv.timestamp);
}

// ─── Publish GPIO state ───────────────────────────────────────────────────────
static void publishGpioState(const DataStore::GpioState& gpio) {
    bool r = appConfig.mqttRetain;
    pub("relay/state", gpio.relay ? "1" : "0", r);
    for (int i = 0; i < 4; i++) {
        char topic[24]; snprintf(topic, sizeof(topic), "gpio%d/state", i + 1);
        pub(topic, gpio.gpio[i] ? "1" : "0", r);
    }
}

// ─── Publish system stats ─────────────────────────────────────────────────────
static void publishSystemStats() {
    DataStore::SystemStatus sys = dsGetSystem();
    char buf[16];
    snprintf(buf, sizeof(buf), "%lu", (unsigned long)sys.uptimeS);
    pub("system/uptime", buf, false);
    snprintf(buf, sizeof(buf), "%d",  (int)sys.wifiRssi);
    pub("system/rssi",   buf, false);
    snprintf(buf, sizeof(buf), "%lu", (unsigned long)sys.freeHeap);
    pub("system/heap",   buf, false);
}

// ─── HA Auto-Discovery (Spec §5.4) ───────────────────────────────────────────
static void publishHaSensor(const char* uid, const char* name,
                             const char* stateSuffix, const char* unit,
                             const char* devClass) {
    char discTopic[128];
    snprintf(discTopic, sizeof(discTopic),
             "homeassistant/sensor/%s_%s/config", appConfig.mqttTopic, uid);
    JsonDocument doc;
    doc["name"]        = name;
    doc["unique_id"]   = String(appConfig.mqttTopic) + "_" + uid;
    doc["state_topic"] = T(stateSuffix);
    if (unit)     doc["unit_of_measurement"] = unit;
    if (devClass) doc["device_class"]        = devClass;
    doc["device"]["identifiers"][0] = appConfig.mqttTopic;
    doc["device"]["name"]           = "HMS-800W-2T";
    doc["device"]["manufacturer"]   = "Hoymiles";
    doc["device"]["sw_version"]     = FW_VERSION;
    String payload; serializeJson(doc, payload);
    esp_mqtt_client_publish(_client, discTopic, payload.c_str(), 0, 0, 1);
}

static void publishHaSwitch(const char* uid, const char* name,
                             const char* stateSuffix, const char* cmdSuffix) {
    char discTopic[128];
    snprintf(discTopic, sizeof(discTopic),
             "homeassistant/switch/%s_%s/config", appConfig.mqttTopic, uid);
    JsonDocument doc;
    doc["name"]          = name;
    doc["unique_id"]     = String(appConfig.mqttTopic) + "_" + uid;
    doc["state_topic"]   = T(stateSuffix);
    doc["command_topic"] = T(cmdSuffix);
    doc["payload_on"]    = "1";
    doc["payload_off"]   = "0";
    doc["device"]["identifiers"][0] = appConfig.mqttTopic;
    doc["device"]["name"]           = "HMS-800W-2T";
    doc["device"]["manufacturer"]   = "Hoymiles";
    String payload; serializeJson(doc, payload);
    esp_mqtt_client_publish(_client, discTopic, payload.c_str(), 0, 0, 1);
}

// Discovery sends one entity per 500ms, starting 5s after connect (Spec §5.4)
static int      _haIdx    = -1;
static uint32_t _haNextMs = 0;

static void haDiscoveryStep() {
    if (_haIdx < 0 || !_client)    return;
    if (millis() < _haNextMs)      return;

    switch (_haIdx) {
        case  0: publishHaSensor("grid_p",  "Grid Power",        "grid/P",            "W",   "power");       break;
        case  1: publishHaSensor("grid_u",  "Grid Voltage",      "grid/U",            "V",   "voltage");     break;
        case  2: publishHaSensor("grid_i",  "Grid Current",      "grid/I",            "A",   "current");     break;
        case  3: publishHaSensor("grid_dE", "Grid Energy Today", "grid/dailyEnergy",  "kWh", "energy");      break;
        case  4: publishHaSensor("grid_tE", "Grid Energy Total", "grid/totalEnergy",  "kWh", "energy");      break;
        case  5: publishHaSensor("pv0_p",   "PV1 Power",         "pv0/P",             "W",   "power");       break;
        case  6: publishHaSensor("pv0_u",   "PV1 Voltage",       "pv0/U",             "V",   "voltage");     break;
        case  7: publishHaSensor("pv1_p",   "PV2 Power",         "pv1/P",             "W",   "power");       break;
        case  8: publishHaSensor("pv1_u",   "PV2 Voltage",       "pv1/U",             "V",   "voltage");     break;
        case  9: publishHaSensor("temp",    "Inverter Temp",     "inverter/Temp",     "°C",  "temperature"); break;
        case 10: publishHaSensor("pwr_lim", "Power Limit",       "inverter/PowerLimit","%",  nullptr);       break;
        case 11: publishHaSwitch("relay",   "Relay",  "relay/state",  "relay/set");  break;
        case 12: publishHaSwitch("gpio1",   "GPIO 1", "gpio1/state",  "gpio1/set");  break;
        case 13: publishHaSwitch("gpio2",   "GPIO 2", "gpio2/state",  "gpio2/set");  break;
        case 14: publishHaSwitch("gpio3",   "GPIO 3", "gpio3/state",  "gpio3/set");  break;
        case 15: publishHaSwitch("gpio4",   "GPIO 4", "gpio4/state",  "gpio4/set");  break;
        default:
            LOG_I(MOD_MQTT, "HA discovery complete (%d entities)", _haIdx);
            _haIdx = -1;
            return;
    }
    _haIdx++;
    _haNextMs = millis() + 500;
}

// ─── Inbound message handler ──────────────────────────────────────────────────
static void onMessage(const char* topic, const char* data) {
    String t    = String(topic);
    String base = String(appConfig.mqttTopic) + "/";

    if (t == base + "inverter/PowerLimitSet/set") {
        DataStore::DtuCommand cmd = {};
        cmd.setPowerLimit   = true;
        cmd.powerLimitValue = atoi(data);
        dsSetDtuCommand(cmd);
    } else if (t == base + "inverter/On/set") {
        DataStore::DtuCommand cmd = {};
        cmd.setInverterOn   = true;
        cmd.inverterOnValue = (atoi(data) == 1);
        dsSetDtuCommand(cmd);
    } else if (t == base + "inverter/RebootDtu/set" && atoi(data) == 1) {
        DataStore::DtuCommand cmd = {};
        cmd.rebootDtu = true;
        dsSetDtuCommand(cmd);
    } else if (t == base + "inverter/RebootGw/set" && atoi(data) == 1) {
        LOG_W(MOD_MQTT, "Gateway reboot via MQTT");
        vTaskDelay(pdMS_TO_TICKS(200)); ESP.restart();
    } else if (t == base + "relay/set") {
        dsSetGpioCommand(0, atoi(data) == 1);
    } else {
        for (int i = 1; i <= 4; i++) {
            char sub[32]; snprintf(sub, sizeof(sub), "gpio%d/set", i);
            if (t == base + sub) {
                dsSetGpioCommand(i, atoi(data) == 1);
                break;
            }
        }
    }
}

// ─── esp-mqtt event handler (runs in esp-mqtt internal thread) ────────────────
static void mqttEventHandler(void* /*arg*/, esp_event_base_t /*base*/,
                              int32_t eventId, void* eventData) {
    auto ev = (esp_mqtt_event_handle_t)eventData;

    switch (eventId) {

        case MQTT_EVENT_CONNECTED: {
            LOG_I(MOD_MQTT, "Connected to %s:%d", appConfig.mqttHost, appConfig.mqttPort);

            DataStore::SystemStatus sys = dsGetSystem();
            sys.mqttConnected     = true;
            sys.mqttLastConnectMs = millis();
            dsSetSystem(sys);
            xEventGroupSetBits(systemStateEvents, EVT_MQTT_CONNECTED);

            // Subscribe to all control topics
            String base = String(appConfig.mqttTopic) + "/";
            const char* subs[] = {
                "relay/set",
                "inverter/PowerLimitSet/set",
                "inverter/On/set",
                "inverter/RebootDtu/set",
                "inverter/RebootGw/set"
            };
            for (auto s : subs)
                esp_mqtt_client_subscribe(_client, (base + s).c_str(), 0);
            for (int i = 1; i <= 4; i++) {
                char sub[48]; snprintf(sub, sizeof(sub), "%sgpio%d/set", base.c_str(), i);
                esp_mqtt_client_subscribe(_client, sub, 0);
            }

            pub("system/status", "online", true);
            publishGpioState(dsGetGpio());

            // Schedule HA discovery: 5s delay, 500ms between entities (Spec §5.4)
            if (appConfig.mqttHaDiscovery) {
                _haIdx    = 0;
                _haNextMs = millis() + 5000;
                LOG_I(MOD_MQTT, "HA discovery scheduled in 5s (%d entities)", 16);
            }
            break;
        }

        case MQTT_EVENT_DISCONNECTED: {
            LOG_W(MOD_MQTT, "Disconnected — will reconnect automatically");
            DataStore::SystemStatus sys = dsGetSystem();
            sys.mqttConnected = false;
            dsSetSystem(sys);
            xEventGroupClearBits(systemStateEvents, EVT_MQTT_CONNECTED);
            _haIdx = -1;
            break;
        }

        case MQTT_EVENT_DATA: {
            if (!ev->topic || ev->topic_len == 0) break;
            // topic and data are NOT null-terminated — copy to stack buffers
            char topic[128] = {};
            char data[128]  = {};
            memcpy(topic, ev->topic, min(ev->topic_len, (int)sizeof(topic) - 1));
            memcpy(data,  ev->data,  min(ev->data_len,  (int)sizeof(data)  - 1));
            onMessage(topic, data);
            break;
        }

        case MQTT_EVENT_ERROR:
            if (ev->error_handle)
                LOG_E(MOD_MQTT, "Error type=%d", ev->error_handle->error_type);
            break;

        default:
            break;
    }
}

// ─── Task ─────────────────────────────────────────────────────────────────────
void taskMQTT(void* pvParameters) {
    if (strlen(appConfig.mqttHost) == 0) {
        LOG_W(MOD_MQTT, "No broker configured — task idle");
        vTaskDelete(nullptr); return;
    }

    // Wait for STA WiFi only (not AP mode)
    xEventGroupWaitBits(systemStateEvents, EVT_WIFI_CONNECTED | EVT_WIFI_AP_MODE,
                        pdFALSE, pdFALSE, portMAX_DELAY);
    if (xEventGroupGetBits(systemStateEvents) & EVT_WIFI_AP_MODE) {
        LOG_W(MOD_MQTT, "AP mode — task idle");
        vTaskDelete(nullptr); return;
    }

    // Build persistent config strings
    snprintf(_uri,      sizeof(_uri),
             "mqtt://%s:%d", appConfig.mqttHost, appConfig.mqttPort);
    snprintf(_clientId, sizeof(_clientId),
             "hmsgws3_%06llX", (unsigned long long)(ESP.getEfuseMac() & 0xFFFFFF));
    snprintf(_lwtTopic, sizeof(_lwtTopic),
             "%s/system/status", appConfig.mqttTopic);

    LOG_I(MOD_MQTT, "Starting — broker: %s  client: %s", _uri, _clientId);

    // Flat struct API (ESP-IDF 4.x / Arduino-ESP32 2.x bundled SDK)
    esp_mqtt_client_config_t cfg = {};
    cfg.uri         = _uri;
    cfg.client_id   = _clientId;
    cfg.keepalive   = MQTT_KEEPALIVE_S;
    cfg.lwt_topic   = _lwtTopic;
    cfg.lwt_msg     = "offline";
    cfg.lwt_msg_len = 7;
    cfg.lwt_retain  = 1;
    if (strlen(appConfig.mqttUser) > 0) {
        cfg.username = appConfig.mqttUser;
        cfg.password = appConfig.mqttPass;
    }

    _client = esp_mqtt_client_init(&cfg);
    esp_mqtt_client_register_event(_client, (esp_mqtt_event_id_t)ESP_EVENT_ANY_ID,
                                   mqttEventHandler, nullptr);
    esp_mqtt_client_start(_client);

    uint32_t lastPvTs     = 0;
    uint32_t lastSysMs    = 0;
    uint32_t lastGpioCkMs = 0;
    DataStore::GpioState lastGpio = {};

    for (;;) {
        EventBits_t bits = xEventGroupGetBits(systemStateEvents);
        bool connected   = (bits & EVT_MQTT_CONNECTED) != 0;

        // ── Publish PV data when new measurement arrives ───────────────────────
        DataStore::PvData pv = dsGetPv();
        if (connected && pv.valid && pv.timestamp != lastPvTs) {
            publishPvData(pv);
            lastPvTs = pv.timestamp;
        }

        // ── Publish GPIO state on change ───────────────────────────────────────
        uint32_t now = millis();
        if (connected && now - lastGpioCkMs >= 250) {
            lastGpioCkMs = now;
            DataStore::GpioState gpio = dsGetGpio();
            if (memcmp(&gpio, &lastGpio, sizeof(gpio)) != 0) {
                publishGpioState(gpio);
                lastGpio = gpio;
            }
        }

        // ── System stats every 60s ─────────────────────────────────────────────
        if (connected && now - lastSysMs >= 60000) {
            lastSysMs = now;
            publishSystemStats();
        }

        // ── HA discovery — one entity per 500ms, starting 5s after connect ─────
        haDiscoveryStep();

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
