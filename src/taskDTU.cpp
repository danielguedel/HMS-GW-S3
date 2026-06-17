// taskDTU.cpp  -  Hoymiles HMS-800W-2T DTU communication (v2, DataStore pattern)
// Protocol: AsyncTCP raw TCP, port 10081
// 10-byte header + Protocol Buffers (manual decode  -  no generated pb files needed)
// Based on ohAnd/dtuGateway (Apache 2.0)

#include "taskDTU.h"
#include "dataStore.h"
#include "appConfig.h"
#include "systemState.h"
#include "taskLED.h"
#include "config.h"
#include "logger.h"
#include <Arduino.h>
#include <AsyncTCP.h>
#include <time.h>

// --- Power limit timeout state ------------------------------------------------
static uint32_t _powerLimitSetAt = 0;  // millis() when a non-default limit was set, 0 = inactive

// --- CRC16 MODBUS -------------------------------------------------------------
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
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        uint8_t b = reflectByte(data[i]);
        crc ^= (uint16_t)b << 8;
        for (int j = 0; j < 8; j++)
            crc = (crc & 0x8000) ? (crc << 1) ^ 0x8005 : crc << 1;
    }
    return reflectWord(crc);
}

// --- Build 10-byte header + payload ------------------------------------------
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

// --- Manual Protobuf encoder --------------------------------------------------
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

// --- Manual Protobuf decoder --------------------------------------------------
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
        field = (uint8_t)(v >> 3); wire = v & 0x07; return true;
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
        start = pos; msgLen = (size_t)l; pos += l; return pos <= len;
    }
};

// --- Parse SGSMO (grid data) --------------------------------------------------
struct SGSMO { int32_t voltage=0,frequency=0,active_power=0,current=0,temperature=0,warning_number=0,power_limit=0,wifi_rssi=0; };
static SGSMO parseSGSMO(const uint8_t* buf, size_t len) {
    SGSMO d; PbReader r{buf,len,0};
    uint8_t f,w; uint64_t v;
    while (r.readTag(f,w)) {
        if (w==0 && r.readVarint(v)) {
            switch(f) {
                case 3:  d.voltage       = (int32_t)v; break;
                case 4:  d.frequency     = (int32_t)v; break;
                case 5:  d.active_power  = (int32_t)v; break;
                case 7:  d.current       = (int32_t)v; break;
                case 9:  d.temperature   = (int32_t)v; break;
                case 10: d.warning_number= (int32_t)v; break;
                case 13: d.power_limit   = (int32_t)v; break;
            }
        } else r.skipField(w);
    }
    return d;
}

// --- Parse PvMO (PV string data) ---------------------------------------------
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

// --- Parse RealDataNew response -----------------------------------------------
static bool parseRealDataNew(const uint8_t* data, size_t len, DataStore::PvData& pv) {
    if (len < 11) return false;
    PbReader r{data+10, len-10, 0};
    uint8_t f, w; uint64_t v;
    int32_t timestamp = 0;
    SGSMO grid; PvMO pvStr[2]; int pvIdx = 0;

    while (r.pos < r.len && r.readTag(f,w)) {
        if      (f==2 && w==0)              { r.readVarint(v); timestamp=(int32_t)v; }
        else if (f==9 && w==2)              { size_t s,l; if (r.readSubMsg(s,l)) grid = parseSGSMO(r.buf+s,l); }
        else if (f==11 && w==2 && pvIdx<2) { size_t s,l; if (r.readSubMsg(s,l)) pvStr[pvIdx++] = parsePvMO(r.buf+s,l); }
        else                                { r.skipField(w); }
    }

    if (timestamp == 0) { LOG_W(MOD_DTU, "RealDataNew: timestamp=0, discarding"); return false; }

    auto cv = [](int32_t v, int32_t div=10) { return (float)v / div; };

    pv.timestamp      = (uint32_t)timestamp;
    pv.lastResponseMs = millis();
    pv.grid_v         = cv(grid.voltage);
    pv.grid_i         = cv(grid.current, 100);
    pv.grid_p         = cv(grid.active_power);
    pv.temp           = cv(grid.temperature);
    pv.warningsActive = grid.warning_number;
    pv.inverterActive = (pv.grid_p > 0.0f);
    pv.pv0_v   = cv(pvStr[0].voltage);
    pv.pv0_i   = cv(pvStr[0].current, 100);
    pv.pv0_p   = cv(pvStr[0].power);
    pv.pv0_dE  = cv(pvStr[0].energy_daily,  1000);
    pv.pv0_tE  = cv(pvStr[0].energy_total,  1000);
    pv.pv1_v   = cv(pvStr[1].voltage);
    pv.pv1_i   = cv(pvStr[1].current, 100);
    pv.pv1_p   = cv(pvStr[1].power);
    pv.pv1_dE  = cv(pvStr[1].energy_daily,  1000);
    pv.pv1_tE  = cv(pvStr[1].energy_total,  1000);
    pv.grid_dE = pv.pv0_dE + pv.pv1_dE;
    pv.grid_tE = pv.pv0_tE + pv.pv1_tE;
    pv.valid   = true;

    LOG_I(MOD_DATA, "PV1: %.1f V / %.2f A / %.0f W  PV2: %.1f V / %.2f A / %.0f W",
          pv.pv0_v, pv.pv0_i, pv.pv0_p, pv.pv1_v, pv.pv1_i, pv.pv1_p);
    LOG_I(MOD_DATA, "Grid: %.1f V / %.2f A / %.0f W  Temp: %.1f Grad Celsius",
          pv.grid_v, pv.grid_i, pv.grid_p, pv.temp);
    LOG_I(MOD_DATA, "Energy today: %.3f kWh  Total: %.3f kWh", pv.grid_dE, pv.grid_tE);
    return true;
}

// --- Parse GetConfig response (field 3 = powerLimit, field 11 = DTU WiFi RSSI)
static void parseGetConfig(const uint8_t* data, size_t len, DataStore::PvData& pv) {
    if (len < 11) return;
    PbReader r{data+10, len-10, 0};
    uint8_t f,w; uint64_t v;
    while (r.readTag(f,w)) {
        if (w==0 && r.readVarint(v)) {
            if (f==3)  pv.powerLimit = (int32_t)v;
            if (f==11) pv.wifiRssi  = (int32_t)v;
        } else r.skipField(w);
    }
    LOG_D(MOD_DTU, "GetConfig: powerLimit=%d%%  DTU RSSI=%d", pv.powerLimit, pv.wifiRssi);
}

// --- Packet builders ----------------------------------------------------------
static size_t buildAppInfoReq(uint8_t* buf, size_t size, uint32_t ntpTime) {
    size_t n = 0;
    n += encodeField(buf+n, 2, 0, 28800);   // offset
    n += encodeField(buf+n, 3, 0, 0);        // package_now = 0
    n += encodeField(buf+n, 4, 0, 0);        // err_code = 0
    n += encodeField(buf+n, 5, 0, ntpTime);  // time
    return n;
}

static size_t buildRealDataNewReq(uint8_t* buf, size_t size, uint32_t ntpTime) {
    size_t n = 0;
    n += encodeField(buf+n, 4, 0, 28800);   // offset
    n += encodeField(buf+n, 5, 0, ntpTime); // time
    return n;
}

static size_t buildGetConfigReq(uint8_t* buf, size_t size, uint32_t ntpTime) {
    size_t n = 0;
    n += encodeField(buf+n, 4, 0, 28800);
    n += encodeField(buf+n, 5, 0, ntpTime);
    return n;
}

// SetPowerLimit uses command 0xa3 0x0e with field 3 = limit [%] (matches GetConfig field mapping)
static size_t buildSetPowerLimitReq(uint8_t* buf, size_t size, uint32_t ntpTime, int limitPct) {
    size_t n = 0;
    n += encodeField(buf+n, 3, 0, (uint64_t)limitPct);
    n += encodeField(buf+n, 4, 0, 28800);
    n += encodeField(buf+n, 5, 0, ntpTime);
    return n;
}

// --- AsyncTCP state -----------------------------------------------------------
static AsyncClient*    _client    = nullptr;
static volatile bool   _connected = false;
static volatile bool   _dataReady = false;  // RealDataNew response received
static volatile bool   _cfgReady  = false;  // GetConfig response received
static volatile bool   _appReady  = false;  // AppInfo response received
static uint8_t            _rxBuf[2048];
static volatile size_t    _rxLen     = 0;
static volatile int       _lastError = 0;
static SemaphoreHandle_t  _rxMutex      = nullptr;
static volatile uint32_t  _ntpTimeCache = 0;  // updated by task loop, read by onConnect without lock
static volatile uint32_t _cloudPauseAt = 0;  // millis() of last ERR_RST -14

static void onData(void*, AsyncClient*, void* data, size_t len) {
    size_t copy = len < sizeof(_rxBuf) ? len : sizeof(_rxBuf);
    xSemaphoreTake(_rxMutex, portMAX_DELAY);
    memcpy(_rxBuf, data, copy);
    _rxLen = copy;
    xSemaphoreGive(_rxMutex);
    const uint8_t* d = (const uint8_t*)data;
    if (len >= 4) LOG_I(MOD_DTU, "RX %zu bytes: cmd=%02X%02X  %02X %02X ...", len, d[2],d[3],d[0],d[1]);
    // Signal based on what's pending
    if (!_appReady)       { _appReady  = true; return; }
    if (!_dataReady)      { _dataReady = true; return; }
    _cfgReady = true;
}

static void onConnect(void*, AsyncClient* c) {
    _connected = true;
    _appReady  = false;
    _dataReady = false;
    _cfgReady  = false;
    // Send AppInformation immediately  -  DTU has very short first-byte timeout
    uint32_t ntpTime = _ntpTimeCache;  // cached by task loop before connect  -  no mutex needed here
    uint8_t pb[64], msg[80];
    size_t pbLen  = buildAppInfoReq(pb, sizeof(pb), ntpTime);
    size_t msgLen = buildMsg(msg, sizeof(msg), 0xa3, 0x01, pb, pbLen);
    if (msgLen > 0) c->write((const char*)msg, msgLen);
    LOG_I(MOD_DTU, "TCP connected  -  sent AppInfo (%zu bytes)", msgLen);
}

static void onDisconnect(void*, AsyncClient*) {
    _connected = false;
    LOG_W(MOD_DTU, "TCP disconnected");
}

static void onError(void*, AsyncClient*, err_t e) {
    _connected = false;
    _lastError = (int)e;
    if (e == -14) {  // ERR_RST  -  DTU busy with Hoymiles cloud sync
        _cloudPauseAt = millis();
        LOG_W(MOD_DTU, "TCP RST (-14)  -  DTU cloud sync, pause %ds", appConfig.dtuCloudPause);
    } else {
        LOG_E(MOD_DTU, "TCP error: %d", (int)e);
    }
}

// --- Connect ------------------------------------------------------------------
static void dtuConnect() {
    if (_client) { delete _client; _client = nullptr; }
    _client = new AsyncClient();
    _client->onData(onData, nullptr);
    _client->onConnect(onConnect, nullptr);
    _client->onDisconnect(onDisconnect, nullptr);
    _client->onError(onError, nullptr);
    _client->setRxTimeout(60);
    LOG_I(MOD_DTU, "Connecting to %s:%d", appConfig.dtuHost, appConfig.dtuPort);
    _client->connect(appConfig.dtuHost, appConfig.dtuPort);
}

// --- Send helpers -------------------------------------------------------------
static bool sendRealDataNew(uint32_t ntpTime) {
    uint8_t pb[32], msg[64];
    size_t pbLen  = buildRealDataNewReq(pb, sizeof(pb), ntpTime);
    size_t msgLen = buildMsg(msg, sizeof(msg), 0xa3, 0x11, pb, pbLen);
    if (!msgLen) return false;
    _dataReady = false;
    _client->write((const char*)msg, msgLen);
    LOG_I(MOD_DTU, "Sent RealDataNew (%zu bytes)", msgLen);
    return true;
}

static bool sendGetConfig(uint32_t ntpTime) {
    uint8_t pb[32], msg[64];
    size_t pbLen  = buildGetConfigReq(pb, sizeof(pb), ntpTime);
    size_t msgLen = buildMsg(msg, sizeof(msg), 0xa3, 0x09, pb, pbLen);
    if (!msgLen) return false;
    _cfgReady = false;
    _client->write((const char*)msg, msgLen);
    LOG_D(MOD_DTU, "Sent GetConfig (%zu bytes)", msgLen);
    return true;
}

static bool sendSetPowerLimit(uint32_t ntpTime, int limitPct) {
    uint8_t pb[32], msg[64];
    size_t pbLen  = buildSetPowerLimitReq(pb, sizeof(pb), ntpTime, limitPct);
    size_t msgLen = buildMsg(msg, sizeof(msg), 0xa3, 0x0e, pb, pbLen);
    if (!msgLen) return false;
    _client->write((const char*)msg, msgLen);
    LOG_I(MOD_DTU, "Sent SetPowerLimit: %d%%", limitPct);
    return true;
}

// --- Wait helper with timeout -------------------------------------------------
static bool waitFor(volatile bool& flag, uint32_t timeoutMs) {
    uint32_t t0 = millis();
    while (!flag && (millis() - t0) < timeoutMs)
        vTaskDelay(pdMS_TO_TICKS(50));
    return flag;
}

// --- DataStore system update helpers ------------------------------------------
static void setDtuOnline(bool online, int failCount) {
    DataStore::SystemStatus sys = dsGetSystem();
    sys.dtuOnline       = online;
    sys.dtuFailCount    = failCount;
    if (online) sys.dtuLastConnectMs = millis();
    dsSetSystem(sys);
}

static void setDtuCloudBusy(bool busy) {
    DataStore::SystemStatus sys = dsGetSystem();
    sys.dtuCloudBusy = busy;
    dsSetSystem(sys);
}

// --- Task ---------------------------------------------------------------------
void taskDTU(void* pvParameters) {
    _rxMutex = xSemaphoreCreateMutex();
    LOG_I(MOD_DTU, "Task started  -  target: %s:%d  interval: %ds",
          appConfig.dtuHost, appConfig.dtuPort, appConfig.dtuInterval);

    // Wait for STA WiFi only (AP mode → task idle)
    xEventGroupWaitBits(systemStateEvents, EVT_WIFI_CONNECTED | EVT_WIFI_AP_MODE,
                        pdFALSE, pdFALSE, portMAX_DELAY);
    if (xEventGroupGetBits(systemStateEvents) & EVT_WIFI_AP_MODE) {
        LOG_W(MOD_DTU, "AP mode  -  task idle");
        vTaskDelete(nullptr); return;
    }

    uint32_t lastPollMs = 0;
    int      failCount  = 0;
    static uint8_t  localBuf[2048];
    size_t   localLen;

    for (;;) {
        // -- Power limit timeout check (Spec §4a) ------------------------------
        if (appConfig.powerLimitTimeout > 0 && _powerLimitSetAt > 0) {
            if ((millis() - _powerLimitSetAt) > (uint32_t)appConfig.powerLimitTimeout * 1000) {
                DataStore::PvData pv = dsGetPv();
                if (pv.powerLimit != appConfig.powerLimitDefault) {
                    LOG_W(MOD_DTU, "Power limit timeout  -  reset to %d%%", appConfig.powerLimitDefault);
                    if (_connected) {
                        uint32_t t = dsGetSystem().ntpTime;
                        sendSetPowerLimit(t, appConfig.powerLimitDefault);
                        pv.powerLimitSet = appConfig.powerLimitDefault;
                        dsSetPv(pv);
                    }
                }
                _powerLimitSetAt = 0;
            }
        }

        // -- Process pending DTU commands from DataStore ------------------------
        {
            DataStore::DtuCommand cmd = dsGetDtuCommand();
            if (cmd.setPowerLimit && _connected) {
                uint32_t t = dsGetSystem().ntpTime;
                sendSetPowerLimit(t, cmd.powerLimitValue);
                DataStore::PvData pv = dsGetPv();
                pv.powerLimitSet = cmd.powerLimitValue;
                dsSetPv(pv);
                _powerLimitSetAt = (cmd.powerLimitValue != appConfig.powerLimitDefault) ? millis() : 0;
                dsClearDtuCommand();
            } else if (cmd.rebootDtu && _connected) {
                LOG_W(MOD_DTU, "DTU reboot requested  -  closing connection");
                if (_client) _client->close();
                dsClearDtuCommand();
            } else if (cmd.rebootInverter || cmd.setInverterOn) {
                LOG_W(MOD_DTU, "Inverter command not yet implemented");
                dsClearDtuCommand();
            }
        }

        // -- Cloud-sync pause ---------------------------------------------------
        if (!_connected && _cloudPauseAt > 0 && appConfig.dtuCloudPause > 0) {
            uint32_t elapsed = millis() - _cloudPauseAt;
            uint32_t pauseMs = (uint32_t)appConfig.dtuCloudPause * 1000;
            if (elapsed < pauseMs) {
                setDtuCloudBusy(true);
                LOG_I(MOD_DTU, "Cloud-sync pause: %lums remaining",
                      (unsigned long)(pauseMs - elapsed));
                vTaskDelay(pdMS_TO_TICKS(pauseMs - elapsed));
            }
            _cloudPauseAt = 0;
            setDtuCloudBusy(false);
        }

        // -- Connect if disconnected --------------------------------------------
        if (!_connected) {
            _ntpTimeCache = dsGetSystem().ntpTime;  // cache before connect so onConnect can read without mutex
            dtuConnect();
            vTaskDelay(pdMS_TO_TICKS(DTU_CONNECT_TIMEOUT_MS));
            if (!_connected) {
                failCount++;
                LOG_W(MOD_DTU, "Connect failed (%d/%d)", failCount, appConfig.dtuRebootAfterFails);
                setDtuOnline(false, failCount);
                if (failCount >= appConfig.dtuRebootAfterFails) {
                    failCount = 0;
                    LOG_W(MOD_DTU, "Max failures reached  -  cooling down 30s");
                    vTaskDelay(pdMS_TO_TICKS(30000));
                }
                continue;
            }
            failCount = 0;

            // Wait for AppInfo response immediately after connect  -  DTU must ack
            // AppInfo before it will respond to RealDataNew. The connection stays
            // alive (setRxTimeout=60s), then we idle until the poll interval.
            if (!waitFor(_appReady, 8000)) {
                LOG_W(MOD_DTU, "AppInfo timeout  -  reconnecting");
                if (_client) _client->close();
                _connected = false;
                failCount++;
                setDtuOnline(false, failCount);
                continue;
            }
            LOG_I(MOD_DTU, "AppInfo ACK received");
        }

        // -- Poll interval (connection stays alive between polls) ---------------
        uint32_t now = millis();
        if ((now - lastPollMs) < (uint32_t)appConfig.dtuInterval * 1000) {
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        lastPollMs = now;

        if (!_connected) continue;

        uint32_t ntpTime = dsGetSystem().ntpTime;

        // -- RealDataNew -------------------------------------------------------
        sendRealDataNew(ntpTime);
        if (!waitFor(_dataReady, 5000)) {
            failCount++;
            LOG_W(MOD_DTU, "RealDataNew timeout (%d/%d)", failCount, appConfig.dtuRebootAfterFails);
            setDtuOnline(false, failCount);
            if (failCount >= appConfig.dtuRebootAfterFails) {
                if (_client) _client->close();
                _connected = false; failCount = 0;
            }
            continue;
        }
        xSemaphoreTake(_rxMutex, portMAX_DELAY);
        localLen = _rxLen;
        memcpy(localBuf, (const uint8_t*)_rxBuf, localLen);
        xSemaphoreGive(_rxMutex);

        DataStore::PvData newPv = dsGetPv();  // keep existing powerLimit / wifiRssi
        if (!parseRealDataNew(localBuf, localLen, newPv)) {
            LOG_W(MOD_DTU, "RealDataNew parse failed");
            continue;
        }

        // -- GetConfig ---------------------------------------------------------
        vTaskDelay(pdMS_TO_TICKS(300));
        if (_connected) {
            sendGetConfig(ntpTime);
            if (waitFor(_cfgReady, 3000)) {
                _cfgReady = false;
                xSemaphoreTake(_rxMutex, portMAX_DELAY);
                localLen = _rxLen;
                memcpy(localBuf, (const uint8_t*)_rxBuf, localLen);
                xSemaphoreGive(_rxMutex);
                parseGetConfig(localBuf, localLen, newPv);
            } else {
                LOG_D(MOD_DTU, "GetConfig timeout (non-fatal)");
            }
        }

        // -- Commit to DataStore -----------------------------------------------
        dsSetPv(newPv);
        setDtuOnline(true, 0);
        xEventGroupSetBits(systemStateEvents, EVT_DTU_ONLINE | EVT_DATA_RECEIVED);
        setLedState(LED_DATA_FLASH);
        failCount = 0;

        LOG_D(MOD_DTU, "Cycle complete  -  next in %ds", appConfig.dtuInterval);
    }
}
