#pragma once
#include <stdint.h>

#define LOG_FILE_PATH        "/log.txt"
#define LOG_FILE_PATH_PREV   "/log.txt.1"
#define LOG_FILE_MAX_BYTES   (16 * 1024)   // per-file cap; 32KB total budget across both files
#define LOG_FILE_MAX_LEVEL   2             // INFO cap, independent of appConfig.logLevel (Serial-only knob)

// Call once from main.cpp right after LittleFS.begin() succeeds.
void logFileInit();

// Appends one pre-formatted, newline-terminated line to /log.txt, rotating first (remove
// /log.txt.1, rename /log.txt -> /log.txt.1) if the append would exceed LOG_FILE_MAX_BYTES.
// No-ops silently if logFileInit() hasn't run yet or level > LOG_FILE_MAX_LEVEL. Caller
// (logMsg()) must already hold _logMutex -- this function does no locking of its own.
void logFileWrite(uint8_t level, const char* line);
