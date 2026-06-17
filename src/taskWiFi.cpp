#include "taskWiFi.h"
#include "dataStore.h"
#include "appConfig.h"
#include "systemState.h"
#include "config.h"
#include "logger.h"
#include <WiFi.h>
#include <esp_wifi.h>
#include <time.h>

static const uint32_t CONNECT_TIMEOUT_MS   = 15000;
static const uint32_t MONITOR_INTERVAL_MS  = 5000;
static const uint32_t NTP_SYNC_TIMEOUT_MS  = 10000;
static const uint32_t NTP_REFRESH_MS       = 3600000UL;  // 1 Stunde

// --- Hilfsfunktionen ----------------------------------------------------------

static void updateSystemWifi(bool connected, bool apMode) {
    DataStore::SystemStatus sys = dsGetSystem();
    sys.wifiConnected = connected;
    sys.wifiApMode    = apMode;
    if (connected) {
        sys.wifiIp   = WiFi.localIP().toString();
        sys.wifiRssi = (int8_t)WiFi.RSSI();
        sys.wifiSsid = String(appConfig.wifiSsid);
    } else if (apMode) {
        sys.wifiIp   = WiFi.softAPIP().toString();
        sys.wifiRssi = 0;
        sys.wifiSsid = String(AP_DEFAULT_SSID);
    }
    dsSetSystem(sys);
}

static void syncNtp() {
    LOG_I(MOD_WIFI, "NTP sync  -  server: %s  tz: %d s", appConfig.ntpServer, appConfig.tzOffset);
    configTime(appConfig.tzOffset, 0, appConfig.ntpServer);

    uint32_t startMs = millis();
    time_t   now     = 0;
    while (now < 1700000000UL && (millis() - startMs) < NTP_SYNC_TIMEOUT_MS) {
        vTaskDelay(pdMS_TO_TICKS(500));
        now = time(nullptr);
    }

    if (now > 1700000000UL) {
        LOG_I(MOD_WIFI, "NTP synced  -  %lu", (unsigned long)now);
        DataStore::SystemStatus sys = dsGetSystem();
        sys.ntpTime = (uint32_t)now;
        dsSetSystem(sys);
    } else {
        LOG_W(MOD_WIFI, "NTP sync timeout");
    }
}

static void startApMode() {
    LOG_I(MOD_WIFI, "Starting AP  -  SSID: %s", AP_DEFAULT_SSID);
    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_DEFAULT_SSID);

    updateSystemWifi(false, true);
    xEventGroupClearBits(systemStateEvents, EVT_WIFI_CONNECTED);
    xEventGroupSetBits(systemStateEvents, EVT_WIFI_AP_MODE);
}

// --- Task ---------------------------------------------------------------------

void taskWiFi(void* pvParameters) {
    LOG_I(MOD_WIFI, "Task started (Core %d)", xPortGetCoreID());

    for (;;) {
        // -- AP-Modus aktiv: kein weiterer Reconnect-Versuch -------------------
        if (xEventGroupGetBits(systemStateEvents) & EVT_WIFI_AP_MODE) {
            vTaskDelay(pdMS_TO_TICKS(MONITOR_INTERVAL_MS));
            continue;
        }

        // -- STA-Verbindungsversuch ---------------------------------------------
        WiFi.mode(WIFI_STA);
        WiFi.begin(appConfig.wifiSsid, appConfig.wifiPass);
        LOG_I(MOD_WIFI, "Connecting to '%s'...", appConfig.wifiSsid);

        uint32_t startMs = millis();
        while (WiFi.status() != WL_CONNECTED && (millis() - startMs) < CONNECT_TIMEOUT_MS) {
            vTaskDelay(pdMS_TO_TICKS(500));
        }

        if (WiFi.status() != WL_CONNECTED) {
            WiFi.disconnect(true);
            LOG_W(MOD_WIFI, "Connection failed after %lu ms", (unsigned long)(millis() - startMs));

            if (appConfig.wifiApFallback) {
                startApMode();
            } else {
                vTaskDelay(pdMS_TO_TICKS(10000));
            }
            continue;
        }

        // -- Verbunden ---------------------------------------------------------
        LOG_I(MOD_WIFI, "Connected  -  IP: %s  RSSI: %d dBm",
              WiFi.localIP().toString().c_str(), WiFi.RSSI());

        // MAC is only valid after WiFi driver is started
        {
            String mac = WiFi.macAddress();
            mac.replace(":", "");
            DataStore::SystemStatus sys = dsGetSystem();
            sys.macAddress = mac;
            dsSetSystem(sys);
        }

        updateSystemWifi(true, false);
        xEventGroupSetBits(systemStateEvents, EVT_WIFI_CONNECTED);
        xEventGroupClearBits(systemStateEvents, EVT_WIFI_AP_MODE);

        syncNtp();

        uint32_t lastMonitorMs = 0;
        uint32_t lastNtpMs     = millis();

        // -- Verbindungs-Monitor -----------------------------------------------
        while (WiFi.status() == WL_CONNECTED) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            uint32_t now = millis();

            if (now - lastMonitorMs >= MONITOR_INTERVAL_MS) {
                lastMonitorMs = now;
                DataStore::SystemStatus sys = dsGetSystem();
                sys.wifiRssi = (int8_t)WiFi.RSSI();
                sys.wifiIp   = WiFi.localIP().toString();
                time_t t = time(nullptr);
                if (t > 1700000000UL) sys.ntpTime = (uint32_t)t;
                dsSetSystem(sys);
                LOG_D(MOD_WIFI, "RSSI: %d dBm", (int)WiFi.RSSI());
            }

            if (now - lastNtpMs >= NTP_REFRESH_MS) {
                lastNtpMs = now;
                syncNtp();
            }
        }

        // -- Verbindung verloren -----------------------------------------------
        LOG_W(MOD_WIFI, "Connection lost");
        xEventGroupClearBits(systemStateEvents, EVT_WIFI_CONNECTED);
        updateSystemWifi(false, false);
    }
}
