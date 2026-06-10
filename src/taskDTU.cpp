#include "taskDTU.h"
#include "config.h"
#include "appConfig.h"
#include "systemState.h"
#include "logger.h"
#include "taskNeoPixel.h"
#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/event_groups.h>

// ─── NanoPB / Protobuf includes ───────────────────────────────────────────────
// These match the proto definitions from ohAnd/dtuGateway
// Place generated .pb.h/.pb.c files in include/proto/
#include "proto/hoymiles.pb.h"
#include <pb_decode.h>
#include <pb_encode.h>

// ─── DTU URL helpers ──────────────────────────────────────────────────────────
static String dtuBaseUrl() {
    return String("http://") + appConfig.dtuHost + ":" + appConfig.dtuPort;
}

// ─── Shared latest data ───────────────────────────────────────────────────────
DtuData_t latestDtuData = {};
bool      dtuDataValid  = false;

// ─── Control flags (set from other tasks) ────────────────────────────────────
static volatile bool     _reqDtuReboot   = false;
static volatile bool     _reqMiReboot    = false;
static volatile bool     _reqPowerLimit  = false;
static volatile int      _pendingLimit   = 100;
static volatile bool     _reqInverterOn  = false;
static volatile bool     _pendingInvOn   = true;

void dtuRequestReboot()              { _reqDtuReboot  = true; }
void dtuRequestInverterReboot()      { _reqMiReboot   = true; }
void dtuSetPowerLimit(int percent)   { _pendingLimit  = percent; _reqPowerLimit = true; }
void dtuSetInverterOn(bool on)       { _pendingInvOn  = on;      _reqInverterOn = true; }

// ─── HTTP helpers ─────────────────────────────────────────────────────────────
static bool httpPost(const char* path, const uint8_t* body, size_t len,
                     uint8_t* respBuf, size_t* respLen) {
    HTTPClient http;
    String url = dtuBaseUrl() + path;
    http.begin(url);
    http.addHeader("Content-Type", "application/x-protobuf");
    http.setTimeout(DTU_CONNECT_TIMEOUT_MS);

    int code = http.POST((uint8_t*)body, len);
    if (code != 200) {
        LOG_W(MOD_DTU, "HTTP %s -> %d", path, code);
        http.end();
        return false;
    }
    if (respBuf && respLen) {
        WiFiClient* stream = http.getStreamPtr();
        size_t avail = http.getSize();
        if (avail > 0 && avail <= *respLen) {
            *respLen = stream->readBytes(respBuf, avail);
        } else {
            *respLen = 0;
        }
    }
    http.end();
    return true;
}

// ─── Parse device info response ───────────────────────────────────────────────
static void parseDeviceInfo(const uint8_t* buf, size_t len, DtuData_t& d) {
    // Simplified extraction — adjust field IDs to match actual hoymiles.proto
    // Field 2 = dtu_version, Field 3 = inverter_version, Field 4 = inverter_serial
    // Full decoding requires generated NanoPB code from hoymiles.proto
    // For now we copy from dtuGateway's parsing logic
    (void)buf; (void)len; // placeholder
    strlcpy(d.dtuVersionStr,      "unknown", sizeof(d.dtuVersionStr));
    strlcpy(d.inverterVersionStr, "unknown", sizeof(d.inverterVersionStr));
    strlcpy(d.inverterModel,      "HMS-800W-2T", sizeof(d.inverterModel));
}

// ─── Parse real-time data response ────────────────────────────────────────────
static bool parseRealData(const uint8_t* buf, size_t len, DtuData_t& d) {
    // Parsing the Hoymiles protobuf response.
    // Field mapping from dtuGateway dtuConst.h / dtuInterface.cpp:
    //   RealDataNew message contains:
    //     repeated PVData pv (field 3): voltage, current, power, energy_daily, energy_total
    //     repeated GridData grid (field 2): same structure
    //     float temperature (field 7)
    //     int32 power_limit_pct (field 8)
    //     int32 wifi_rssi (field 10)
    //     int32 warnings_active (field 11)
    //     bool  inverter_active (field 6)

    if (!buf || len == 0) return false;

    // Manual protobuf decode (lightweight, no generated code needed for simple fields)
    size_t pos = 0;
    int    pvIdx = 0;
    bool   gotGrid = false;

    memset(&d, 0, sizeof(DtuData_t));

    while (pos < len) {
        uint8_t tag_wire = buf[pos++];
        uint8_t field = tag_wire >> 3;
        uint8_t wire  = tag_wire & 0x07;

        if (wire == 0) {
            // Varint
            uint64_t val = 0; int shift = 0;
            while (pos < len) {
                uint8_t b = buf[pos++];
                val |= ((uint64_t)(b & 0x7F)) << shift;
                shift += 7;
                if (!(b & 0x80)) break;
            }
            switch (field) {
                case 6:  d.inverterActive  = (val != 0); break;
                case 8:  d.powerLimit      = (int)val;   break;
                case 10: d.wifiRssi        = (int)val;   break;
                case 11: d.warningsActive  = (int)val;   break;
            }
        } else if (wire == 2) {
            // Length-delimited
            uint32_t msgLen = 0; int shift = 0;
            while (pos < len) {
                uint8_t b = buf[pos++];
                msgLen |= ((uint32_t)(b & 0x7F)) << shift;
                shift += 7;
                if (!(b & 0x80)) break;
            }
            if (pos + msgLen > len) break;

            if (field == 2 || field == 3) {
                // Grid (2) or PV sub-message (3)
                // Sub-fields: 1=voltage*10, 2=current*100, 3=power*10,
                //             4=energy_daily*1000, 5=energy_total*1000
                float v = 0, i_ = 0, p = 0, dE = 0, tE = 0;
                size_t end = pos + msgLen;
                size_t sp  = pos;
                while (sp < end) {
                    uint8_t stw = buf[sp++];
                    uint8_t sf = stw >> 3;
                    uint8_t sw = stw & 0x07;
                    if (sw == 0) {
                        uint64_t sv = 0; int ss = 0;
                        while (sp < end) {
                            uint8_t b = buf[sp++];
                            sv |= ((uint64_t)(b & 0x7F)) << ss;
                            ss += 7;
                            if (!(b & 0x80)) break;
                        }
                        switch (sf) {
                            case 1: v  = sv / 10.0f;    break;
                            case 2: i_ = sv / 100.0f;   break;
                            case 3: p  = sv / 10.0f;    break;
                            case 4: dE = sv / 1000.0f;  break;
                            case 5: tE = sv / 1000.0f;  break;
                        }
                    } else sp++;  // skip unknown wire types
                }
                if (field == 2 && !gotGrid) {
                    d.grid_v = v; d.grid_i = i_; d.grid_p = p;
                    d.grid_dE = dE; d.grid_tE = tE;
                    gotGrid = true;
                } else if (field == 3 && pvIdx < 2) {
                    if (pvIdx == 0) { d.pv0_v=v; d.pv0_i=i_; d.pv0_p=p; d.pv0_dE=dE; d.pv0_tE=tE; }
                    else            { d.pv1_v=v; d.pv1_i=i_; d.pv1_p=p; d.pv1_dE=dE; d.pv1_tE=tE; }
                    pvIdx++;
                }
                pos += msgLen;
            } else if (wire == 5) {
                // 32-bit fixed
                if (field == 7 && pos + 4 <= len) {
                    float f; memcpy(&f, &buf[pos], 4);
                    d.temp = f;
                }
                pos += 4;
            } else {
                pos += msgLen;
            }
        } else if (wire == 5) {
            if (field == 7 && pos + 4 <= len) {
                float f; memcpy(&f, &buf[pos], 4);
                d.temp = f;
            }
            pos += 4;
        } else {
            break;  // unknown wire type, stop
        }
    }

    d.timestamp    = (uint32_t)(millis() / 1000);
    d.lastResponse = d.timestamp;
    d.dtuConnState = 1;

    LOG_I(MOD_DATA, "PV1: %.1fV / %.2fA / %.0fW   PV2: %.1fV / %.2fA / %.0fW",
          d.pv0_v, d.pv0_i, d.pv0_p,
          d.pv1_v, d.pv1_i, d.pv1_p);
    LOG_I(MOD_DATA, "Grid: %.1fV / %.2fA / %.0fW  Temp: %.1f°C  Limit: %d%%",
          d.grid_v, d.grid_i, d.grid_p, d.temp, d.powerLimit);
    LOG_I(MOD_DATA, "Energy today: %.3f kWh  Total: %.3f kWh",
          d.grid_dE, d.grid_tE);

    return (d.grid_p >= 0 || pvIdx > 0);
}

// ─── Build minimal protobuf request ───────────────────────────────────────────
// RealDataNew request: just a fixed 10-byte header from dtuGateway protocol
static const uint8_t REALDATA_REQ[] = {
    0x0A, 0x03, 0x00, 0x00, 0x00   // minimal AppGetHistPowerReqDto
};

// ─── Task ─────────────────────────────────────────────────────────────────────
void taskDTU(void* pvParameters) {
    static uint8_t respBuf[2048];
    int failCount = 0;

    LOG_I(MOD_DTU, "DTU task started. Target: %s:%d  Interval: %ds",
          appConfig.dtuHost, appConfig.dtuPort, appConfig.dtuInterval);

    for (;;) {
        // Wait for WiFi
        EventBits_t bits = xEventGroupWaitBits(systemStateEvents,
                                               EVT_WIFI_CONNECTED,
                                               pdFALSE, pdTRUE,
                                               pdMS_TO_TICKS(5000));
        if (!(bits & EVT_WIFI_CONNECTED)) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        // ── Send pending control commands first ────────────────────────────
        if (_reqDtuReboot) {
            _reqDtuReboot = false;
            LOG_I(MOD_DTU, "Requesting DTU reboot...");
            // POST to /api/dtu/reboot (dtuGateway endpoint)
            httpPost("/api/dtu/reboot", nullptr, 0, nullptr, nullptr);
        }
        if (_reqMiReboot) {
            _reqMiReboot = false;
            LOG_I(MOD_DTU, "Requesting inverter reboot...");
            httpPost("/api/inverter/reboot", nullptr, 0, nullptr, nullptr);
        }
        if (_reqPowerLimit) {
            _reqPowerLimit = false;
            int pct = _pendingLimit;
            if (pct < 2)   pct = 2;
            if (pct > 100) pct = 100;
            LOG_I(MOD_DTU, "Setting power limit to %d%%", pct);
            // Build limit request (1-byte protobuf: field1 varint = pct)
            uint8_t limitReq[4];
            limitReq[0] = 0x08;  // field 1, wire 0
            limitReq[1] = (uint8_t)pct;
            size_t rLen = sizeof(respBuf);
            httpPost("/api/inverter/limit", limitReq, 2, respBuf, &rLen);
        }
        if (_reqInverterOn) {
            _reqInverterOn = false;
            LOG_I(MOD_DTU, "Inverter -> %s", _pendingInvOn ? "ON" : "OFF");
            uint8_t onReq[2] = {0x08, (uint8_t)(_pendingInvOn ? 1 : 0)};
            size_t rLen = sizeof(respBuf);
            httpPost("/api/inverter/power", onReq, 2, respBuf, &rLen);
        }

        // ── Cloud pause ────────────────────────────────────────────────────
        if (appConfig.dtuCloudPause > 0) {
            vTaskDelay(pdMS_TO_TICKS((uint32_t)appConfig.dtuCloudPause * 1000));
        }

        // ── Fetch real-time data ───────────────────────────────────────────
        size_t rLen = sizeof(respBuf);
        bool ok = httpPost("/api/realdata/new", REALDATA_REQ, sizeof(REALDATA_REQ),
                           respBuf, &rLen);

        if (ok && rLen > 0) {
            DtuData_t newData = {};
            strlcpy(newData.inverterModel,      latestDtuData.inverterModel,      sizeof(newData.inverterModel));
            strlcpy(newData.inverterSerial,     latestDtuData.inverterSerial,     sizeof(newData.inverterSerial));
            strlcpy(newData.dtuVersionStr,      latestDtuData.dtuVersionStr,      sizeof(newData.dtuVersionStr));
            strlcpy(newData.inverterVersionStr, latestDtuData.inverterVersionStr, sizeof(newData.inverterVersionStr));

            if (parseRealData(respBuf, rLen, newData)) {
                // Update shared data
                if (xSemaphoreTake(configMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
                    memcpy(&latestDtuData, &newData, sizeof(DtuData_t));
                    dtuDataValid = true;
                    xSemaphoreGive(configMutex);
                }

                // Push to queues (non-blocking)
                xQueueOverwrite(dtuDataQueue, &newData);

                // Update event group and LED
                xEventGroupSetBits(systemStateEvents, EVT_DTU_ONLINE | EVT_DATA_RECEIVED);
                setLedState(LED_DATA_FLASH);

                failCount = 0;
            }
        } else {
            failCount++;
            LOG_W(MOD_DTU, "Fetch failed (%d/%d)", failCount, appConfig.dtuRebootAfterFails);
            xEventGroupClearBits(systemStateEvents, EVT_DTU_ONLINE);

            if (xEventGroupGetBits(systemStateEvents) & EVT_MQTT_CONNECTED)
                setLedState(LED_DTU_OFFLINE);

            if (failCount >= appConfig.dtuRebootAfterFails) {
                LOG_W(MOD_DTU, "Max fails reached, requesting DTU reboot");
                httpPost("/api/dtu/reboot", nullptr, 0, nullptr, nullptr);
                failCount = 0;
                vTaskDelay(pdMS_TO_TICKS(15000));  // wait for DTU to restart
            }
        }

        // ── Wait for next poll interval ────────────────────────────────────
        uint32_t waitMs = (uint32_t)appConfig.dtuInterval * 1000;
        vTaskDelay(pdMS_TO_TICKS(waitMs));
    }
}
