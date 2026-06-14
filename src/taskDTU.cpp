// taskDTU.cpp — Hoymiles HMS-800W-2T DTU communication
// Protocol: AsyncTCP raw TCP, port 10081
// 10-byte header + Protocol Buffers (manual decode — no generated pb files needed)
// Based on ohAnd/dtuGateway (Apache 2.0)

#include "taskDTU.h"
#include "config.h"
#include "appConfig.h"
#include "systemState.h"
#include "logger.h"
#include "taskNeoPixel.h"
#include <Arduino.h>
#include <time.h>
#include <WiFi.h>
#include <AsyncTCP.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/event_groups.h>

// ─── Shared data ──────────────────────────────────────────────────────────────
DtuData_t  latestDtuData = {};
bool       dtuDataValid  = false;

// ─── Control flags ────────────────────────────────────────────────────────────
static volatile bool _reqDtuReboot  = false;
static volatile bool _reqMiReboot   = false;
static volatile bool _reqPowerLimit = false;
static volatile int  _pendingLimit  = 100;
static volatile bool _reqInverterOn = false;
static volatile bool _pendingInvOn  = true;

void dtuRequestReboot()         { _reqDtuReboot  = true; }
void dtuRequestInverterReboot() { _reqMiReboot   = true; }
void dtuSetPowerLimit(int pct)  { _pendingLimit  = pct; _reqPowerLimit = true; }
void dtuSetInverterOn(bool on)  { _pendingInvOn  = on;  _reqInverterOn = true; }

// ─── CRC16 MODBUS (matches dtuGateway CRC16_MODBUS settings) ─────────────────
// Initial=0xFFFF, Poly=0x8005, RefIn=true, RefOut=true, XorOut=0x0000
static uint8_t reflectByte(uint8_t b) {
    uint8_t r = 0;
    for (int i = 0; i < 8; i++) { r = (r << 1) | (b & 1); b >>= 1; }
    return r;
}
static uint16_t reflectWord(uint16_t w) {
    uint16_t r = 0;
    for (int i = 0; i < 16; i++) { r = (r << 1) | (w & 1); w >>= 1; }
    return r;
}
static uint16_t crc16(const uint8_t* data, size_t len) {
    uint16_t crc = 0xFFFF;  // MODBUS initial
    for (size_t i = 0; i < len; i++) {
        uint8_t b = reflectByte(data[i]);  // RefIn=true
        crc ^= (uint16_t)b << 8;
        for (int j = 0; j < 8; j++)
            crc = (crc & 0x8000) ? (crc << 1) ^ 0x8005 : crc << 1;
    }
    return reflectWord(crc);  // RefOut=true, XorOut=0x0000
}

// ─── Build 10-byte header + payload ──────────────────────────────────────────
// Header: 0x48 0x4d [cmd0] [cmd1] 0x00 0x01 [CRC-hi] [CRC-lo] [len-hi] [len-lo]
static size_t buildMsg(uint8_t* out, size_t outSize,
                        uint8_t cmd0, uint8_t cmd1,
                        const uint8_t* payload, size_t payLen) {
    size_t total = 10 + payLen;
    if (total > outSize) return 0;
    uint16_t crc = crc16(payload, payLen);
    out[0]=0x48; out[1]=0x4d;
    out[2]=cmd0;  out[3]=cmd1;
    out[4]=0x00;  out[5]=0x01;
    out[6]=(crc>>8)&0xFF; out[7]=crc&0xFF;
    out[8]=(total>>8)&0xFF; out[9]=total&0xFF;
    memcpy(out+10, payload, payLen);
    return total;
}

// ─── Manual Protobuf encoder (varint only — sufficient for our requests) ──────
static size_t encodeVarint(uint8_t* buf, uint64_t val) {
    size_t n = 0;
    do { buf[n++] = (val & 0x7F) | (val > 0x7F ? 0x80 : 0); val >>= 7; } while (val);
    return n;
}
static size_t encodeField(uint8_t* buf, uint8_t fieldNum, uint8_t wireType, uint64_t val) {
    size_t n = encodeVarint(buf, ((uint64_t)fieldNum << 3) | wireType);
    n += encodeVarint(buf + n, val);
    return n;
}

// ─── NTP timestamp (set from NTP or 0 if not available) ──────────────────────
static uint32_t _ntpTime = 0;  // will be set when NTP is available

// ─── Build RealDataNew request ────────────────────────────────────────────────
// RealDataNewResDTO { offset=4, time=5 }
static size_t buildRealDataNewReq(uint8_t* buf, size_t size) {
    size_t n = 0;
    n += encodeField(buf+n, 4, 0, 28800);   // offset = DTU_TIME_OFFSET
    n += encodeField(buf+n, 5, 0, _ntpTime); // time = 0 until NTP available
    return n;
}

// ─── Build GetConfig request ──────────────────────────────────────────────────
static size_t buildGetConfigReq(uint8_t* buf, size_t size) {
    size_t n = 0;
    n += encodeField(buf+n, 4, 0, 28800);
    n += encodeField(buf+n, 5, 0, _ntpTime);
    return n;
}

// ─── Manual Protobuf decoder ──────────────────────────────────────────────────
struct PbReader {
    const uint8_t* buf;
    size_t         len;
    size_t         pos;

    bool readVarint(uint64_t& val) {
        val = 0; int shift = 0;
        while (pos < len) {
            uint8_t b = buf[pos++];
            val |= ((uint64_t)(b & 0x7F)) << shift;
            shift += 7;
            if (!(b & 0x80)) return true;
        }
        return false;
    }
    bool readTag(uint8_t& field, uint8_t& wire) {
        uint64_t v; if (!readVarint(v)) return false;
        field = v >> 3; wire = v & 0x07; return true;
    }
    bool skipField(uint8_t wire) {
        if (wire == 0) { uint64_t v; return readVarint(v); }
        if (wire == 2) { uint64_t l; if (!readVarint(l)) return false; pos += l; return pos <= len; }
        if (wire == 5) { pos += 4; return pos <= len; }
        if (wire == 1) { pos += 8; return pos <= len; }
        return false;
    }
    bool readSubMsg(size_t& start, size_t& msgLen) {
        uint64_t l; if (!readVarint(l)) return false;
        start = pos; msgLen = l; pos += l; return pos <= len;
    }
};

// ─── Parse SGSMO (grid data) from sub-message ─────────────────────────────────
struct SGSMO { int32_t voltage=0,frequency=0,active_power=0,current=0,temperature=0,warning_number=0,power_limit=0; };
static SGSMO parseSGSMO(const uint8_t* buf, size_t len) {
    SGSMO d; PbReader r{buf,len,0};
    uint8_t f,w; uint64_t v;
    while (r.readTag(f,w)) {
        if (w==0 && r.readVarint(v)) {
            switch(f) {
                case 3: d.voltage      = (int32_t)v; break;
                case 4: d.frequency    = (int32_t)v; break;
                case 5: d.active_power = (int32_t)v; break;
                case 7: d.current      = (int32_t)v; break;
                case 9: d.temperature  = (int32_t)v; break;
                case 10:d.warning_number=(int32_t)v; break;
                case 13:d.power_limit  = (int32_t)v; break;
            }
        } else r.skipField(w);
    }
    return d;
}

// ─── Parse PvMO (PV string data) from sub-message ─────────────────────────────
struct PvMO { int32_t port_number=0,voltage=0,current=0,power=0,energy_total=0,energy_daily=0; };
static PvMO parsePvMO(const uint8_t* buf, size_t len) {
    PvMO d; PbReader r{buf,len,0};
    uint8_t f,w; uint64_t v;
    while (r.readTag(f,w)) {
        if (w==0 && r.readVarint(v)) {
            switch(f) {
                case 2: d.port_number  = (int32_t)v; break;
                case 3: d.voltage      = (int32_t)v; break;
                case 4: d.current      = (int32_t)v; break;
                case 5: d.power        = (int32_t)v; break;
                case 6: d.energy_total = (int32_t)v; break;
                case 7: d.energy_daily = (int32_t)v; break;
            }
        } else r.skipField(w);
    }
    return d;
}

// ─── Parse RealDataNewReqDTO ──────────────────────────────────────────────────
static bool parseRealDataNew(const uint8_t* data, size_t len, DtuData_t& d) {
    if (len < 11) return false;
    // Skip 10-byte header
    PbReader r{data+10, len-10, 0};
    uint8_t f, w; uint64_t v;
    int32_t timestamp = 0;
    SGSMO grid; PvMO pv[2]; int pvIdx=0;

    while (r.pos < r.len && r.readTag(f,w)) {
        if (f==2 && w==0) { r.readVarint(v); timestamp=(int32_t)v; }
        else if (f==9 && w==2) { // sgs_data (SGSMO)
            size_t start, msgLen;
            if (r.readSubMsg(start, msgLen)) grid = parseSGSMO(r.buf+start, msgLen);
        }
        else if (f==11 && w==2 && pvIdx<2) { // pv_data (PvMO)
            size_t start, msgLen;
            if (r.readSubMsg(start, msgLen)) pv[pvIdx++] = parsePvMO(r.buf+start, msgLen);
        }
        else r.skipField(w);
    }

    if (timestamp == 0) { LOG_W(MOD_DTU, "RealDataNew: timestamp=0, invalid"); return false; }

    auto cv = [](int32_t v, int32_t div=10) { return (float)v/div; };

    d.timestamp    = timestamp;
    d.lastResponse = (uint32_t)(millis()/1000);
    d.dtuConnState = 1;
    d.grid_v  = cv(grid.voltage);
    d.grid_i  = cv(grid.current, 100);
    d.grid_p  = cv(grid.active_power);
    d.temp    = cv(grid.temperature);
    d.warningsActive = grid.warning_number;
    d.pv0_v   = cv(pv[0].voltage);
    d.pv0_i   = cv(pv[0].current, 100);
    d.pv0_p   = cv(pv[0].power);
    d.pv0_dE  = cv(pv[0].energy_daily,  1000);
    d.pv0_tE  = cv(pv[0].energy_total,  1000);
    d.pv1_v   = cv(pv[1].voltage);
    d.pv1_i   = cv(pv[1].current, 100);
    d.pv1_p   = cv(pv[1].power);
    d.pv1_dE  = cv(pv[1].energy_daily,  1000);
    d.pv1_tE  = cv(pv[1].energy_total,  1000);
    d.grid_dE = d.pv0_dE + d.pv1_dE;
    d.grid_tE = d.pv0_tE + d.pv1_tE;

    LOG_I(MOD_DATA, "PV1: %.1fV/%.2fA/%.0fW  PV2: %.1fV/%.2fA/%.0fW",
          d.pv0_v,d.pv0_i,d.pv0_p, d.pv1_v,d.pv1_i,d.pv1_p);
    LOG_I(MOD_DATA, "Grid: %.1fV/%.2fA/%.0fW  Temp: %.1f°C",
          d.grid_v,d.grid_i,d.grid_p, d.temp);
    LOG_I(MOD_DATA, "Energy today: %.3fkWh  Total: %.3fkWh", d.grid_dE, d.grid_tE);
    return true;
}

// ─── Parse GetConfigReqDTO (power limit in field 3) ──────────────────────────
static void parseGetConfig(const uint8_t* data, size_t len, DtuData_t& d) {
    if (len < 11) return;
    PbReader r{data+10, len-10, 0};
    uint8_t f,w; uint64_t v;
    while (r.readTag(f,w)) {
        if (w==0 && r.readVarint(v)) {
            if (f==3) d.powerLimit = (int32_t)v;
        } else r.skipField(w);
    }
    LOG_D(MOD_DTU, "GetConfig: powerLimit=%d%%", d.powerLimit);
}

// ─── Forward declaration ──────────────────────────────────────────────────────
static size_t buildAppInfoReq(uint8_t* buf, size_t size);

// ─── AsyncTCP state ───────────────────────────────────────────────────────────
static AsyncClient* _client   = nullptr;
static volatile bool _connected  = false;
static volatile bool _dataReady  = false;
static volatile bool _cfgReady   = false;
static volatile int  _txrxState  = 0; // 0=idle, 1=waitData, 2=waitCfg
static uint8_t _rxBuf[2048];
static volatile size_t _rxLen = 0;

static void onData(void*, AsyncClient*, void* data, size_t len) {
    size_t copy = len < sizeof(_rxBuf) ? len : sizeof(_rxBuf);
    memcpy(_rxBuf, data, copy); _rxLen = copy;
    // Log first bytes of response for debugging
    uint8_t* d = (uint8_t*)data;
    if (len >= 4) {
        LOG_D(MOD_DTU, "RX %zu bytes: %02X %02X %02X %02X ...", len, d[0],d[1],d[2],d[3]);
    }
    // Accept response based on current state
    if (_txrxState == 3) { _dataReady = true; }  // AppInfo response
    if (_txrxState == 1 || _txrxState == 0) _dataReady = true;  // RealDataNew
    if (_txrxState == 2) _cfgReady  = true;       // GetConfig
}
static volatile bool _sendAppInfo = false;

static void onConnect(void*, AsyncClient* c) {
    _connected = true;
    // Send AppInfo immediately on connect — DTU has very short first-byte timeout
    // Build and send directly from callback (small packet, safe to do here)
    uint8_t pb[32], msg[48];
    size_t pbLen = buildAppInfoReq(pb, sizeof(pb));
    size_t msgLen = buildMsg(msg, sizeof(msg), 0xa3, 0x01, pb, pbLen);
    if (msgLen > 0) {
        _txrxState = 3;
        _dataReady = false;
        c->write((const char*)msg, msgLen);
    }
    _sendAppInfo = false;  // already sent
    LOG_I(MOD_DTU, "TCP connected — sent AppInfo immediately (%zu bytes)", msgLen);
}
static void onDisconnect(void*, AsyncClient*) {
    _connected = false; _txrxState = 0;
    xEventGroupClearBits(systemStateEvents, EVT_DTU_ONLINE);
    LOG_W(MOD_DTU, "TCP disconnected");
}
static volatile int      _lastError      = 0;
static volatile uint32_t _lastCloudPause = 0;  // millis() of last RST-14 (cloud sync)

static void onError(void*, AsyncClient*, err_t e) {
    _connected = false;
    _lastError = (int)e;
    if (e == -14) {  // ERR_RST — DTU busy with Hoymiles cloud sync
        _lastCloudPause = millis();
        LOG_W(MOD_DTU, "TCP RST (-14) — DTU busy with cloud sync, will retry");
    } else {
        LOG_E(MOD_DTU, "TCP error: %d", (int)e);
    }
}

// ─── Connect ──────────────────────────────────────────────────────────────────
static void dtuConnect() {
    if (_client) { delete _client; _client = nullptr; }
    _client = new AsyncClient();
    _client->onData(onData, nullptr);
    _client->onConnect(onConnect, nullptr);
    _client->onDisconnect(onDisconnect, nullptr);
    _client->onError(onError, nullptr);
    _client->setRxTimeout(10);
    LOG_I(MOD_DTU, "Connecting to %s:%d ...", appConfig.dtuHost, appConfig.dtuPort);
    _client->connect(appConfig.dtuHost, appConfig.dtuPort);
}

// ─── Send helpers ─────────────────────────────────────────────────────────────
// ─── Build AppInformation request (first request after connect) ───────────────
// APPInfoDataResDTO { offset=2, package_now=3, err_code=4, time=5 }
static size_t buildAppInfoReq(uint8_t* buf, size_t size) {
    size_t n = 0;
    n += encodeField(buf+n, 2, 0, 28800);    // offset
    n += encodeField(buf+n, 3, 0, 0);         // package_now = 0
    n += encodeField(buf+n, 4, 0, 0);         // err_code = 0
    n += encodeField(buf+n, 5, 0, _ntpTime);  // time
    return n;
}

static bool sendAppInformation() {
    uint8_t pb[64], msg[80];
    size_t pbLen = buildAppInfoReq(pb, sizeof(pb));
    size_t msgLen = buildMsg(msg, sizeof(msg), 0xa3, 0x01, pb, pbLen);
    if (!msgLen) return false;
    _txrxState = 3; _dataReady = false;  // state 3 = wait AppInfo response
    _client->write((const char*)msg, msgLen);
    LOG_I(MOD_DTU, "Sent AppInformation request (%zu bytes)", msgLen);
    return true;
}

static bool sendRealDataNew() {
    uint8_t pb[32], msg[64];
    size_t pbLen = buildRealDataNewReq(pb, sizeof(pb));
    size_t msgLen = buildMsg(msg, sizeof(msg), 0xa3, 0x11, pb, pbLen);
    if (!msgLen) return false;
    _txrxState = 1; _dataReady = false;
    _client->write((const char*)msg, msgLen);
    LOG_I(MOD_DTU, "Sent RealDataNew request (%zu bytes)", msgLen);
    return true;
}

static bool sendGetConfig() {
    uint8_t pb[32], msg[64];
    size_t pbLen = buildGetConfigReq(pb, sizeof(pb));
    size_t msgLen = buildMsg(msg, sizeof(msg), 0xa3, 0x09, pb, pbLen);
    if (!msgLen) return false;
    _txrxState = 2; _cfgReady = false;
    _client->write((const char*)msg, msgLen);
    return true;
}

// ─── Task ─────────────────────────────────────────────────────────────────────
void taskDTU(void* pvParameters) {
    LOG_I(MOD_DTU, "DTU task started. Target: %s:%d  Interval: %ds",
          appConfig.dtuHost, appConfig.dtuPort, appConfig.dtuInterval);

    // Wait for WiFi (not AP mode)
    xEventGroupWaitBits(systemStateEvents, EVT_WIFI_CONNECTED | EVT_WIFI_AP_MODE,
                        pdFALSE, pdFALSE, portMAX_DELAY);
    if (xEventGroupGetBits(systemStateEvents) & EVT_WIFI_AP_MODE) {
        LOG_W(MOD_DTU, "AP mode — DTU task idle");
        vTaskDelete(nullptr); return;
    }

    // Sync NTP time
    configTime(appConfig.tzOffset, 0, "pool.ntp.org", "time.nist.gov");
    LOG_I(MOD_DTU, "Waiting for NTP time...");
    struct tm timeinfo;
    uint32_t ntpWait = millis();
    while (!getLocalTime(&timeinfo) && (millis()-ntpWait) < 10000)
        vTaskDelay(pdMS_TO_TICKS(500));
    if (getLocalTime(&timeinfo)) {
        time_t now; time(&now);
        _ntpTime = (uint32_t)now;
        LOG_I(MOD_DTU, "NTP time: %lu", (unsigned long)_ntpTime);
    } else {
        LOG_W(MOD_DTU, "NTP sync failed — using time=0");
    }

    uint32_t lastPoll = 0;
    int      failCount = 0;
    DtuData_t newData = {};

    for (;;) {
        // ── Maintain connection ────────────────────────────────────────────
        if (!_connected) {
            // Cloud pause: if DTU sent RST-14 (cloud sync), wait out the remaining pause
            if (appConfig.dtuCloudPause > 0 && _lastCloudPause > 0) {
                uint32_t elapsed  = millis() - _lastCloudPause;
                uint32_t pauseMs  = (uint32_t)appConfig.dtuCloudPause * 1000;
                if (elapsed < pauseMs) {
                    LOG_I(MOD_DTU, "Cloud-sync pause: waiting %lums", (unsigned long)(pauseMs - elapsed));
                    vTaskDelay(pdMS_TO_TICKS(pauseMs - elapsed));
                }
            }
            dtuConnect();
            vTaskDelay(pdMS_TO_TICKS(3000));
            if (!_connected) {
                failCount++;
                LOG_W(MOD_DTU, "Connect failed (%d/%d)", failCount, appConfig.dtuRebootAfterFails);
                if (failCount >= appConfig.dtuRebootAfterFails) {
                    failCount = 0;
                    vTaskDelay(pdMS_TO_TICKS(30000));
                }
                continue;
            }
            failCount = 0;
        }

        // ── Poll interval ──────────────────────────────────────────────────
        uint32_t now = millis();
        if ((now - lastPoll) < (uint32_t)appConfig.dtuInterval * 1000) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        lastPoll = now;

        if (!_connected) continue;

        // ── Request RealDataNew ────────────────────────────────────────────
        memset(&newData, 0, sizeof(newData));
        LOG_I(MOD_DTU, "Sending RealDataNew request...");
        sendRealDataNew();

        uint32_t t0 = millis();
        while (!_dataReady && (millis()-t0) < 5000)
            vTaskDelay(pdMS_TO_TICKS(50));

        if (!_dataReady) {
            failCount++;
            LOG_W(MOD_DTU, "Timeout RealDataNew - no response in 5s (%d/%d)", failCount, appConfig.dtuRebootAfterFails);
            xEventGroupClearBits(systemStateEvents, EVT_DTU_ONLINE);
            if (failCount >= appConfig.dtuRebootAfterFails) {
                if (_client) _client->close(true);
                _connected = false; failCount = 0;
            }
            continue;
        }
        _dataReady = false;

        // Copy rx buffer (volatile — take snapshot)
        uint8_t localBuf[2048]; size_t localLen;
        localLen = _rxLen;
        memcpy(localBuf, (const uint8_t*)_rxBuf, localLen);

        if (!parseRealDataNew(localBuf, localLen, newData)) {
            LOG_W(MOD_DTU, "Parse RealDataNew failed");
            continue;
        }

        // ── Request GetConfig ──────────────────────────────────────────────
        vTaskDelay(pdMS_TO_TICKS(300));
        if (_connected) {
            sendGetConfig();
            t0 = millis();
            while (!_cfgReady && (millis()-t0) < 3000)
                vTaskDelay(pdMS_TO_TICKS(50));
            if (_cfgReady) {
                _cfgReady = false;
                localLen = _rxLen;
                memcpy(localBuf, (const uint8_t*)_rxBuf, localLen);
                parseGetConfig(localBuf, localLen, newData);
            }
        }

        // ── Update shared state ────────────────────────────────────────────
        if (xSemaphoreTake(configMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
            memcpy(&latestDtuData, &newData, sizeof(DtuData_t));
            dtuDataValid = true;
            xSemaphoreGive(configMutex);
        }
        xQueueOverwrite(dtuDataQueue, &newData);
        xEventGroupSetBits(systemStateEvents, EVT_DTU_ONLINE | EVT_DATA_RECEIVED);
        setLedState(LED_DATA_FLASH);
        failCount = 0;
    }
}
