#ifndef ENGINE_SOURCE_MODERNWATER_LOG_H
#define ENGINE_SOURCE_MODERNWATER_LOG_H

#include <cstdarg>

// Not in the original 3.3.5a binary. Modern water's logging, the engine equivalent of the WXL module's
// log.cpp. Everything goes to CGxDevice::Log, which is where the client's own gx diagnostics land, so the
// water's lines sit next to them in the one log a player already knows how to send.
//
// The levels are kept because the ported sources are full of VF_LOG_INFO / VF_LOG_DEBUG calls that are
// worth keeping: they are the only record of why the pass stood down on a given machine. Info is on,
// Debug is off, matching the WXL module's default LogLevel=1.
enum class LogLevel : int { Error = 0, Info = 1, Debug = 2 };

void LogOpen(const char* path);
void LogSetLevel(int level);
bool LogEnabled(LogLevel level);
void LogWrite(LogLevel level, const char* fmt, ...);

#define VF_LOG_ERROR(...) LogWrite(LogLevel::Error, __VA_ARGS__)
#define VF_LOG_INFO(...) LogWrite(LogLevel::Info, __VA_ARGS__)
#define VF_LOG_DEBUG(...) LogWrite(LogLevel::Debug, __VA_ARGS__)

#endif
