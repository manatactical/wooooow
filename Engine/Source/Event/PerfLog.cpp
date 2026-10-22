#include "Engine/Source/Event/PerfLog.h"

#include <windows.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "Engine/Source/Os/TimeManager.h"
#include "Engine/Source/Storm/H/storm.h"

// NOT IN BINARY. See PerfLog.h.
//
// The file is opened for append and closed again per write rather than held open: a handle held for the
// life of the process cannot be read while the game is still running, which is exactly when the log is
// wanted. One write is one record.
//
// Threading: the frame itself is recorded by the scheduler thread, but two call sites are shared with
// worker threads -- the Model2 cache's garbage collector and the async file/upload poll. Everything that
// mutates state therefore takes the recursive lock below, and every recorded name is copied into its own
// storage so a worker thread cannot hand us a pointer that dies with its stack frame.
namespace {
// Recursive so the tick-end path can call the summary writer while already holding it.
CRITICAL_SECTION s_lock;

// Function-local static: initialised exactly once, thread-safely, before any logger call can use it.
void LockEnter() {
    static struct Guard {
        Guard() { InitializeCriticalSection(&s_lock); }
    } guard;

    (void)guard;
    EnterCriticalSection(&s_lock);
}

void LockLeave() {
    LeaveCriticalSection(&s_lock);
}

constexpr uint32_t kMaxSpans = 4;
constexpr uint32_t kMaxCounters = 12;
constexpr uint32_t kNameSize = 32;

char s_path[512];
// Read by hot paths (every Present, every stage) without the lock, so these two are atomic: the console
// thread can flip them mid-frame and a torn read would change which path the frame takes.
std::atomic<bool> s_enabled{ false };
std::atomic<uint32_t> s_thresholdMs{ 0xFFFFFFFF };
std::atomic<bool> s_openFailed{ false };
bool s_started;
bool s_headerWritten;
uint32_t s_summaryEveryMs = 5000;
uint32_t s_startMs;
uint32_t s_lastTickStart;
uint32_t s_lastSummaryMs;

// ---- current frame ----
void CopyName(char* out, const char* name) {
    name = (name && *name) ? name : "?(none)";

    uint32_t i = 0;

    while (name[i] && i < kNameSize - 1) {
        out[i] = name[i];
        i++;
    }

    out[i] = '\0';
}

bool SameName(const char* a, const char* b) {
    return a[0] == b[0] && !strcmp(a, b);
}

char s_stage[kNameSize];
bool s_stageOpen;
uint32_t s_stageEvent = 0xFFFFFFFF;
uint32_t s_stageStartMs;

char s_worstStage[kNameSize];
uint32_t s_worstEvent = 0xFFFFFFFF;
uint32_t s_worstMs;

// Spans measured around a call that already finished (async wait, present, GC, ...). Only the four
// longest per frame are kept, so a spike line names what actually hurt instead of everything measured.
struct Span {
    char name[kNameSize];
    uint32_t ms;
    char detail[kNameSize];
};
Span s_spans[kMaxSpans];
uint32_t s_spanCount;

struct Counter {
    char name[kNameSize];
    uint32_t value;
};
Counter s_counters[kMaxCounters];
uint32_t s_counterCount;

// ---- rolling window ----
// 1 ms buckets up to 1 s, then one overflow bucket: a percentile only needs the shape of the tail.
// Note this makes p99 optimistic on a window of fewer than ~100 frames, and hides how far past 1 s the
// worst frame went; max is printed beside it for exactly that reason.
uint32_t s_histogram[1024];
uint32_t s_windowFrames;
uint32_t s_windowWorkTotal;
uint32_t s_windowWorstMs;
uint32_t s_windowSpikes;
Counter s_windowCounters[kMaxCounters];
uint32_t s_windowCounterCount;

struct SpanTotal {
    char name[kNameSize];
    uint32_t totalMs;
    uint32_t count;
    uint32_t maxMs;
};
SpanTotal s_windowSpans[kMaxCounters * 4];
uint32_t s_windowSpanCount;

FILE* Open(const char* mode) {
    FILE* file = nullptr;
    fopen_s(&file, s_path, mode);

    if (!file) {
        // Reported once through `perf` rather than on every write: a read-only Logs directory would
        // otherwise turn a diagnostic into a stutter of its own.
        s_openFailed = true;
    }

    return file;
}

void WriteHeader(bool truncate) {
    FILE* file = Open(truncate ? "w" : "a");

    if (!file) {
        return;
    }

    fprintf(file, "# perf log for process %u, spike threshold %u ms, summary every %u ms\n",
            SGetCurrentProcessId(), s_thresholdMs.load(std::memory_order_relaxed), s_summaryEveryMs);
    fclose(file);
}

void ResetWindow(uint32_t now) {
    memset(s_histogram, 0, sizeof(s_histogram));
    s_windowFrames = 0;
    s_windowWorkTotal = 0;
    s_windowWorstMs = 0;
    s_windowSpikes = 0;
    s_windowCounterCount = 0;
    s_windowSpanCount = 0;
    s_lastSummaryMs = now;
}

uint32_t Percentile(double fraction) {
    if (!s_windowFrames) {
        return 0;
    }

    uint32_t target = static_cast<uint32_t>(fraction * s_windowFrames);
    uint32_t seen = 0;

    for (uint32_t i = 0; i < 1024; i++) {
        seen += s_histogram[i];

        if (seen >= target) {
            return i;
        }
    }

    return 1023;
}

void NoteSpanInWindow(const char* name, uint32_t ms) {
    for (uint32_t i = 0; i < s_windowSpanCount; i++) {
        if (SameName(s_windowSpans[i].name, name)) {
            s_windowSpans[i].totalMs += ms;
            s_windowSpans[i].count++;

            if (ms > s_windowSpans[i].maxMs) {
                s_windowSpans[i].maxMs = ms;
            }

            return;
        }
    }

    const uint32_t capacity = sizeof(s_windowSpans) / sizeof(s_windowSpans[0]);

    if (s_windowSpanCount == capacity) {
        return;
    }

    SpanTotal& total = s_windowSpans[s_windowSpanCount++];
    CopyName(total.name, name);
    total.totalMs = ms;
    total.count = 1;
    total.maxMs = ms;
}

Counter* CounterFor(Counter* list, uint32_t& count, const char* name) {
    for (uint32_t i = 0; i < count; i++) {
        if (SameName(list[i].name, name)) {
            return &list[i];
        }
    }

    if (count == kMaxCounters) {
        // Explicit bucket rather than folding into the last real name: a number filed under the wrong
        // label is worse than a number labelled "other".
        static char other[] = "(other)";
        list[count - 1].name[0] = '\0';
        CopyName(list[count - 1].name, other);

        return &list[count - 1];
    }

    Counter& counter = list[count++];
    CopyName(counter.name, name);
    counter.value = 0;

    return &counter;
}

void WriteSummaryLocked() {
    if (!s_windowFrames) {
        return;
    }

    FILE* file = Open("a");

    if (!file) {
        return;
    }

    uint32_t p50 = Percentile(0.50);
    uint32_t p95 = Percentile(0.95);
    uint32_t p99 = Percentile(0.99);
    uint32_t average = s_windowWorkTotal / s_windowFrames;

    fprintf(file, "# summary %8u ms  frames=%u  fps=%.1f  fps_low1=%.1f  frame p50=%u p95=%u p99=%u max=%u  work avg=%u ms  spikes=%u\n",
            OsGetAsyncTimeMs() - s_startMs, s_windowFrames,
            // NOT IN BINARY. Average and worst-1% frame rate, derived from the same histogram the
            // percentiles come from, so a fps claim can be checked against the frame times beside it.
            (p50 || s_windowWorstMs) ? 1000.0f / static_cast<float>(p50 ? p50 : s_windowWorstMs) : 0.0f,
            p99 ? 1000.0f / static_cast<float>(p99) : 0.0f,
            p50, p95, p99, s_windowWorstMs, average, s_windowSpikes);

    for (uint32_t i = 0; i < s_windowSpanCount; i++) {
        fprintf(file, "#   span  %-24s total=%7u ms  max=%5u ms  count=%u\n", s_windowSpans[i].name,
                s_windowSpans[i].totalMs, s_windowSpans[i].maxMs, s_windowSpans[i].count);
    }

    for (uint32_t i = 0; i < s_windowCounterCount; i++) {
        fprintf(file, "#   count %-25s %u\n", s_windowCounters[i].name, s_windowCounters[i].value);
    }

    fclose(file);
}

void WriteSpikeLine(uint32_t tickStart, uint32_t frameMs, uint32_t workMs) {
    char label[kNameSize];

    if (s_worstEvent != 0xFFFFFFFF) {
        snprintf(label, sizeof(label), "event %u", s_worstEvent);
    } else {
        snprintf(label, sizeof(label), "%s", s_worstStage[0] ? s_worstStage : "(none)");
    }

    FILE* file = Open("a");

    if (!file) {
        return;
    }

    fprintf(file, "%8u ms  frame=%5u  work=%5u  idle=%5u  worst=%-22s %5u ms",
            tickStart - s_startMs, frameMs, workMs, frameMs - workMs, label, s_worstMs);

    for (uint32_t i = 0; i < s_spanCount; i++) {
        if (s_spans[i].detail[0]) {
            fprintf(file, "  %s=%u ms(%s)", s_spans[i].name, s_spans[i].ms, s_spans[i].detail);
        } else {
            fprintf(file, "  %s=%u ms", s_spans[i].name, s_spans[i].ms);
        }
    }

    for (uint32_t i = 0; i < s_counterCount; i++) {
        fprintf(file, "  %s=%u", s_counters[i].name, s_counters[i].value);
    }

    fputc('\n', file);
    fclose(file);
}

// Closes the open stage and keeps it if it was the longest so far.
void CloseStageLocked(uint32_t now) {
    if (!s_stageOpen) {
        return;
    }

    uint32_t elapsed = now - s_stageStartMs;

    if (elapsed > s_worstMs) {
        s_worstMs = elapsed;
        CopyName(s_worstStage, s_stage);
        s_worstEvent = s_stageEvent;
    }
}
}  // namespace

bool PerfLogEnabled() {
    return s_enabled.load(std::memory_order_relaxed);
}

bool PerfLogOpenFailed() {
    return s_openFailed.load(std::memory_order_relaxed);
}

uint32_t PerfLogThreshold() {
    return s_thresholdMs.load(std::memory_order_relaxed);
}

void PerfLogSetSummaryInterval(uint32_t summaryEveryMs) {
    LockEnter();
    s_summaryEveryMs = summaryEveryMs ? summaryEveryMs : 5000;
    LockLeave();
}

void PerfLogSetThreshold(uint32_t thresholdMs, uint32_t summaryEveryMs) {
    LockEnter();

    if (summaryEveryMs) {
        s_summaryEveryMs = summaryEveryMs;
    }

    if (!thresholdMs) {
        // Off. The window running now is written first so turning the logger off does not throw away the
        // run it was describing.
        WriteSummaryLocked();
        s_enabled = false;
        s_thresholdMs = 0xFFFFFFFF;
        LockLeave();

        return;
    }

    if (!s_started) {
        s_started = true;
        snprintf(s_path, sizeof(s_path), "Logs\\spikes.%u.log", SGetCurrentProcessId());
        s_headerWritten = false;
    }

    // WOWLOG_QUIET=1 wins over an explicit `perf on`: the quiet switch is for runs where nothing should be
    // written, and the console command cannot be issued before the file exists anyway.
    if (!DiagnosticLogsEnabled()) {
        s_thresholdMs = thresholdMs;
        s_started = false;
        LockLeave();

        return;
    }

    // One header's worth of truncation per process: a run split by `perf off` / `perf on` keeps both
    // halves, but the previous process's file is never appended to.
    s_thresholdMs = thresholdMs;
    WriteHeader(!s_headerWritten);
    s_headerWritten = true;

    // The first tick after this call would otherwise be measured against a tick start from before the
    // logger was last turned off, and report that whole gap as one frame.
    s_startMs = OsGetAsyncTimeMs();
    ResetWindow(s_startMs);
    s_lastTickStart = s_startMs;
    s_enabled = true;

    LockLeave();
}

void PerfLogStage(const char* name) {
    if (!s_enabled) {
        return;
    }

    LockEnter();

    const uint32_t now = OsGetAsyncTimeMs();

    CloseStageLocked(now);

    CopyName(s_stage, name);
    s_stageOpen = true;
    s_stageEvent = 0xFFFFFFFF;
    s_stageStartMs = now;

    LockLeave();
}

void PerfLogStageEvent(uint32_t eventId) {
    if (!s_enabled) {
        return;
    }

    LockEnter();

    CopyName(s_stage, "handler");
    s_stageOpen = true;
    s_stageEvent = eventId;
    s_stageStartMs = OsGetAsyncTimeMs();

    LockLeave();
}

void PerfLogPaintEnd() {
    PerfLogStage("present");
}

// Shared by the frame thread and the worker threads that call it (M2 GC, async upload poll).
void PerfLogSpanDetail(const char* name, uint32_t ms, const char* detail) {
    if (!s_enabled || !ms) {
        return;
    }

    LockEnter();

    char spanName[kNameSize];
    CopyName(spanName, name);
    NoteSpanInWindow(spanName, ms);

    // Kept out of the per-spike list below 5 ms: a hundred 1 ms texture loads would otherwise push the one
    // 250 ms wait off the line. The window totals still count them.
    if (ms < 5) {
        LockLeave();

        return;
    }

    if (s_spanCount == kMaxSpans && ms <= s_spans[kMaxSpans - 1].ms) {
        LockLeave();

        return;
    }

    Span span;
    memcpy(span.name, spanName, sizeof(span.name));
    span.ms = ms;
    span.detail[0] = '\0';

    if (detail && *detail) {
        // Leaf name only: a full texture path is 60 characters of noise in a column beside five numbers.
        const char* leaf = detail;

        for (const char* c = detail; *c; c++) {
            if (*c == '\\' || *c == '/') {
                leaf = c + 1;
            }
        }

        CopyName(span.detail, leaf);
    }

    uint32_t slot = s_spanCount < kMaxSpans ? s_spanCount++ : kMaxSpans - 1;

    // Insertion sort, longest first. When the list is full the old tail is overwritten by the shift.
    while (slot > 0 && s_spans[slot - 1].ms < ms) {
        s_spans[slot] = s_spans[slot - 1];
        slot--;
    }

    s_spans[slot] = span;

    LockLeave();
}

void PerfLogSpan(const char* name, uint32_t ms) {
    PerfLogSpanDetail(name, ms, nullptr);
}

void PerfLogCount(const char* name, uint32_t amount) {
    if (!s_enabled || !amount) {
        return;
    }

    LockEnter();

    char counterName[kNameSize];
    CopyName(counterName, name);

    CounterFor(s_counters, s_counterCount, counterName)->value += amount;
    CounterFor(s_windowCounters, s_windowCounterCount, counterName)->value += amount;

    LockLeave();
}

void PerfLogTickStart() {
    if (!s_enabled) {
        return;
    }

    LockEnter();

    s_stageOpen = false;
    s_stageEvent = 0xFFFFFFFF;
    s_worstStage[0] = '\0';
    s_worstEvent = 0xFFFFFFFF;
    s_worstMs = 0;
    s_spanCount = 0;
    s_counterCount = 0;

    LockLeave();
}

void PerfLogTickEnd(uint32_t tickStart, uint32_t now) {
    if (!s_enabled) {
        return;
    }

    LockEnter();

    uint32_t frameMs = tickStart - s_lastTickStart;
    uint32_t workMs = now - tickStart;

    // The frame's last stage is never closed by the next PerfLogStage() call, so close it against `now`
    // here rather than letting it run into the next frame's first stage.
    CloseStageLocked(now);
    s_stageOpen = false;

    s_windowFrames++;
    s_windowWorkTotal += workMs;

    if (frameMs > s_windowWorstMs) {
        s_windowWorstMs = frameMs;
    }

    s_histogram[frameMs < 1024 ? frameMs : 1023]++;

    // Read the flag, not the raw member: a `perf off` from the console can land between the tick start and
    // here, and a spike line written after the disable is harmless but a torn read is not.
    if (frameMs >= s_thresholdMs.load(std::memory_order_relaxed)) {
        s_windowSpikes++;

        // s_lastTickStart is still the previous frame's start here, which is what made frameMs above.
        WriteSpikeLine(tickStart, frameMs, workMs);
    }

    s_lastTickStart = tickStart;

    if (s_summaryEveryMs && now - s_lastSummaryMs >= s_summaryEveryMs) {
        WriteSummaryLocked();
        ResetWindow(now);
    }

    LockLeave();
}

void PerfLogWriteSummary() {
    if (!s_enabled) {
        return;
    }

    LockEnter();
    WriteSummaryLocked();
    ResetWindow(OsGetAsyncTimeMs());
    LockLeave();
}

// NOT IN BINARY. See PerfLog.h. Three states, so the unset case cannot be mistaken for a deliberate "off":
// never asked, asked-and-enabled, asked-and-disabled. Only an explicit truthy value (1/true/yes/on) asks
// for quiet; anything else, including a typo, leaves diagnostics running. Failing towards "logging on" is
// deliberate: a mistyped switch that silently discarded a diagnostic would be found only by not finding it.
bool DiagnosticLogsEnabled() {
    enum { Unasked, Enabled, Disabled };
    static std::atomic<int> s_state{ Unasked };

    const int state = s_state.load(std::memory_order_acquire);

    if (state != Unasked) {
        return state == Enabled;
    }

    const char* value = getenv("WOWLOG_QUIET");

    const bool quiet = value &&
                       (_stricmp(value, "1") == 0 || _stricmp(value, "true") == 0 ||
                        _stricmp(value, "yes") == 0 || _stricmp(value, "on") == 0);

    s_state.store(quiet ? Disabled : Enabled, std::memory_order_release);

    return !quiet;
}
