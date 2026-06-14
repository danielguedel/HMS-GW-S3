#include "dataStore.h"
#include "config.h"

DataStore ds;

void dsInit() {
    ds.mutex = xSemaphoreCreateMutex();
    configASSERT(ds.mutex);

    // PvData — Nullwerte / ungültig
    memset(&ds.pv, 0, sizeof(ds.pv));
    ds.pv.valid = false;

    // SystemStatus — Nullwerte / offline
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

    // GpioState — alles aus
    ds.gpio.relay = false;
    memset(ds.gpio.gpio, 0, sizeof(ds.gpio.gpio));

    // GpioCommand — kein Befehl ausstehend
    ds.gpioCmd.pending = false;
    ds.gpioCmd.target  = 0;
    ds.gpioCmd.state   = false;

    // DtuCommand — kein Befehl ausstehend
    memset(&ds.dtuCmd, 0, sizeof(ds.dtuCmd));
}

// ─── Lesen ────────────────────────────────────────────────────────────────────

DataStore::PvData dsGetPv() {
    xSemaphoreTake(ds.mutex, portMAX_DELAY);
    DataStore::PvData copy = ds.pv;
    xSemaphoreGive(ds.mutex);
    return copy;
}

DataStore::SystemStatus dsGetSystem() {
    xSemaphoreTake(ds.mutex, portMAX_DELAY);
    DataStore::SystemStatus copy = ds.system;
    xSemaphoreGive(ds.mutex);
    return copy;
}

DataStore::GpioState dsGetGpio() {
    xSemaphoreTake(ds.mutex, portMAX_DELAY);
    DataStore::GpioState copy = ds.gpio;
    xSemaphoreGive(ds.mutex);
    return copy;
}

// ─── Schreiben ────────────────────────────────────────────────────────────────

void dsSetPv(const DataStore::PvData& data) {
    xSemaphoreTake(ds.mutex, portMAX_DELAY);
    ds.pv = data;
    xSemaphoreGive(ds.mutex);
}

void dsSetSystem(const DataStore::SystemStatus& status) {
    xSemaphoreTake(ds.mutex, portMAX_DELAY);
    ds.system = status;
    xSemaphoreGive(ds.mutex);
}

void dsSetGpio(const DataStore::GpioState& state) {
    xSemaphoreTake(ds.mutex, portMAX_DELAY);
    ds.gpio = state;
    xSemaphoreGive(ds.mutex);
}

void dsSetGpioCommand(int target, bool state) {
    xSemaphoreTake(ds.mutex, portMAX_DELAY);
    ds.gpioCmd.pending = true;
    ds.gpioCmd.target  = target;
    ds.gpioCmd.state   = state;
    xSemaphoreGive(ds.mutex);
}

void dsSetDtuCommand(const DataStore::DtuCommand& cmd) {
    xSemaphoreTake(ds.mutex, portMAX_DELAY);
    ds.dtuCmd = cmd;
    xSemaphoreGive(ds.mutex);
}

void dsClearDtuCommand() {
    xSemaphoreTake(ds.mutex, portMAX_DELAY);
    memset(&ds.dtuCmd, 0, sizeof(ds.dtuCmd));
    xSemaphoreGive(ds.mutex);
}

// ─── Convenience ──────────────────────────────────────────────────────────────

bool dsIsDtuOnline() {
    xSemaphoreTake(ds.mutex, portMAX_DELAY);
    bool v = ds.system.dtuOnline;
    xSemaphoreGive(ds.mutex);
    return v;
}

bool dsIsWifiConnected() {
    xSemaphoreTake(ds.mutex, portMAX_DELAY);
    bool v = ds.system.wifiConnected;
    xSemaphoreGive(ds.mutex);
    return v;
}

bool dsPvValid() {
    xSemaphoreTake(ds.mutex, portMAX_DELAY);
    bool v = ds.pv.valid;
    xSemaphoreGive(ds.mutex);
    return v;
}
