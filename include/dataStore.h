#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// --- DataStore ----------------------------------------------------------------
// Central in-memory store. All tasks communicate exclusively through this.
// Never access ds.* fields directly  -  use the API functions below.

struct DataStore {

    // -- PV-Daten (written by taskDTU) -----------------------------------------
    struct PvData {
        float   pv0_v, pv0_i, pv0_p;       // PV1: Spannung [V], Strom [A], Leistung [W]
        float   pv0_dE, pv0_tE;             // PV1: Tagesertrag, Gesamtertrag [kWh]
        float   pv1_v, pv1_i, pv1_p;       // PV2: Spannung [V], Strom [A], Leistung [W]
        float   pv1_dE, pv1_tE;             // PV2: Tagesertrag, Gesamtertrag [kWh]
        float   grid_v, grid_i, grid_p;     // Grid: Spannung [V], Strom [A], Leistung [W]
        float   grid_dE, grid_tE;           // Grid: Tagesertrag, Gesamtertrag [kWh]
        float   temp;                        // Wechselrichter-Temperatur [ C]
        int32_t powerLimit;                  // Leistungsbegrenzung [%]
        int32_t powerLimitSet;              // Gesetzter Grenzwert [%]
        bool    inverterActive;             // Wechselrichter aktiv
        int32_t warningsActive;             // Aktive Warnungen
        int32_t wifiRssi;                   // RSSI des DTU-WLAN
        uint32_t timestamp;                  // Unix-Timestamp der letzten Messung
        uint32_t lastResponseMs;            // millis() des letzten Empfangs
        bool    valid;                       // Daten gültig (mindestens 1 Empfang)
    } pv;

    // -- System-Status (written by taskWiFi / taskDTU / taskMQTT) -------------
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
        bool    dtuCloudBusy;               // ERR_RST -14 empfangen
        // MQTT
        bool    mqttConnected;
        uint32_t mqttLastConnectMs;
        // System
        uint32_t uptimeS;
        uint32_t freeHeap;
        String  fwVersion;
        int     buildNumber;
        String  macAddress;
        uint32_t ntpTime;                   // Unix-Timestamp (NTP)
    } system;

    // -- GPIO-Zustand (written by taskGPIO) ------------------------------------
    struct GpioState {
        bool relay;                          // Relay-Zustand
        bool gpio[3];                        // IO1–IO3 Zustände
    } gpio;

    // -- GPIO-Befehle (written by taskMQTT / taskWeb / taskSerial) -------------
    struct GpioCommand {
        bool    pending;
        int     target;                      // 0=Relay, 1-3=IO1-IO3
        bool    state;
    } gpioCmd;

    // -- DTU-Steuerbefehle -----------------------------------------------------
    struct DtuCommand {
        bool    setPowerLimit;
        int     powerLimitValue;             // [%]
        bool    rebootDtu;
        bool    rebootInverter;
        bool    setInverterOn;
        bool    inverterOnValue;
    } dtuCmd;

    // -- Internet-OTA-Status (written by taskWebServer) ------------------------
    struct OtaInfo {
        bool     available;          // neuere Version gefunden
        bool     checking;           // Manifest-Check läuft gerade
        char     version[32];        // Version aus Manifest
        int      buildNumber;        // Build-Nummer aus Manifest
        char     url[256];           // Firmware-Download-URL
        char     fsUrl[256];         // Filesystem-Download-URL (leer = kein FS-Update)
        char     md5[33];            // erwarteter MD5-Hash der Firmware (leer = keine Prüfung)
        char     fsMd5[33];          // erwarteter MD5-Hash des Filesystem-Images (leer = keine Prüfung)
        char     notes[128];         // Release-Notes
        uint32_t lastCheckMs;        // millis() des letzten Checks (0 = noch nie)
    } otaInfo;

    // -- Mutex ------------------------------------------------------------------
    SemaphoreHandle_t mutex;
};

extern DataStore ds;

// --- DataStore API ------------------------------------------------------------

// Initialisierung (muss in main.cpp vor Task-Start aufgerufen werden)
void dsInit();

// Lesen (gibt Kopie zurück  -  kein langer Lock nötig)
DataStore::PvData       dsGetPv();
DataStore::SystemStatus dsGetSystem();
DataStore::GpioState    dsGetGpio();
DataStore::GpioCommand  dsGetGpioCommand();  // atomic read + clears pending
DataStore::DtuCommand   dsGetDtuCommand();
DataStore::OtaInfo      dsGetOtaInfo();

// Schreiben (atomisch mit Mutex)
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
