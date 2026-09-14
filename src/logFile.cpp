#include "logFile.h"
#include <LittleFS.h>
#include <string.h>

static bool     _ready   = false;
static uint32_t _curSize = 0;   // bytes currently in /log.txt; avoids a stat() on every line

void logFileInit() {
    File f = LittleFS.open(LOG_FILE_PATH, "r");
    _curSize = f ? f.size() : 0;
    if (f) f.close();
    _ready = true;
}

static void rotate() {
    LittleFS.remove(LOG_FILE_PATH_PREV);
    LittleFS.rename(LOG_FILE_PATH, LOG_FILE_PATH_PREV);
    _curSize = 0;
}

void logFileWrite(uint8_t level, const char* line) {
    if (!_ready || level > LOG_FILE_MAX_LEVEL) return;

    size_t len = strlen(line);
    if (_curSize + len > LOG_FILE_MAX_BYTES) rotate();

    File f = LittleFS.open(LOG_FILE_PATH, "a");   // plain append -- no read-modify-rewrite
    if (!f) return;                                // best-effort; never blocks/crashes caller
    f.print(line);
    f.close();
    _curSize += len;
}
