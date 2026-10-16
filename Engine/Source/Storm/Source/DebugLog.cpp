#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <windows.h>
#include "Engine/Source/Storm/H/storm.h"

#ifndef NDEBUG
namespace {

// Not in the original binary, but behaviour preserving: OutputDebugStringA produces no output at all
// when no debugger is listening, so the formatting work below was pure cost. Per-batch callers in
// CM2SceneRender made that cost 44% of a frame (3.42ms of 7.70ms, 934 formatted batches). -1 means
// "decide on first use", i.e. on iff a debugger is attached.
int32_t s_debugLogEnabled = -1;

bool DebugLogIsEnabled() {
    if (s_debugLogEnabled < 0) {
        s_debugLogEnabled = IsDebuggerPresent() ? 1 : 0;
    }

    return s_debugLogEnabled != 0;
}

}  // namespace

void DebugLogSetEnabled(int32_t enabled) {
    // Negative restores the automatic "on iff a debugger is attached" behaviour.
    s_debugLogEnabled = enabled < 0 ? -1 : (enabled != 0 ? 1 : 0);
}

void DebugLog(const char* format, ...) {
    if (!DebugLogIsEnabled()) {
        return;
    }

    char buffer[1024];

    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);

    OutputDebugStringA(buffer);

    size_t length = strlen(buffer);

    if (length == 0 || buffer[length - 1] != '\n') {
        OutputDebugStringA("\n");
    }
}

void DebugLog(int32_t target, const char* format, ...) {
    if (!DebugLogIsEnabled()) {
        return;
    }

    char buffer[1024];

    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);

    DebugLog("%s", buffer);
}

void DebugValidate(const char* expression) {
    DebugLog("Validation failed: %s", expression);
}

#endif