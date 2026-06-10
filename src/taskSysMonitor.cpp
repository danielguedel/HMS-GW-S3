#include "taskSysMonitor.h"
#include "config.h"
#include "systemState.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// TODO: Implement taskSysMonitor
void taskSysMonitor(void* pvParameters) {
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
