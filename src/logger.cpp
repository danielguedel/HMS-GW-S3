#include "logger.h"
#include "appConfig.h"
#include "systemState.h"
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <stdarg.h>
#include <stdio.h>

static SemaphoreHandle_t _logMutex = nullptr;
static const char* levelStr[] = {"ERR", "WRN", "INF", "DBG"};

void logInit() {
    _logMutex = xSemaphoreCreateMutex();
}

void logMsg(uint8_t level, const char* module, const char* fmt, ...) {
    if (level > appConfig.logLevel) return;
    if (!_logMutex) {
        // Before mutex init — print directly
        Serial.printf("[%s] [%s] ", levelStr[level], module);
        va_list args;
        va_start(args, fmt);
        char buf[256];
        vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);
        Serial.println(buf);
        return;
    }
    if (xSemaphoreTake(_logMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        // Timestamp HH:MM:SS.mmm
        uint32_t ms = millis();
        uint32_t sec = ms / 1000;
        uint32_t hh  = sec / 3600;
        uint32_t mm  = (sec % 3600) / 60;
        uint32_t ss  = sec % 60;
        uint32_t mss = ms % 1000;

        va_list args;
        va_start(args, fmt);
        char buf[256];
        vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);

        Serial.printf("[%02lu:%02lu:%02lu.%03lu] [%s] [%s] %s\n",
                      (unsigned long)hh, (unsigned long)mm,
                      (unsigned long)ss, (unsigned long)mss,
                      levelStr[level], module, buf);
        xSemaphoreGive(_logMutex);
    }
}
