#include "Engine/Source/ModernAssets/ModernAssetsLog.h"

#include "Engine/Source/Event/PerfLog.h"

#include <windows.h>
#include <cstdarg>
#include <cstdio>

namespace modern::assets
{
    namespace
    {
        // Function-local static: initialised exactly once, thread-safely, before any log call can use
        // it. A lazily-initialised "if (!ready) InitializeCriticalSection" pair is not safe here -- two
        // threads can both see the flag clear and initialise the same section twice.
        struct LogLock
        {
            CRITICAL_SECTION section;
            LogLock() { InitializeCriticalSection(&section); }
        };

        LogLock& LockObject()
        {
            static LogLock lock;
            return lock;
        }

        void Lock()   { EnterCriticalSection(&LockObject().section); }
        void Unlock() { LeaveCriticalSection(&LockObject().section); }

        FILE*& FileSlot()
        {
            static FILE* file = nullptr;
            return file;
        }

        bool& FileTried()
        {
            static bool tried = false;
            return tried;
        }

        /// Opens the log once. Must be called with the lock held.
        FILE* File()
        {
            if (FileTried()) return FileSlot();
            FileTried() = true;

            // WOWLOG_QUIET=1: no Logs\modern-assets.log at all, not even created. The debugger output in
            // LogWrite still happens, so a run under a debugger keeps its diagnostics.
            if (!DiagnosticLogsEnabled()) return nullptr;

            CreateDirectoryA("Logs", nullptr);
            FileSlot() = std::fopen("Logs\\modern-assets.log", "a");
            if (!FileSlot())
                OutputDebugStringA("modern-assets: cannot open Logs\\modern-assets.log; "
                                   "logging goes to the debugger only\n");
            return FileSlot();
        }
    }

    void LogWrite(const char* level, const char* format, ...)
    {
        if (!format) return;

        char message[1024];
        va_list args;
        va_start(args, format);
        const int wrote = std::vsnprintf(message, sizeof(message), format, args);
        va_end(args);
        if (wrote < 0) message[0] = '\0';

        char line[1100];
        std::snprintf(line, sizeof(line), "[%s] %s%s\n", level ? level : "info", message,
                      (wrote >= static_cast<int>(sizeof(message))) ? " ..." : "");

        Lock();
        OutputDebugStringA(line);
        if (FILE* file = File())
        {
            std::fputs(line, file);
            std::fflush(file);
        }
        Unlock();
    }
}
