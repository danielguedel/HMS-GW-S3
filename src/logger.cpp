#include "logger.h"
#include "appConfig.h"
#include "logFile.h"
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <stdarg.h>
#include <stdio.h>

static SemaphoreHandle_t _logMutex = nullptr;

static const char* levelStr[]   = { "ERR", "WRN", "INF", "DBG" };
static const char* levelColor[] = {
    "\033[1;31m",   // ERR  -  bold red
    "\033[33m",     // WRN  -  yellow
    "\033[32m",     // INF  -  green
    "\033[36m",     // DBG  -  cyan
};
static const char* ANSI_RESET = "\033[0m";

void logInit() {
    _logMutex = xSemaphoreCreateMutex();
}

// Called via the LOG_E/LOG_W/LOG_I/LOG_D macros from any task. Serial output is gated by
// appConfig.logLevel; the persisted log file is gated independently by LOG_FILE_MAX_LEVEL
// (fixed at INFO) so file volume stays bounded regardless of the user's Serial verbosity.
// Before logInit() runs (no mutex yet) it prints without a timestamp and without locking, and
// file logging is skipped (LittleFS isn't mounted yet at that point either); afterwards it
// silently drops the line instead of blocking if the mutex isn't free within 50ms.
void logMsg(uint8_t level, const char* module, const char* fmt, ...) {
    bool toSerial = (level <= appConfig.logLevel);
    bool toFile   = (level <= LOG_FILE_MAX_LEVEL);
    if (!toSerial && !toFile) return;

    va_list args;
    va_start(args, fmt);
    char msg[256];
    vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);

    if (!_logMutex) {
        if (toSerial) {
            Serial.printf("%s[%s] [%-*s]%s %s\n",
                          levelColor[level], levelStr[level],
                          LOG_MOD_WIDTH, module, ANSI_RESET, msg);
        }
        return;
    }
    if (xSemaphoreTake(_logMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        uint32_t ms  = millis();
        uint32_t sec = ms / 1000;
        if (toSerial) {
            Serial.printf("[%02lu:%02lu:%02lu.%03lu] %s[%s] [%-*s]%s %s\n",
                          (unsigned long)(sec / 3600),
                          (unsigned long)((sec % 3600) / 60),
                          (unsigned long)(sec % 60),
                          (unsigned long)(ms % 1000),
                          levelColor[level], levelStr[level],
                          LOG_MOD_WIDTH, module, ANSI_RESET, msg);
        }
        if (toFile) {
            char line[300];
            snprintf(line, sizeof(line), "[%02lu:%02lu:%02lu.%03lu] [%s] [%-*s] %s\n",
                     (unsigned long)(sec / 3600),
                     (unsigned long)((sec % 3600) / 60),
                     (unsigned long)(sec % 60),
                     (unsigned long)(ms % 1000),
                     levelStr[level], LOG_MOD_WIDTH, module, msg);
            logFileWrite(level, line);
        }
        xSemaphoreGive(_logMutex);
    }
}
