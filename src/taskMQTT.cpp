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
#include <AsyncMqttClient.h>
#include <ArduinoJson.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/event_groups.h>

static AsyncMqttClient mqttClient;
static volatile bool   _mqttConnected  = false;
static volatile bool   _needsDiscovery = false;
static TimerHandle_t   _reconnectTimer = nullptr;

// ─── Topic helpers ────────────────────────────────────────────────────────────
static String T(const char* suffix) {
    return String(appConfig.mqttTopic) + "/" + suffix;
}

// Standard-mode publish
static void pub(const char* suffix, const char* payload, bool retain = false) {
    if (!_mqttConnected) return;
    String topic = T(suffix);
    mqttClient.publish(topic.c_str(), appConfig.mqttQos, retain, payload);
}

static void pubFloat(const char* suffix, float val, int decimals = 2) {
    char buf[24];
    dtostrf(val, 1, decimals, buf);
    pub(suffix, buf, appConfig.mqttRetain);
}

static void pubInt(const char* suffix, int val) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", val);
    pub(suffix, buf, appConfig.mqttRetain);
}

// ─── Publish full DTU dataset ─────────────────────────────────────────────────
static void publishDtuData(const DtuData_t& d) {
    if (!_mqttConnected) return;

    if (appConfig.mqttOpenDtu) {
        // OpenDTU-compatible topic structure
        String base = String(appConfig.mqttTopic) + "/";
        auto opub = [&](const char* path, float val, int dec=2) {
            char buf[24]; dtostrf(val, 1, dec, buf);
            mqttClient.publish((base + path).c_str(), appConfig.mqttQos,
                               appConfig.mqttRetain, buf);
        };
        opub("0/power",       d.grid_p, 1);
        opub("0/voltage",     d.grid_v, 1);
        opub("0/current",     d.grid_i, 2);
        opub("0/yieldday",    d.grid_dE, 3);
        opub("0/yieldtotal",  d.grid_tE, 3);
        opub("0/temperatur",  d.temp, 1);
        opub("1/power",       d.pv0_p, 1);
        opub("1/voltage",     d.pv0_v, 1);
        opub("1/current",     d.pv0_i, 2);
        opub("1/yieldday",    d.pv0_dE, 3);
        opub("1/yieldtotal",  d.pv0_tE, 3);
        opub("2/power",       d.pv1_p, 1);
        opub("2/voltage",     d.pv1_v, 1);
        opub("2/current",     d.pv1_i, 2);
        opub("2/yieldday",    d.pv1_dE, 3);
        opub("2/yieldtotal",  d.pv1_tE, 3);
        char buf[16];
        snprintf(buf, sizeof(buf), "%d", d.powerLimit);
        mqttClient.publish((base + "status/limit_relative").c_str(),
                           appConfig.mqttQos, appConfig.mqttRetain, buf);
        snprintf(buf, sizeof(buf), "%d", d.wifiRssi);
        mqttClient.publish((base + "dtu/rssi").c_str(),
                           appConfig.mqttQos, appConfig.mqttRetain, buf);
    } else {
        // Standard mode
        char buf[16];
        snprintf(buf, sizeof(buf), "%lu", (unsigned long)d.timestamp);
        pub("timestamp", buf, false);

        pubFloat("grid/U",           d.grid_v, 1);
        pubFloat("grid/I",           d.grid_i, 2);
        pubFloat("grid/P",           d.grid_p, 1);
        pubFloat("grid/dailyEnergy", d.grid_dE, 3);
        pubFloat("grid/totalEnergy", d.grid_tE, 3);

        pubFloat("pv0/U",            d.pv0_v, 1);
        pubFloat("pv0/I",            d.pv0_i, 2);
        pubFloat("pv0/P",            d.pv0_p, 1);
        pubFloat("pv0/dailyEnergy",  d.pv0_dE, 3);

        pubFloat("pv1/U",            d.pv1_v, 1);
        pubFloat("pv1/I",            d.pv1_i, 2);
        pubFloat("pv1/P",            d.pv1_p, 1);
        pubFloat("pv1/dailyEnergy",  d.pv1_dE, 3);

        pubFloat("inverter/Temp",          d.temp, 1);
        pubInt  ("inverter/PowerLimit",    d.powerLimit);
        pubInt  ("inverter/dtuConnState",  d.dtuConnState);
        pubInt  ("inverter/warningsActive",d.warningsActive);
        pubInt  ("inverter/WifiRSSI",      d.wifiRssi);
    }

    LOG_D(MOD_MQTT, "Published DTU data");
}

// ─── Publish GPIO state ───────────────────────────────────────────────────────
void mqttPublishGpioState() {
    if (!_mqttConnected) return;
    pub("relay/state", gpioState.relay ? "1" : "0", appConfig.mqttRetain);
    for (int i = 0; i < 4; i++) {
        char topic[24];
        snprintf(topic, sizeof(topic), "gpio%d/state", i+1);
        pub(topic, gpioState.gpio[i] ? "1" : "0", appConfig.mqttRetain);
    }
}

// ─── Home Assistant Auto-Discovery ────────────────────────────────────────────
static void publishHaSensor(const char* uid, const char* name, const char* topic,
                             const char* unit, const char* devClass) {
    char discTopic[128];
    snprintf(discTopic, sizeof(discTopic),
             "homeassistant/sensor/%s_%s/config",
             appConfig.mqttTopic, uid);

    DynamicJsonDocument doc(512);
    doc["name"]              = name;
    doc["unique_id"]         = String(appConfig.mqttTopic) + "_" + uid;
    doc["state_topic"]       = T(topic);
    if (unit)    doc["unit_of_measurement"] = unit;
    if (devClass) doc["device_class"]       = devClass;
    doc["device"]["identifiers"][0]  = appConfig.mqttTopic;
    doc["device"]["name"]            = latestDtuData.inverterModel[0]
                                       ? latestDtuData.inverterModel : "HMS-800W-2T";
    doc["device"]["manufacturer"]    = "Hoymiles";
    doc["device"]["model"]           = "HMS-800W-2T";
    doc["device"]["sw_version"]      = FW_VERSION;

    char payload[512];
    serializeJson(doc, payload, sizeof(payload));
    mqttClient.publish(discTopic, 1, true, payload);
}

static void publishHaSwitch(const char* uid, const char* name,
                             const char* stateTopic, const char* cmdTopic) {
    char discTopic[128];
    snprintf(discTopic, sizeof(discTopic),
             "homeassistant/switch/%s_%s/config",
             appConfig.mqttTopic, uid);

    DynamicJsonDocument doc(512);
    doc["name"]           = name;
    doc["unique_id"]      = String(appConfig.mqttTopic) + "_" + uid;
    doc["state_topic"]    = T(stateTopic);
    doc["command_topic"]  = T(cmdTopic);
    doc["payload_on"]     = "1";
    doc["payload_off"]    = "0";
    doc["device"]["identifiers"][0] = appConfig.mqttTopic;
    doc["device"]["name"]           = "HMS-800W-2T";
    doc["device"]["manufacturer"]   = "Hoymiles";

    char payload[512];
    serializeJson(doc, payload, sizeof(payload));
    mqttClient.publish(discTopic, 1, true, payload);
}

void mqttSendHaDiscovery() {
    if (!_mqttConnected || !appConfig.mqttHaDiscovery || appConfig.mqttOpenDtu) return;
    LOG_I(MOD_MQTT, "Sending HA auto-discovery...");

    publishHaSensor("grid_p",      "Grid Power",        "grid/P",           "W",   "power");
    publishHaSensor("grid_u",      "Grid Voltage",      "grid/U",           "V",   "voltage");
    publishHaSensor("grid_i",      "Grid Current",      "grid/I",           "A",   "current");
    publishHaSensor("grid_dE",     "Grid Energy Today", "grid/dailyEnergy", "kWh", "energy");
    publishHaSensor("grid_tE",     "Grid Energy Total", "grid/totalEnergy", "kWh", "energy");
    publishHaSensor("pv0_p",       "PV1 Power",         "pv0/P",            "W",   "power");
    publishHaSensor("pv0_u",       "PV1 Voltage",       "pv0/U",            "V",   "voltage");
    publishHaSensor("pv1_p",       "PV2 Power",         "pv1/P",            "W",   "power");
    publishHaSensor("pv1_u",       "PV2 Voltage",       "pv1/U",            "V",   "voltage");
    publishHaSensor("temp",        "Inverter Temp",     "inverter/Temp",    "°C",  "temperature");
    publishHaSensor("power_limit", "Power Limit",       "inverter/PowerLimit", "%", nullptr);

    publishHaSwitch("relay",  "Relay",  "relay/state",  "relay/set");
    publishHaSwitch("gpio1",  "GPIO 1", "gpio1/state",  "gpio1/set");
    publishHaSwitch("gpio2",  "GPIO 2", "gpio2/state",  "gpio2/set");
    publishHaSwitch("gpio3",  "GPIO 3", "gpio3/state",  "gpio3/set");
    publishHaSwitch("gpio4",  "GPIO 4", "gpio4/state",  "gpio4/set");

    LOG_I(MOD_MQTT, "HA discovery sent");
}

// ─── Subscribe to control topics ──────────────────────────────────────────────
static void subscribeAll() {
    String base = String(appConfig.mqttTopic) + "/";
    mqttClient.subscribe((base + "inverter/PowerLimitSet/set").c_str(), 1);
    mqttClient.subscribe((base + "inverter/RebootDtu/set").c_str(), 1);
    mqttClient.subscribe((base + "inverter/RebootGw/set").c_str(), 1);
    mqttClient.subscribe((base + "inverter/On/set").c_str(), 1);
    mqttClient.subscribe((base + "relay/set").c_str(), 1);
    for (int i = 1; i <= 4; i++) {
        mqttClient.subscribe((base + "gpio" + i + "/set").c_str(), 1);
    }
    LOG_D(MOD_MQTT, "Subscribed to control topics");
}

// ─── MQTT callbacks ───────────────────────────────────────────────────────────
static void onMqttConnect(bool sessionPresent) {
    _mqttConnected = true;
    _needsDiscovery = true;
    xEventGroupSetBits(systemStateEvents, EVT_MQTT_CONNECTED);
    setLedState(LED_OPERATIONAL);
    LOG_I(MOD_MQTT, "Connected to %s:%d", appConfig.mqttHost, appConfig.mqttPort);
    subscribeAll();
}

static void onMqttDisconnect(AsyncMqttClientDisconnectReason reason) {
    _mqttConnected = false;
    xEventGroupClearBits(systemStateEvents, EVT_MQTT_CONNECTED);
    LOG_W(MOD_MQTT, "Disconnected (reason %d). Reconnect in %dms",
          (int)reason, MQTT_RECONNECT_MS);
    if (xTimerIsTimerActive(_reconnectTimer) == pdFALSE)
        xTimerStart(_reconnectTimer, 0);
}

static void onMqttMessage(char* topic, char* payload,
                          AsyncMqttClientMessageProperties props,
                          size_t len, size_t index, size_t total) {
    char val[64] = {};
    strlcpy(val, payload, min(len + 1, sizeof(val)));

    String t = String(topic);
    String base = String(appConfig.mqttTopic) + "/";

    if (t == base + "inverter/PowerLimitSet/set") {
        int pct = atoi(val);
        dtuSetPowerLimit(pct);
        LOG_I(MOD_MQTT, "CMD: PowerLimit -> %d%%", pct);
    } else if (t == base + "inverter/RebootDtu/set") {
        dtuRequestReboot();
        LOG_I(MOD_MQTT, "CMD: RebootDTU");
    } else if (t == base + "inverter/RebootGw/set") {
        LOG_I(MOD_MQTT, "CMD: RebootGateway");
        vTaskDelay(pdMS_TO_TICKS(500));
        ESP.restart();
    } else if (t == base + "inverter/On/set") {
        bool on = (atoi(val) == 1);
        dtuSetInverterOn(on);
        LOG_I(MOD_MQTT, "CMD: InverterOn -> %s", on ? "true" : "false");
    } else if (t == base + "relay/set") {
        bool s = (atoi(val) == 1);
        GpioCommand_t cmd = {0, s};
        xQueueSend(gpioCommandQueue, &cmd, 0);
        LOG_I(MOD_MQTT, "CMD: relay -> %s", s ? "ON" : "OFF");
    } else {
        for (int i = 1; i <= 4; i++) {
            char gTopic[32];
            snprintf(gTopic, sizeof(gTopic), "gpio%d/set", i);
            if (t == base + gTopic) {
                bool s = (atoi(val) == 1);
                GpioCommand_t cmd = {i, s};
                xQueueSend(gpioCommandQueue, &cmd, 0);
                LOG_I(MOD_MQTT, "CMD: gpio%d -> %s", i, s ? "HIGH" : "LOW");
            }
        }
    }
}

// ─── Reconnect timer callback ─────────────────────────────────────────────────
static void reconnectCallback(TimerHandle_t t) {
    if (!_mqttConnected &&
        (xEventGroupGetBits(systemStateEvents) & EVT_WIFI_CONNECTED)) {
        LOG_I(MOD_MQTT, "Reconnecting...");
        mqttClient.connect();
    }
}

// ─── Task ─────────────────────────────────────────────────────────────────────
void taskMQTT(void* pvParameters) {
    if (strlen(appConfig.mqttHost) == 0) {
        LOG_W(MOD_MQTT, "No MQTT broker configured, task idle");
        vTaskDelete(nullptr);
        return;
    }

    _reconnectTimer = xTimerCreate("mqttRecon", pdMS_TO_TICKS(MQTT_RECONNECT_MS),
                                   pdTRUE, nullptr, reconnectCallback);

    mqttClient.onConnect(onMqttConnect);
    mqttClient.onDisconnect(onMqttDisconnect);
    mqttClient.onMessage(onMqttMessage);
    mqttClient.setServer(appConfig.mqttHost, appConfig.mqttPort);
    mqttClient.setKeepAlive(MQTT_KEEPALIVE_S);
    mqttClient.setCleanSession(false);

    // Set credentials if provided
    if (strlen(appConfig.mqttUser) > 0)
        mqttClient.setCredentials(appConfig.mqttUser, appConfig.mqttPass);

    // Unique client ID from chip MAC
    char clientId[32];
    snprintf(clientId, sizeof(clientId), "hmsgws3_%06llX",
             (unsigned long long)(ESP.getEfuseMac() & 0xFFFFFF));
    mqttClient.setClientId(clientId);

    LOG_I(MOD_MQTT, "Connecting to %s:%d as %s",
          appConfig.mqttHost, appConfig.mqttPort, clientId);

    // Wait for WiFi then connect
    xEventGroupWaitBits(systemStateEvents, EVT_WIFI_CONNECTED,
                        pdFALSE, pdTRUE, portMAX_DELAY);
    mqttClient.connect();

    DtuData_t data = {};
    uint32_t  lastSysMs = 0;
    uint32_t  lastGpioCheck = 0;
    GpioState_t lastGpio = {};

    for (;;) {
        // Send HA discovery after (re)connect
        if (_needsDiscovery && _mqttConnected) {
            _needsDiscovery = false;
            mqttSendHaDiscovery();
            mqttPublishGpioState();
        }

        // Receive and publish DTU data
        if (xQueueReceive(dtuDataQueue, &data, pdMS_TO_TICKS(100)) == pdTRUE) {
            if (_mqttConnected) publishDtuData(data);
        }

        // Publish GPIO state on change
        uint32_t now = millis();
        if (now - lastGpioCheck > 250) {
            lastGpioCheck = now;
            if (memcmp(&gpioState, &lastGpio, sizeof(GpioState_t)) != 0) {
                mqttPublishGpioState();
                memcpy(&lastGpio, &gpioState, sizeof(GpioState_t));
            }
        }

        // System stats every 60 s
        if (now - lastSysMs > 60000 && _mqttConnected) {
            lastSysMs = now;
            char buf[16];
            snprintf(buf, sizeof(buf), "%lu", (unsigned long)(now / 1000));
            pub("system/uptime", buf, false);
            snprintf(buf, sizeof(buf), "%d",  (int)WiFi.RSSI());
            pub("system/rssi",   buf, false);
            snprintf(buf, sizeof(buf), "%lu", (unsigned long)ESP.getFreeHeap());
            pub("system/heap",   buf, false);
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
