// Not in the original binary. Diagnostic only: the client is essentially single threaded (main
// thread + one disk queue thread + FMOD), so this records which threads exist, each thread's
// preferred core, and how much CPU each burned in the interval - enough to show whether cores 3..N
// are idle because nothing runs there, or because the process is kept off them.
//
// Deliberately narrow: it reports the *ideal* (preferred) processor, not where a thread actually
// last ran. Windows exposes the latter only through per-thread cycle counters. Affinity is reported
// once for the whole process, because a stale processAffinityMask in WTF is what silently pins the
// client to one or two cores.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <windows.h>
#include <tlhelp32.h>
#include "Engine/Source/Os/System.h"
#include "Engine/Source/Os/TimeManager.h"
#include "Engine/Source/Storm/H/storm.h"

namespace {

struct ThreadStat {
    uint32_t m_threadId;
    int32_t m_priority;      // THREAD_PRIORITY_ERROR_RETURN encoded as -999
    uint64_t m_lifetimeTicks;  // raw GetThreadTimes total, never rewritten
    uint64_t m_deltaTicks;     // CPU in the interval, derived from m_lifetimeTicks
    int32_t m_idealCore;     // group * 64 + processor number, -1 when unknown
    char m_name[32];
};

const uint32_t kMaxThreads = 64;
const uint32_t kSnapshotIntervalMs = 2000;
const uint64_t kBusyThresholdTicks = 500000;  // 50ms expressed in 100ns ticks

ThreadStat s_prev[kMaxThreads];
uint32_t s_prevCount;
bool s_havePrev;
uint32_t s_lastSnapshotMs;

uint64_t FileTimeToTicks(const FILETIME& time) {
    return (static_cast<uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
}

int32_t CountBits32(uint32_t value) {
    int32_t count = 0;

    while (value) {
        value &= value - 1;
        count++;
    }

    return count;
}

// Who the process is allowed to run on. Zero mask means every core. Returns false only when the
// query itself fails, so the caller does not report an API failure as "all cores".
bool WantedAffinity(uint64_t& mask) {
    DWORD_PTR processMask = 0;
    DWORD_PTR systemMask = 0;

    if (!GetProcessAffinityMask(GetCurrentProcess(), &processMask, &systemMask)) {
        return false;
    }

    mask = static_cast<uint64_t>(processMask);
    return true;
}

void DumpThreadList(HSLOG log) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);

    if (snapshot == INVALID_HANDLE_VALUE) {
        SLogWrite(log, "threads: CreateToolhelp32Snapshot failed (%lu)", GetLastError());
        return;
    }

    DWORD processId = GetCurrentProcessId();
    ThreadStat current[kMaxThreads] = {};
    uint32_t count = 0;

    THREADENTRY32 entry = {};
    entry.dwSize = sizeof(entry);

    if (Thread32First(snapshot, &entry)) {
        do {
            if (entry.th32OwnerProcessID != processId || count >= kMaxThreads) {
                continue;
            }

            HANDLE thread = OpenThread(THREAD_QUERY_INFORMATION, FALSE, entry.th32ThreadID);

            if (!thread) {
                continue;
            }

            ThreadStat& stat = current[count];
            stat.m_threadId = entry.th32ThreadID;
            stat.m_priority = GetThreadPriority(thread);
            stat.m_idealCore = -1;

            if (stat.m_priority == THREAD_PRIORITY_ERROR_RETURN) {
                stat.m_priority = -999;
            }

            PROCESSOR_NUMBER processor = {};

            if (GetThreadIdealProcessorEx(thread, &processor)) {
                stat.m_idealCore = processor.Group * 64 + processor.Number;
            }

            FILETIME creation = {};
            FILETIME exit = {};
            FILETIME kernel = {};
            FILETIME user = {};

            if (GetThreadTimes(thread, &creation, &exit, &kernel, &user)) {
                stat.m_lifetimeTicks = FileTimeToTicks(kernel) + FileTimeToTicks(user);
            }

            // m_name is zero initialized by the array above and SGetThreadName bounds its copy.
            SGetThreadName(entry.th32ThreadID, stat.m_name, sizeof(stat.m_name));

            CloseHandle(thread);
            count++;
        } while (Thread32Next(snapshot, &entry));
    }

    CloseHandle(snapshot);

    uint64_t affinity = 0;
    char line[512];

    if (!WantedAffinity(affinity)) {
        SLogWrite(log, "affinity: query failed (%lu)", GetLastError());
    } else if (affinity == 0) {
        SLogWrite(log, "affinity: default (all logical cores)");
    } else {
        // Only the low 32 bits are meaningful; the client's own processAffinityMask CVar is 32 bit.
        _snprintf_s(line, sizeof(line), _TRUNCATE, "affinity: mask=%08X cores=%d logical=%d processCores=%d",
                    static_cast<uint32_t>(affinity), CountBits32(static_cast<uint32_t>(affinity)),
                    static_cast<int32_t>(GetActiveProcessorCount(ALL_PROCESSOR_GROUPS)), static_cast<int32_t>(OsGetNumberOfProcessors()));
        SLogWrite(log, "%s", line);
    }

    _snprintf_s(line, sizeof(line), _TRUNCATE, "threads: count=%u, busy threshold=50ms per %ums, cpu '%s'", count, kSnapshotIntervalMs, s_havePrev ? "interval delta" : "lifetime n/a");
    SLogWrite(log, "%s", line);
    SLogWrite(log, "  tid      name                     ideal prio   cpu");

    uint32_t busy = 0;
    uint32_t idle = 0;

    for (uint32_t i = 0; i < count; i++) {
        ThreadStat& stat = current[i];
        bool firstDump = true;

        for (uint32_t p = 0; p < s_prevCount; p++) {
            if (s_prev[p].m_threadId != stat.m_threadId) {
                continue;
            }

            // On the first dump there is no earlier sample, so the counter is lifetime CPU and
            // must not be reported as an interval delta. Subtract from m_lifetimeTicks, never from
            // m_deltaTicks: s_prev is a copy of this array and would otherwise hold deltas, making
            // every later snapshot differ a delta against a lifetime.
            if (s_havePrev) {
                stat.m_deltaTicks = stat.m_lifetimeTicks >= s_prev[p].m_lifetimeTicks ? stat.m_lifetimeTicks - s_prev[p].m_lifetimeTicks : 0;
                firstDump = false;
            }

            break;
        }

        if (!firstDump && stat.m_deltaTicks < kBusyThresholdTicks) {
            idle++;
            continue;
        }

        busy++;
        SLogWrite(log, "  %-8u %-24s %-5d %-5d  %llums", stat.m_threadId, stat.m_name, stat.m_idealCore, stat.m_priority, static_cast<unsigned long long>(stat.m_deltaTicks / 10000));
    }

    // Idle threads are counted rather than listed, so presence is still recorded without one line
    // per sleeping worker.
    if (idle) {
        SLogWrite(log, "  (%u thread(s) below threshold, not listed)", idle);
    }

    // Threads that existed last snapshot and are gone now.
    for (uint32_t p = 0; p < s_prevCount; p++) {
        bool alive = false;

        for (uint32_t i = 0; i < count; i++) {
            if (current[i].m_threadId == s_prev[p].m_threadId) {
                alive = true;
                break;
            }
        }

        if (!alive) {
            SLogWrite(log, "  %-8u %-24s (exited)", s_prev[p].m_threadId, s_prev[p].m_name);
        }
    }

    memcpy(s_prev, current, sizeof(ThreadStat) * count);
    s_prevCount = count;
}

}  // namespace

// Called once from main(), before the render thread exists, so it cannot race with the snapshots.
void OsLogThreadStartup(int32_t timerResolutionResult) {
    HSLOG log;

    if (!SLogCreate("Logs\\threads.log", 0, &log)) {
        return;
    }

    uint64_t affinity = 0;
    bool affinityKnown = WantedAffinity(affinity);

    SLogWrite(log, "startup: pid=%u logicalCores=%d processCores=%d affinityMask=%s", GetCurrentProcessId(), static_cast<int32_t>(GetActiveProcessorCount(ALL_PROCESSOR_GROUPS)), static_cast<int32_t>(OsGetNumberOfProcessors()), affinityKnown ? "see below" : "query failed");
    SLogWrite(log, "startup: processPriority=0x%X (0x80=HIGH) threadPriority=%d timerResolution=%s result=%d", static_cast<uint32_t>(GetPriorityClass(GetCurrentProcess())), GetThreadPriority(GetCurrentThread()), OsTimerResolutionEnabled() ? "1ms" : "default", timerResolutionResult);

    DumpThreadList(log);
    SLogClose(log);

    // s_lastSnapshotMs is deliberately left at 0. This runs before CommonMain() calls
    // OsTimeStartup(), so s_OsTimeMgr does not exist yet and OsGetAsyncTimeMs() would fault.
    // The first OsLogThreadSnapshot() baselines the clock instead.
    s_havePrev = true;
}

// Called from WaitForFPSCap() once per silent window; cheap enough to run on the render thread.
// Only ever runs after OsLogThreadStartup() completed on the main thread, so s_prev is not raced.
// Also only runs once the render loop is up, which is after OsTimeStartup(), so s_OsTimeMgr exists.
void OsLogThreadSnapshot() {
    if (!s_havePrev) {
        return;
    }

    uint32_t now = OsGetAsyncTimeMs();

    // First call after startup: take the time base without spending a dump on it.
    if (!s_lastSnapshotMs) {
        s_lastSnapshotMs = now;
        return;
    }

    if (now - s_lastSnapshotMs < kSnapshotIntervalMs) {
        return;
    }

    s_lastSnapshotMs = now;

    HSLOG log;

    if (!SLogCreate("Logs\\threads.log", 4, &log)) {
        return;
    }

    SLogWrite(log, "-- snapshot t=%ums", now);
    DumpThreadList(log);
    SLogClose(log);
}