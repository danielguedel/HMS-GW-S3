#include "dataStore.h"
#include "config.h"

DataStore ds;
static SemaphoreHandle_t _mutex = nullptr;  // private: never exposed via DataStore, only ds*() API may touch it

void dsInit() {
    _mutex = xSemaphoreCreateMutex();
    configASSERT(_mutex);

    // PvData  -  zero values / invalid
    memset(&ds.pv, 0, sizeof(ds.pv));
    ds.pv.valid = false;

    // SystemStatus  -  zero values / offline
    ds.system.wifiConnected   = false;
    ds.system.wifiApMode      = false;
    ds.system.wifiIp          = "";
    ds.system.wifiRssi        = 0;
    ds.system.wifiSsid        = "";
    ds.system.dtuOnline       = false;
    ds.system.dtuLastConnectMs = 0;
    ds.system.dtuFailCount    = 0;
    ds.system.dtuCloudBusy    = false;
    ds.system.mqttConnected   = false;
    ds.system.mqttLastConnectMs = 0;
    ds.system.uptimeS         = 0;
    ds.system.freeHeap        = 0;
    ds.system.fwVersion       = FW_VERSION;
    ds.system.buildNumber     = BUILD_NUMBER;
    ds.system.macAddress      = "";
    ds.system.ntpTime         = 0;

    // GpioState  -  all off
    ds.gpio.relay = false;
    memset(ds.gpio.gpio, 0, sizeof(ds.gpio.gpio));

    // GpioCommand  -  no command pending
    ds.gpioCmd.pending = false;
    ds.gpioCmd.target  = 0;
    ds.gpioCmd.state   = false;

    // DtuCommand  -  no command pending
    memset(&ds.dtuCmd, 0, sizeof(ds.dtuCmd));

    // OtaInfo  -  no check performed yet
    memset(&ds.otaInfo, 0, sizeof(ds.otaInfo));
}

// --- Reading --------------------------------------------------------------------

DataStore::PvData dsGetPv() {
    xSemaphoreTake(_mutex, portMAX_DELAY);
    DataStore::PvData copy = ds.pv;
    xSemaphoreGive(_mutex);
    return copy;
}

DataStore::SystemStatus dsGetSystem() {
    xSemaphoreTake(_mutex, portMAX_DELAY);
    DataStore::SystemStatus copy = ds.system;
    xSemaphoreGive(_mutex);
    return copy;
}

DataStore::GpioState dsGetGpio() {
    xSemaphoreTake(_mutex, portMAX_DELAY);
    DataStore::GpioState copy = ds.gpio;
    xSemaphoreGive(_mutex);
    return copy;
}

// Unlike the other dsGet* functions, this consumes the command: a pending request is atomically cleared as part of the read, so each command is delivered to exactly one caller.
DataStore::GpioCommand dsGetGpioCommand() {
    xSemaphoreTake(_mutex, portMAX_DELAY);
    DataStore::GpioCommand copy = ds.gpioCmd;
    if (copy.pending) ds.gpioCmd.pending = false;
    xSemaphoreGive(_mutex);
    return copy;
}

DataStore::DtuCommand dsGetDtuCommand() {
    xSemaphoreTake(_mutex, portMAX_DELAY);
    DataStore::DtuCommand copy = ds.dtuCmd;
    xSemaphoreGive(_mutex);
    return copy;
}

// --- Writing ----------------------------------------------------------------

void dsSetPv(const DataStore::PvData& data) {
    xSemaphoreTake(_mutex, portMAX_DELAY);
    ds.pv = data;
    xSemaphoreGive(_mutex);
}

void dsSetSystem(const DataStore::SystemStatus& status) {
    xSemaphoreTake(_mutex, portMAX_DELAY);
    ds.system = status;
    xSemaphoreGive(_mutex);
}

void dsSetGpio(const DataStore::GpioState& state) {
    xSemaphoreTake(_mutex, portMAX_DELAY);
    ds.gpio = state;
    xSemaphoreGive(_mutex);
}

void dsSetGpioCommand(int target, bool state) {
    xSemaphoreTake(_mutex, portMAX_DELAY);
    ds.gpioCmd.pending = true;
    ds.gpioCmd.target  = target;
    ds.gpioCmd.state   = state;
    xSemaphoreGive(_mutex);
}

void dsSetDtuCommand(const DataStore::DtuCommand& cmd) {
    xSemaphoreTake(_mutex, portMAX_DELAY);
    ds.dtuCmd = cmd;
    xSemaphoreGive(_mutex);
}

void dsClearDtuCommand() {
    xSemaphoreTake(_mutex, portMAX_DELAY);
    memset(&ds.dtuCmd, 0, sizeof(ds.dtuCmd));
    xSemaphoreGive(_mutex);
}

DataStore::OtaInfo dsGetOtaInfo() {
    xSemaphoreTake(_mutex, portMAX_DELAY);
    DataStore::OtaInfo copy = ds.otaInfo;
    xSemaphoreGive(_mutex);
    return copy;
}

void dsSetOtaInfo(const DataStore::OtaInfo& info) {
    xSemaphoreTake(_mutex, portMAX_DELAY);
    ds.otaInfo = info;
    xSemaphoreGive(_mutex);
}

// --- Convenience --------------------------------------------------------------

bool dsIsDtuOnline() {
    xSemaphoreTake(_mutex, portMAX_DELAY);
    bool v = ds.system.dtuOnline;
    xSemaphoreGive(_mutex);
    return v;
}

bool dsIsWifiConnected() {
    xSemaphoreTake(_mutex, portMAX_DELAY);
    bool v = ds.system.wifiConnected;
    xSemaphoreGive(_mutex);
    return v;
}

bool dsPvValid() {
    xSemaphoreTake(_mutex, portMAX_DELAY);
    bool v = ds.pv.valid;
    xSemaphoreGive(_mutex);
    return v;
}
