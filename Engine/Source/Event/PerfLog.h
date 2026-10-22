#ifndef ENGINE_SOURCE_EVENT_PERFLOG_H
#define ENGINE_SOURCE_EVENT_PERFLOG_H

#include <cstdint>

// NOT IN BINARY. Frame/perf instrumentation. Writes Logs\spikes.<pid>.log.
//
// Off until something turns it on: the WOWLOG_SPIKES environment variable (its value is the spike
// threshold in whole ms), PerfLogSetThreshold(), or the `perf` console command. Every entry point
// starts with a flag test, so an off logger costs one predictable branch per call site.
//
// Two kinds of record go in the file:
//   - one line per frame at or over the threshold, naming the worst stage, the worst measured span
//     (async wait, present, GC, ...) and the frame's counters;
//   - a rolling summary every PerfLogSetSummaryInterval() ms with frame percentiles and the phase /
//     counter totals for that window.
//
// Thread-safe: the frame is recorded on the scheduler thread, but the M2 garbage collector and the async
// file/upload poll run on worker threads and report through the same entry points. All recorded names are
// copied, so a caller may pass a stack buffer.
//
// Phase accounting is a simple one-deep nesting: PerfLogStage() closes whatever stage was open and
// opens the named one, so callers just mark points in a linear frame and need no stack.
void PerfLogStage(const char* name);
void PerfLogStageEvent(uint32_t eventId);
void PerfLogPaintEnd();

// A span measured around a call that already happened, e.g. an async file wait or Present().
// `detail` is optional and copied (a file name, an event name); keep it short.
void PerfLogSpan(const char* name, uint32_t ms);
void PerfLogSpanDetail(const char* name, uint32_t ms, const char* detail);

// Adds one counter for the current frame, e.g. textures evicted, models GC'd.
void PerfLogCount(const char* name, uint32_t amount);

// Frame boundaries. TickStart runs on every tick so a phase from the previous frame is never
// attributed to the next one; TickEnd does the bookkeeping and writes.
void PerfLogTickStart();
void PerfLogTickEnd(uint32_t tickStart, uint32_t now);

// 0 disables. Any other value is the spike threshold and writes the header immediately.
void PerfLogSetThreshold(uint32_t thresholdMs, uint32_t summaryEveryMs);
void PerfLogSetSummaryInterval(uint32_t summaryEveryMs);
uint32_t PerfLogThreshold();
bool PerfLogEnabled();

// True once a write has failed, e.g. because Logs is read-only, so the console status can say so instead
// of claiming the logger is recording when it is not.
bool PerfLogOpenFailed();

// Writes the rolling summary now and starts a new window.
void PerfLogWriteSummary();

// NOT IN BINARY. The switch for the diagnostic dumps this tree adds on top of the original client
// (Logs\m2-sequences.csv, modern-assets.log, animationdata-client.db2, custom_mpq.log, unitoutline.log,
// texaudit.log, spikes.<pid>.log). WOWLOG_QUIET=1 (also true/yes/on) silences them; unset or anything else,
// including a typo, leaves them enabled, so behaviour is unchanged unless explicitly asked. Not the original
// client's own logs: gx.log, cpu.log, SESound.log and the FrameXML/GlueXML status files are written
// regardless, because they are the stock client's behaviour and in several places the only record left
// after a crash.
//
// Read once and cached: the answer must not change mid-run, or a dump could start and then stop writing.
bool DiagnosticLogsEnabled();

#endif
