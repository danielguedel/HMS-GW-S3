// taskSysMonitor.cpp  -  v2 (DataStore pattern)
// Single responsibility: keep uptimeS and freeHeap current in DataStore.
// LED state is auto-derived by taskLED  -  no state management here.

#include "taskSysMonitor.h"
#include "dataStore.h"
#include "config.h"
#include "logger.h"
#include <Arduino.h>

#define HEAP_WARN_THRESHOLD  20000   // warn if free heap < 20 KB
#define HEAP_LOG_INTERVAL_MS 10000   // detailed log every 10 s

void taskSysMonitor(void* pvParameters) {
    LOG_I(MOD_SYS, "SysMonitor started  -  heap warn threshold: %d B", HEAP_WARN_THRESHOLD);

    uint32_t lastHeapLogMs = 0;

    for (;;) {
        uint32_t now  = millis();
        uint32_t heap = ESP.getFreeHeap();

        // Update DataStore every tick (1 s)
        DataStore::SystemStatus sys = dsGetSystem();
        sys.uptimeS  = now / 1000;
        sys.freeHeap = heap;
        dsSetSystem(sys);

        // Periodic log + heap warning every 10 s
        if (now - lastHeapLogMs >= HEAP_LOG_INTERVAL_MS) {
            lastHeapLogMs = now;
            LOG_D(MOD_SYS, "Uptime: %lu s  Heap: %lu B  MinHeap: %lu B",
                  (unsigned long)(now / 1000),
                  (unsigned long)heap,
                  (unsigned long)ESP.getMinFreeHeap());
            if (heap < HEAP_WARN_THRESHOLD)
                LOG_W(MOD_SYS, "Low heap: %lu B!", (unsigned long)heap);
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
