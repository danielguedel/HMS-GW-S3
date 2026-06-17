#pragma once
// Pre-generated NanoPB stub for Hoymiles HMS DTU protocol.
// Full generation: nanopb_generator hoymiles.proto
// For HMS-GW-S3 the manual protobuf decoder in taskDTU.cpp is used directly,
// so these structs are provided for reference / future use with nanopb_generator.

#include <pb.h>

#ifdef __cplusplus
extern "C" {
#endif

// -- GridData ------------------------------------------------------------------
typedef struct _GridData {
    int32_t voltage;        // field 1  -  V * 10
    int32_t current;        // field 2  -  A * 100
    int32_t power;          // field 3  -  W * 10
    int32_t energy_daily;   // field 4  -  kWh * 1000
    int32_t energy_total;   // field 5  -  kWh * 1000
    int32_t frequency;      // field 6  -  Hz * 100
} GridData;

// -- PVData --------------------------------------------------------------------
typedef struct _PVData {
    int32_t voltage;
    int32_t current;
    int32_t power;
    int32_t energy_daily;
    int32_t energy_total;
} PVData;

// -- RealDataNew ---------------------------------------------------------------
#define REALDATA_MAX_PV 2
typedef struct _RealDataNew {
    GridData grid[1];
    size_t   grid_count;
    PVData   pv[REALDATA_MAX_PV];
    size_t   pv_count;
    float    temperature;
    int32_t  power_limit_pct;
    bool     inverter_active;
    int32_t  wifi_rssi;
    int32_t  warnings_active;
} RealDataNew;

// -- DevInfoNew ----------------------------------------------------------------
typedef struct _DevInfoNew {
    uint32_t dtu_version;
    uint32_t inverter_version;
    char     inverter_serial[24];
    uint32_t dtu_serial_high;
    uint32_t dtu_serial_low;
} DevInfoNew;

// -- Helper: decode version uint to string "MM.mm.pp" -------------------------
static inline void versionToString(uint32_t v, char* out, size_t len) {
    snprintf(out, len, "%02lu.%02lu.%02lu",
             (unsigned long)((v >> 16) & 0xFF),
             (unsigned long)((v >>  8) & 0xFF),
             (unsigned long)( v        & 0xFF));
}

#ifdef __cplusplus
}
#endif
