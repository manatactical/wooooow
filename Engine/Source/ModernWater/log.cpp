#include "Engine/Source/ModernWater/log.h"

#include "Engine/Source/Gx/CGxDevice/CGxDevice.h"

#include <cstdio>

namespace {

int s_level = static_cast<int>(LogLevel::Info);

const char* LevelName(LogLevel level) {
    switch (level) {
        case LogLevel::Error:
            return "error";
        case LogLevel::Debug:
            return "debug";
        default:
            return "info";
    }
}

}  // namespace

// Not in the original binary. The WXL module opened its own log file beside the DLL. Here the client already
// has a log and a place to put it, so the path is taken and ignored rather than opening a second file.
void LogOpen(const char* path) {
    (void)path;
}

void LogSetLevel(int level) {
    s_level = level;
}

bool LogEnabled(LogLevel level) {
    return static_cast<int>(level) <= s_level;
}

void LogWrite(LogLevel level, const char* fmt, ...) {
    if (!LogEnabled(level)) {
        return;
    }

    char message[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(message, sizeof(message), fmt, args);
    va_end(args);

    CGxDevice::Log("ModernWater %s: %s\n", LevelName(level), message);
}
