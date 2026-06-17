#include "logger.h"
#include "appConfig.h"
#include "systemState.h"
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <stdarg.h>
#include <stdio.h>

static SemaphoreHandle_t _logMutex = nullptr;

static const char* levelStr[]   = { "ERR", "WRN", "INF", "DBG" };
static const char* levelColor[] = {
    "\033[1;31m",   // ERR — bold red
    "\033[33m",     // WRN — yellow
    "\033[32m",     // INF — green
    "\033[36m",     // DBG — cyan
};
static const char* ANSI_RESET = "\033[0m";

void logInit() {
    _logMutex = xSemaphoreCreateMutex();
}

void logMsg(uint8_t level, const char* module, const char* fmt, ...) {
    if (level > appConfig.logLevel) return;

    va_list args;
    va_start(args, fmt);
    char buf[256];
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    if (!_logMutex) {
        Serial.printf("%s[%s] [%-*s]%s %s\n",
                      levelColor[level], levelStr[level],
                      LOG_MOD_WIDTH, module, ANSI_RESET, buf);
        return;
    }
    if (xSemaphoreTake(_logMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        uint32_t ms  = millis();
        uint32_t sec = ms / 1000;
        Serial.printf("[%02lu:%02lu:%02lu.%03lu] %s[%s] [%-*s]%s %s\n",
                      (unsigned long)(sec / 3600),
                      (unsigned long)((sec % 3600) / 60),
                      (unsigned long)(sec % 60),
                      (unsigned long)(ms % 1000),
                      levelColor[level], levelStr[level],
                      LOG_MOD_WIDTH, module, ANSI_RESET, buf);
        xSemaphoreGive(_logMutex);
    }
}
