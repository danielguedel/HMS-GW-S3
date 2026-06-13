#include "taskMQTT.h"
#include "config.h"
#include "appConfig.h"
#include "systemState.h"
#include "logger.h"
#include "taskNeoPixel.h"
#include "taskDTU.h"
#include "taskGPIO.h"
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/event_groups.h>

static WiFiClient   wifiClient;
static PubSubClient mqttClient(wifiClient);

// ─── Topic helpers ────────────────────────────────────────────────────────────
static String T(const char* suffix) {
    return String(appConfig.mqttTopic) + "/" + suffix;
}

static void pub(const char* suffix, const char* payload, bool retain = false) {
    if (!mqttClient.connected()) return;
    String topic = T(suffix);
    mqttClient.publish(topic.c_str(), payload, retain);
}

static void pubFloat(const char* suffix, float val, int dec = 2) {
    char buf[24]; dtostrf(val, 1, dec, buf);
    pub(suffix, buf, appConfig.mqttRetain);
}

static void pubInt(const char* suffix, int val) {
    char buf[16]; snprintf(buf, sizeof(buf), "%d", val);
    pub(suffix, buf, appConfig.mqttRetain);
}

// ─── Publish DTU data ─────────────────────────────────────────────────────────
static void publishDtuData(const DtuData_t& d) {
    if (!mqttClient.connected()) return;

    if (appConfig.mqttOpenDtu) {
        String base = String(appConfig.mqttTopic) + "/";
        auto opub = [&](const char* path, float val, int dec=2) {
            char buf[24]; dtostrf(val, 1, dec, buf);
            mqttClient.publish((base + path).c_str(), buf, appConfig.mqttRetain);
        };
        opub("0/power",      d.grid_p, 1); opub("0/voltage",    d.grid_v, 1);
        opub("0/current",    d.grid_i, 2); opub("0/yieldday",   d.grid_dE, 3);
        opub("0/yieldtotal", d.grid_tE,3); opub("0/temperatur", d.temp, 1);
        opub("1/power",      d.pv0_p, 1);  opub("1/voltage",    d.pv0_v, 1);
        opub("1/current",    d.pv0_i, 2);  opub("1/yieldday",   d.pv0_dE,3);
        opub("1/yieldtotal", d.pv0_tE,3);
        opub("2/power",      d.pv1_p, 1);  opub("2/voltage",    d.pv1_v, 1);
        opub("2/current",    d.pv1_i, 2);  opub("2/yieldday",   d.pv1_dE,3);
        opub("2/yieldtotal", d.pv1_tE,3);
        char buf[16];
        snprintf(buf,sizeof(buf),"%d",d.powerLimit);
        mqttClient.publish((base+"status/limit_relative").c_str(),buf,appConfig.mqttRetain);
        snprintf(buf,sizeof(buf),"%d",d.wifiRssi);
        mqttClient.publish((base+"dtu/rssi").c_str(),buf,appConfig.mqttRetain);
    } else {
        char buf[16];
        snprintf(buf,sizeof(buf),"%lu",(unsigned long)d.timestamp);
        pub("timestamp", buf, false);
        pubFloat("grid/U",            d.grid_v, 1);
        pubFloat("grid/I",            d.grid_i, 2);
        pubFloat("grid/P",            d.grid_p, 1);
        pubFloat("grid/dailyEnergy",  d.grid_dE,3);
        pubFloat("grid/totalEnergy",  d.grid_tE,3);
        pubFloat("pv0/U",             d.pv0_v, 1);
        pubFloat("pv0/I",             d.pv0_i, 2);
        pubFloat("pv0/P",             d.pv0_p, 1);
        pubFloat("pv0/dailyEnergy",   d.pv0_dE,3);
        pubFloat("pv1/U",             d.pv1_v, 1);
        pubFloat("pv1/I",             d.pv1_i, 2);
        pubFloat("pv1/P",             d.pv1_p, 1);
        pubFloat("pv1/dailyEnergy",   d.pv1_dE,3);
        pubFloat("inverter/Temp",          d.temp,   1);
        pubInt  ("inverter/PowerLimit",    d.powerLimit);
        pubInt  ("inverter/dtuConnState",  d.dtuConnState);
        pubInt  ("inverter/warningsActive",d.warningsActive);
        pubInt  ("inverter/WifiRSSI",      d.wifiRssi);
    }
    LOG_D(MOD_MQTT, "Published DTU data");
}

// ─── GPIO state ───────────────────────────────────────────────────────────────
void mqttPublishGpioState() {
    if (!mqttClient.connected()) return;
    pub("relay/state", gpioState.relay ? "1" : "0", appConfig.mqttRetain);
    for (int i = 0; i < 4; i++) {
        char topic[24]; snprintf(topic,sizeof(topic),"gpio%d/state",i+1);
        pub(topic, gpioState.gpio[i] ? "1" : "0", appConfig.mqttRetain);
    }
}

// ─── HA Auto-Discovery ────────────────────────────────────────────────────────
static void publishHaSensor(const char* uid, const char* name,
                             const char* topic, const char* unit,
                             const char* devClass) {
    char discTopic[128];
    snprintf(discTopic,sizeof(discTopic),
             "homeassistant/sensor/%s_%s/config", appConfig.mqttTopic, uid);
    JsonDocument doc;
    doc["name"]       = name;
    doc["unique_id"]  = String(appConfig.mqttTopic) + "_" + uid;
    doc["state_topic"]= T(topic);
    if (unit)     doc["unit_of_measurement"] = unit;
    if (devClass) doc["device_class"]        = devClass;
    doc["device"]["identifiers"][0] = appConfig.mqttTopic;
    doc["device"]["name"]           = "HMS-800W-2T";
    doc["device"]["manufacturer"]   = "Hoymiles";
    doc["device"]["sw_version"]     = FW_VERSION;
    String payload; serializeJson(doc, payload);
    mqttClient.publish(discTopic, payload.c_str(), true);
}

static void publishHaSwitch(const char* uid, const char* name,
                             const char* stateTopic, const char* cmdTopic) {
    char discTopic[128];
    snprintf(discTopic,sizeof(discTopic),
             "homeassistant/switch/%s_%s/config", appConfig.mqttTopic, uid);
    JsonDocument doc;
    doc["name"]          = name;
    doc["unique_id"]     = String(appConfig.mqttTopic) + "_" + uid;
    doc["state_topic"]   = T(stateTopic);
    doc["command_topic"] = T(cmdTopic);
    doc["payload_on"]    = "1";
    doc["payload_off"]   = "0";
    doc["device"]["identifiers"][0] = appConfig.mqttTopic;
    doc["device"]["name"]           = "HMS-800W-2T";
    doc["device"]["manufacturer"]   = "Hoymiles";
    String payload; serializeJson(doc, payload);
    mqttClient.publish(discTopic, payload.c_str(), true);
}

// HA discovery sent one item per loop iteration via index counter
static int _haDiscoveryIdx = -1;  // -1 = idle, 0..N = sending

void mqttSendHaDiscovery() {
    // HA discovery disabled — PubSubClient.connect() already blocks 1s+
    // which starves the WiFi watchdog on Core 0.
    // Will be re-enabled as separate task on Core 1 in future release.
    return;
}

// Called once per loop iteration — sends one item and returns
static void haDiscoveryStep() {
    // HA discovery disabled: PubSubClient publish() on Core 0 starves WiFi watchdog
    // Home Assistant will auto-discover via MQTT topics without explicit discovery
    _haDiscoveryIdx = -1;
    return;
    if (_haDiscoveryIdx < 0 || !mqttClient.connected()) return;
    switch (_haDiscoveryIdx) {
        case  0: publishHaSensor("grid_p",      "Grid Power",        "grid/P",            "W",   "power");       break;
        case  1: publishHaSensor("grid_u",      "Grid Voltage",      "grid/U",            "V",   "voltage");     break;
        case  2: publishHaSensor("grid_i",      "Grid Current",      "grid/I",            "A",   "current");     break;
        case  3: publishHaSensor("grid_dE",     "Grid Energy Today", "grid/dailyEnergy",  "kWh", "energy");      break;
        case  4: publishHaSensor("grid_tE",     "Grid Energy Total", "grid/totalEnergy",  "kWh", "energy");      break;
        case  5: publishHaSensor("pv0_p",       "PV1 Power",         "pv0/P",             "W",   "power");       break;
        case  6: publishHaSensor("pv0_u",       "PV1 Voltage",       "pv0/U",             "V",   "voltage");     break;
        case  7: publishHaSensor("pv1_p",       "PV2 Power",         "pv1/P",             "W",   "power");       break;
        case  8: publishHaSensor("pv1_u",       "PV2 Voltage",       "pv1/U",             "V",   "voltage");     break;
        case  9: publishHaSensor("temp",        "Inverter Temp",     "inverter/Temp",     "°C",  "temperature"); break;
        case 10: publishHaSensor("power_limit", "Power Limit",       "inverter/PowerLimit","%",  nullptr);       break;
        case 11: publishHaSwitch("relay",  "Relay",  "relay/state",  "relay/set");  break;
        case 12: publishHaSwitch("gpio1",  "GPIO 1", "gpio1/state",  "gpio1/set");  break;
        case 13: publishHaSwitch("gpio2",  "GPIO 2", "gpio2/state",  "gpio2/set");  break;
        case 14: publishHaSwitch("gpio3",  "GPIO 3", "gpio3/state",  "gpio3/set");  break;
        case 15: publishHaSwitch("gpio4",  "GPIO 4", "gpio4/state",  "gpio4/set");  break;
        case 16:
            LOG_I(MOD_MQTT, "HA discovery sent");
            _haDiscoveryIdx = -1;  // done
            return;
    }
    _haDiscoveryIdx++;
}

// ─── Inbound message callback ─────────────────────────────────────────────────
static void onMessage(char* topic, byte* payload, unsigned int len) {
    char val[64] = {};
    strlcpy(val, (char*)payload, min((unsigned int)sizeof(val)-1, len)+1);

    String t    = String(topic);
    String base = String(appConfig.mqttTopic) + "/";

    if      (t == base+"inverter/PowerLimitSet/set") { dtuSetPowerLimit(atoi(val)); }
    else if (t == base+"inverter/RebootDtu/set")     { dtuRequestReboot(); }
    else if (t == base+"inverter/RebootGw/set")      { vTaskDelay(pdMS_TO_TICKS(200)); ESP.restart(); }
    else if (t == base+"inverter/On/set")            { dtuSetInverterOn(atoi(val)==1); }
    else if (t == base+"relay/set") {
        GpioCommand_t cmd = {0, atoi(val)==1};
        xQueueSend(gpioCommandQueue, &cmd, 0);
    } else {
        for (int i = 1; i <= 4; i++) {
            char gTopic[32]; snprintf(gTopic,sizeof(gTopic),"gpio%d/set",i);
            if (t == base+gTopic) {
                GpioCommand_t cmd = {i, atoi(val)==1};
                xQueueSend(gpioCommandQueue, &cmd, 0);
            }
        }
    }
}

// ─── Connect helper ───────────────────────────────────────────────────────────
static bool mqttConnect() {
    char clientId[32];
    snprintf(clientId,sizeof(clientId),"hmsgws3_%06llX",
             (unsigned long long)(ESP.getEfuseMac() & 0xFFFFFF));
    WiFi.setSleep(false);

    // Pre-connect TCP with explicit 2s timeout to avoid watchdog starvation
    // PubSubClient.connect() calls WiFiClient.connect() without timeout (blocks forever)
    // By pre-connecting we skip that blocking call
    if (!wifiClient.connected()) {
        if (!wifiClient.connect(appConfig.mqttHost, appConfig.mqttPort, 2000)) {
            LOG_W(MOD_MQTT, "TCP connect failed");
            return false;
        }
    }

    bool ok;
    if (strlen(appConfig.mqttUser) > 0)
        ok = mqttClient.connect(clientId, appConfig.mqttUser, appConfig.mqttPass);
    else
        ok = mqttClient.connect(clientId);

    if (ok) {
        LOG_I(MOD_MQTT, "Connected to %s:%d", appConfig.mqttHost, appConfig.mqttPort);
        xEventGroupSetBits(systemStateEvents, EVT_MQTT_CONNECTED);
        setLedState(LED_OPERATIONAL);

        // Subscribe to control topics
        String base = String(appConfig.mqttTopic) + "/";
        mqttClient.subscribe((base+"inverter/PowerLimitSet/set").c_str());
        mqttClient.subscribe((base+"inverter/RebootDtu/set").c_str());
        mqttClient.subscribe((base+"inverter/RebootGw/set").c_str());
        mqttClient.subscribe((base+"inverter/On/set").c_str());
        mqttClient.subscribe((base+"relay/set").c_str());
        for (int i = 1; i <= 4; i++)
            mqttClient.subscribe((base+"gpio"+i+"/set").c_str());

        mqttSendHaDiscovery();
        mqttPublishGpioState();
    } else {
        LOG_W(MOD_MQTT, "Connect failed (rc=%d). Retry in %ds",
              mqttClient.state(), MQTT_RECONNECT_MS/1000);
        xEventGroupClearBits(systemStateEvents, EVT_MQTT_CONNECTED);
    }
    return ok;
}

// ─── Task ─────────────────────────────────────────────────────────────────────
void taskMQTT(void* pvParameters) {
    // MQTT temporarily disabled — PubSubClient.connect() triggers interrupt
    // watchdog on ESP32-S3. Will be re-implemented with non-blocking approach.
    LOG_W(MOD_MQTT, "MQTT disabled pending non-blocking implementation");
    vTaskDelete(nullptr);
    return;

    if (strlen(appConfig.mqttHost) == 0) {
        LOG_W(MOD_MQTT, "No MQTT broker configured, task idle");
        vTaskDelete(nullptr); return;
    }

    mqttClient.setServer(appConfig.mqttHost, appConfig.mqttPort);
    mqttClient.setCallback(onMessage);
    mqttClient.setKeepAlive(MQTT_KEEPALIVE_S);
    mqttClient.setBufferSize(2048);

    LOG_I(MOD_MQTT, "Connecting to %s:%d", appConfig.mqttHost, appConfig.mqttPort);

    // Wait for WiFi
    xEventGroupWaitBits(systemStateEvents, EVT_WIFI_CONNECTED,
                        pdFALSE, pdTRUE, portMAX_DELAY);

    uint32_t lastReconnect = 0;
    uint32_t lastSysMs     = 0;
    uint32_t lastGpioCheck = 0;
    GpioState_t lastGpio   = {};
    DtuData_t   data       = {};

    for (;;) {
        // ── Maintain connection ────────────────────────────────────────────
        if (!mqttClient.connected()) {
            xEventGroupClearBits(systemStateEvents, EVT_MQTT_CONNECTED);
            uint32_t now = millis();
            if (now - lastReconnect >= 10000UL) {  // 10s between retries
                lastReconnect = now;
                LOG_I(MOD_MQTT, "Reconnecting...");
                mqttConnect();
            }
        } // reconnect

        // ── Publish DTU data ───────────────────────────────────────────────
        if (xQueueReceive(dtuDataQueue, &data, 0) == pdTRUE) {
            if (mqttClient.connected()) publishDtuData(data);
        }

        // ── GPIO state on change ───────────────────────────────────────────
        uint32_t now = millis();
        if (now - lastGpioCheck > 250) {
            lastGpioCheck = now;
            if (memcmp(&gpioState, &lastGpio, sizeof(GpioState_t)) != 0) {
                mqttPublishGpioState();
                memcpy(&lastGpio, &gpioState, sizeof(GpioState_t));
            }
        }

        // ── System stats every 60 s ────────────────────────────────────────
        if (now - lastSysMs > 60000 && mqttClient.connected()) {
            lastSysMs = now;
            char buf[16];
            snprintf(buf,sizeof(buf),"%lu",(unsigned long)(now/1000));
            pub("system/uptime", buf, false);
            snprintf(buf,sizeof(buf),"%d",(int)WiFi.RSSI());
            pub("system/rssi",   buf, false);
            snprintf(buf,sizeof(buf),"%lu",(unsigned long)ESP.getFreeHeap());
            pub("system/heap",   buf, false);
        }

        // Send one HA discovery item per iteration (non-blocking spread)
        haDiscoveryStep();

        mqttClient.loop();
        vTaskDelay(pdMS_TO_TICKS(100));  // 100ms — plenty of time for WiFi keepalive
    }
}
