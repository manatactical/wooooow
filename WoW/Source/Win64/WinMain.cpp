#if defined(WOWDECOMP_ARCH_64)

#include <windows.h>
#include <cstdlib>
#include <float.h>
#include "Engine/Source/Os/System.h"
#include "Engine/Source/Os/TimeManager.h"
#include "Engine/Source/Storm/H/storm.h"
#include "WoW/Source/Client.h"

// STATIC: 0x00B311E8
static LPVOID lpFiber;

static void InvalidParameterHandler(const wchar_t* expression, const wchar_t* function, const wchar_t* file, unsigned int line, uintptr_t reserved) {
}

static bool ReturnFalse() {
    return false;
}

extern "C" int WinMainCRTStartup();

// OFFSET: 0x00401000
extern "C" int start() {
    StormRtlInitialize();
    return WinMainCRTStartup();
}

// OFFSET: 0x00406D70
int main() {
    _set_invalid_parameter_handler(InvalidParameterHandler);
    _clearfp();
    _control87(0x9001F & ~(_MCW_PC | _MCW_IC), _MCW_DN | _MCW_EM | _MCW_RC);
    ReturnFalse();

    // Not in the original binary. The engine only raised priority inside OsTimeManager::Calibrate()
    // and restored it, so the game ran at NORMAL_PRIORITY_CLASS. Same call the engine already uses
    // there, this time kept for the whole process lifetime.
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);

    CreateDirectoryA("Logs", nullptr);

    int32_t timerResolution = OsApplyTimerResolution(1);

    // DIAG: writes Logs\threads.log with the real thread list, per-thread core and per-thread CPU.
    OsLogThreadStartup(timerResolution);

    HSLOG log;

    if (SLogCreate("Logs\\frame_limiter.log", 4, &log)) {
        SLogWrite(log, "main: processPriority=%s threadPriority=%d", GetPriorityClass(GetCurrentProcess()) == HIGH_PRIORITY_CLASS ? "HIGH" : "other", GetThreadPriority(GetCurrentThread()));
        SLogClose(log);
    }

    return CommonMain();
}

// OFFSET: 0x0040B7D0
static VOID CALLBACK MainFiberProc(LPVOID parameter) {
    *static_cast<int*>(parameter) = main();
    SwitchToFiber(lpFiber);
}

// OFFSET: 0x0040B7F0
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nShowCmd) {
    lpFiber = ConvertThreadToFiber(nullptr);

    if (!lpFiber) {
        return -1;
    }

    int result = -1;
    LPVOID fiber = CreateFiberEx(0, 0x800000, 0, MainFiberProc, &result);

    if (!fiber) {
        return -1;
    }

    SwitchToFiber(fiber);
    DeleteFiber(fiber);

    return result;
}

#endif
