#ifndef ENGINE_SOURCE_MODERNASSETS_MODERNASSETSLOG_H
#define ENGINE_SOURCE_MODERNASSETS_MODERNASSETSLOG_H

// Logging seam for the modern-asset readers (DB2 tables, FileDataID resolution, modern M2).
//
// DebugLog() is compiled out of release builds (NDEBUG in Storm/H/storm.h), which is exactly where
// these paths need to explain themselves, so this writes to Logs\modern-assets.log on first use and
// mirrors to the debugger. No log file is created until something is actually logged.
namespace modern::assets
{
    void LogWrite(const char* level, const char* format, ...);
}

#define MODERN_ASSETS_LOG_INFO(...)  ::modern::assets::LogWrite("info", __VA_ARGS__)
#define MODERN_ASSETS_LOG_WARN(...)  ::modern::assets::LogWrite("warn", __VA_ARGS__)

#endif
