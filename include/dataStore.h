#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// --- DataStore ----------------------------------------------------------------
// Central in-memory store. All tasks communicate exclusively through this.
// Never access ds.* fields directly  -  use the API functions below.

struct DataStore {

    // -- PV data (written by taskDTU) ------------------------------------------
    struct PvData {
        float   pv0_v, pv0_i, pv0_p;       // PV1: voltage [V], current [A], power [W]
        float   pv0_dE, pv0_tE;             // PV1: daily yield, total yield [kWh]
        float   pv1_v, pv1_i, pv1_p;       // PV2: voltage [V], current [A], power [W]
        float   pv1_dE, pv1_tE;             // PV2: daily yield, total yield [kWh]
        float   grid_v, grid_i, grid_p;     // Grid: voltage [V], current [A], power [W]
        float   grid_dE, grid_tE;           // Grid: daily yield, total yield [kWh]
        float   temp;                        // inverter temperature [C]
        int32_t powerLimit;                  // power limit [%]
        int32_t powerLimitSet;              // limit value that was set [%]
        bool    inverterActive;             // inverter active
        int32_t warningsActive;             // active warnings
        int32_t wifiRssi;                   // RSSI of the DTU's WiFi
        uint32_t timestamp;                  // Unix timestamp of the last measurement
        uint32_t lastResponseMs;            // millis() of the last reception
        bool    valid;                       // data valid (at least one reception)
    } pv;

    // -- System status (written by taskWiFi / taskDTU / taskMQTT) -------------
    struct SystemStatus {
        // WiFi
        bool    wifiConnected;
        bool    wifiApMode;
        String  wifiIp;
        int8_t  wifiRssi;
        String  wifiSsid;
        // DTU
        bool    dtuOnline;
        uint32_t dtuLastConnectMs;
        int     dtuFailCount;
        bool    dtuCloudBusy;               // received ERR_RST -14
        // MQTT
        bool    mqttConnected;
        uint32_t mqttLastConnectMs;
        // System
        uint32_t uptimeS;
        uint32_t freeHeap;
        String  fwVersion;
        int     buildNumber;
        String  macAddress;
        uint32_t ntpTime;                   // Unix timestamp (NTP)
    } system;

    // -- GPIO state (written by taskGPIO) --------------------------------------
    struct GpioState {
        bool relay;                          // relay state
        bool gpio[3];                        // IO1-IO3 states
    } gpio;

    // -- GPIO commands (written by taskMQTT / taskWeb / taskSerial) -----------
    struct GpioCommand {
        bool    pending;
        int     target;                      // 0=Relay, 1-3=IO1-IO3
        bool    state;
    } gpioCmd;

    // -- DTU control commands ---------------------------------------------------
    struct DtuCommand {
        bool    setPowerLimit;
        int     powerLimitValue;             // [%]
        bool    rebootDtu;
        bool    rebootInverter;
        bool    setInverterOn;
        bool    inverterOnValue;
    } dtuCmd;

    // -- Internet OTA status (written by taskWebServer) ------------------------
    struct OtaInfo {
        bool     available;          // newer version found
        bool     checking;           // manifest check currently running
        char     version[32];        // version from the manifest
        int      buildNumber;        // build number from the manifest
        char     url[256];           // firmware download URL
        char     fsUrl[256];         // filesystem download URL (empty = no FS update)
        char     md5[33];            // expected MD5 hash of the firmware (empty = no check)
        char     fsMd5[33];          // expected MD5 hash of the filesystem image (empty = no check)
        char     notes[128];         // release notes
        uint32_t lastCheckMs;        // millis() of the last check (0 = never)
    } otaInfo;
};

extern DataStore ds;

// --- DataStore API ------------------------------------------------------------

// Initialization (must be called in main.cpp before tasks start)
void dsInit();

// Reads (return a copy  -  no long lock needed)
DataStore::PvData       dsGetPv();
DataStore::SystemStatus dsGetSystem();
DataStore::GpioState    dsGetGpio();
DataStore::GpioCommand  dsGetGpioCommand();  // atomic read + clears pending
DataStore::DtuCommand   dsGetDtuCommand();
DataStore::OtaInfo      dsGetOtaInfo();

// Writes (atomic via mutex)
void dsSetPv(const DataStore::PvData& data);
void dsSetSystem(const DataStore::SystemStatus& status);
void dsSetGpio(const DataStore::GpioState& state);
void dsSetGpioCommand(int target, bool state);
void dsSetDtuCommand(const DataStore::DtuCommand& cmd);
void dsClearDtuCommand();
void dsSetOtaInfo(const DataStore::OtaInfo& info);

// Convenience
bool dsIsDtuOnline();
bool dsIsWifiConnected();
bool dsPvValid();
