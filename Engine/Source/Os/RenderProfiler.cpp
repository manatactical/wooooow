// Not in the original binary. See RenderProfiler.h. Diagnostic only, safe to delete.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include "Engine/Source/Os/RenderProfiler.h"
#include "Engine/Source/Os/System.h"
#include "Engine/Source/Os/TimeManager.h"
#include "Engine/Source/Storm/H/storm.h"

namespace {

const char* s_phaseNames[RenderPhase_Count] = {
    "frame",
    "worldRender",
    "uiLayers",
    "gxuFont",
    "present",
    "worldScene",
    "m2Draw",
    "m2Setup",
    "m2Log",
    "m2BoneUpload",
    "m2SetState",
    "m2DrawCall",
    "drawLists",
    "spellVisuals",
    "particulate",
    "nameplates",
    "ffxFinish",
    "updatePools",
    "occluders",
    "viewerMapObj",
    "cullSort",
    "cullPortal",
    "cullMapObj",
    "sortedModels",
    "m2Animate",
    "chunks",
    "mapObjGroups",
    "horizon",
    "portals",
    "entities",
    "liquid",
    "gxuScene",
    "shadowQuery",
    "collisionDebug",
    "cullTblChunks",
    "cullTblMapObj",
    "cullTblLiquid",
    "cullTblEntitys",
    "cullTblDoodads",
    "cullTblOccl",
    "occlHoledGuarded",
    "occlHoledAll",
    "occlOccluders",
    "clipBufferUpdate",
    "occlProj",
    "occlScatter",
};

// Per-frame accumulators, in QPC ticks.
uint64_t s_phaseTicks[RenderPhase_Count];
uint32_t s_phaseCalls[RenderPhase_Count];
bool s_inFrame;
uint64_t s_frameStart;

// DIAG: draw submissions per render group (indexed by phase), plus the innermost open scope so a draw
// can be attributed to the group that issued it. s_depth guards the restore when scopes nest.
int32_t s_currentPhase = RenderPhase_Frame;
uint32_t s_frameDraws[RenderPhase_Count];
uint32_t s_windowDraws[RenderPhase_Count];

// Rolling window, so the report is not dominated by a single hitchy frame.
uint64_t s_windowTicks[RenderPhase_Count];
uint32_t s_windowCalls[RenderPhase_Count];
uint32_t s_windowFrames;
uint32_t s_windowStartMs;

// M2 batches submitted this frame / this window. Not a phase: a count, so the per-batch cost is sane.
uint32_t s_frameBatches;
uint32_t s_windowBatches;
uint32_t s_frameOpaqueBefore;
uint32_t s_frameOpaqueAfter;
uint32_t s_windowOpaqueBefore;
uint32_t s_windowOpaqueAfter;
uint32_t s_fintOutOfRange;
uint32_t s_windowFintOutOfRange;

int32_t s_enabled = 1;

// QPC ticks per millisecond, resolved once. OsGetAsyncClocksPerSecond is cheap but this runs in
// the inner scope destructor, so cache it.
double s_ticksPerMs;

double TicksPerMs() {
    if (s_ticksPerMs == 0.0) {
        int64_t perSecond = OsGetAsyncClocksPerSecond();

        // A zero or negative frequency would divide by zero below; fall back to "ticks are ms".
        s_ticksPerMs = perSecond > 0 ? static_cast<double>(perSecond) * 0.001 : 1.0;
    }

    return s_ticksPerMs;
}

}  // namespace

const char* RenderProfilerPhaseName(RenderProfilePhase phase) {
    if (phase < 0 || phase >= RenderPhase_Count) {
        return "?";
    }

    return s_phaseNames[phase];
}

void RenderProfilerCountM2Batch() {
    if (s_enabled && s_inFrame) {
        s_frameBatches++;
    }
}

void RenderProfilerCountFintOutOfRange() {
    s_fintOutOfRange++;
}

int32_t RenderProfilerCurrentPhase() {
    return s_currentPhase;
}

void RenderProfilerCountDraw(int32_t phase) {
    if (s_enabled && s_inFrame && phase >= 0 && phase < RenderPhase_Count) {
        s_frameDraws[phase]++;
    }
}

void RenderProfilerCountOpaqueDrawCalls(uint32_t before, uint32_t after) {
    if (s_enabled && s_inFrame) {
        s_frameOpaqueBefore += before;
        s_frameOpaqueAfter += after;
    }
}

RenderProfileClipBufferScope::RenderProfileClipBufferScope() {
    // Deliberately NOT gated on s_inFrame: UpdateOcclusion runs inside the frame, but this scope also
    // guards against a zeroed timestamp when profiling is off.
    this->m_start = (s_enabled && s_inFrame) ? OsGetAsyncTimeClocks() : 0;
}

RenderProfileClipBufferScope::~RenderProfileClipBufferScope() {
    if (!this->m_start || !s_inFrame) {
        return;
    }

    s_phaseTicks[RenderPhase_CullBufferUpdate] += OsGetAsyncTimeClocks() - this->m_start;
    s_phaseCalls[RenderPhase_CullBufferUpdate]++;
}

uint64_t& RenderProfilerOcclProjTicks() {
    return s_phaseTicks[RenderPhase_OcclProj];
}

uint64_t& RenderProfilerOcclScatterTicks() {
    return s_phaseTicks[RenderPhase_OcclScatter];
}

RenderProfileOcclusionSubScope::RenderProfileOcclusionSubScope(uint64_t& accumulator) : m_accumulator(accumulator) {
    this->m_start = (s_enabled && s_inFrame) ? OsGetAsyncTimeClocks() : 0;
}

RenderProfileOcclusionSubScope::~RenderProfileOcclusionSubScope() {
    if (!this->m_start || !s_inFrame) {
        return;
    }

    this->m_accumulator += OsGetAsyncTimeClocks() - this->m_start;

    // Calls are counted on the proj scope only, since both halves run once per UpdateBufferChunk call.
    if (&this->m_accumulator == &s_phaseTicks[RenderPhase_OcclProj]) {
        s_phaseCalls[RenderPhase_OcclProj]++;
    }
}

RenderProfileScope::RenderProfileScope(RenderProfilePhase phase) {
    this->m_phase = phase;
    this->m_start = 0;
    this->m_savedPhase = s_currentPhase;

    if (!s_enabled || !s_inFrame) {
        return;
    }

    this->m_start = OsGetAsyncTimeClocks();

    s_currentPhase = phase;
}

RenderProfileScope::~RenderProfileScope() {
    // Restore unconditionally: the push in the constructor happened iff m_start is set, and the
    // constructor's early return leaves m_start == 0.
    if (this->m_start) {
        s_currentPhase = this->m_savedPhase;
    }

    if (!this->m_start || !s_inFrame) {
        return;
    }

    uint64_t elapsed = OsGetAsyncTimeClocks() - this->m_start;

    s_phaseTicks[this->m_phase] += elapsed;
    s_phaseCalls[this->m_phase]++;
}

void RenderProfilerSetEnabled(int32_t enabled) {
    s_enabled = enabled != 0;
}

int32_t RenderProfilerEnabled() {
    return s_enabled;
}

void RenderProfilerBeginFrame() {
    memset(s_phaseTicks, 0, sizeof(s_phaseTicks));
    memset(s_phaseCalls, 0, sizeof(s_phaseCalls));
    s_frameBatches = 0;
    s_frameOpaqueBefore = 0;
    s_frameOpaqueAfter = 0;

    // Fresh frame: no scope is open yet, and the draw-group counters start clean.
    s_currentPhase = RenderPhase_Frame;
    memset(s_frameDraws, 0, sizeof(s_frameDraws));

    // The frame phase is measured here and in EndFrame rather than by a scope object: a scope would
    // have to outlive EndFrame(), which fights the RAII order at the call site.
    s_frameStart = OsGetAsyncTimeClocks();
    s_inFrame = true;
}

void RenderProfilerEndFrame() {
    s_inFrame = false;

    if (!s_enabled) {
        return;
    }

    s_phaseTicks[RenderPhase_Frame] = OsGetAsyncTimeClocks() - s_frameStart;
    s_phaseCalls[RenderPhase_Frame] = 1;

    s_windowFrames++;
    s_windowBatches += s_frameBatches;
    s_windowOpaqueBefore += s_frameOpaqueBefore;
    s_windowOpaqueAfter += s_frameOpaqueAfter;
    s_windowFintOutOfRange += s_fintOutOfRange;
    s_fintOutOfRange = 0;

    for (int32_t i = 0; i < RenderPhase_Count; i++) {
        s_windowTicks[i] += s_phaseTicks[i];
        s_windowCalls[i] += s_phaseCalls[i];
        s_windowDraws[i] += s_frameDraws[i];
    }

    uint32_t now = OsGetAsyncTimeMs();

    if (!s_windowStartMs) {
        s_windowStartMs = now;
        return;
    }

    if (s_windowFrames < 240 && now - s_windowStartMs < 2000) {
        return;
    }

    double ticksPerMs = TicksPerMs();
    HSLOG log;

    if (!SLogCreate("Logs\\render_profile.log", 4, &log)) {
        s_windowFrames = 0;
        s_windowStartMs = now;
        return;
    }

    uint64_t frameTicks = s_windowTicks[RenderPhase_Frame];
    double frameMsTotal = static_cast<double>(frameTicks) / ticksPerMs;
    double avgFrameMs = s_windowFrames ? frameMsTotal / s_windowFrames : 0.0;

    // Wall clock, not CPU-derived: the old line divided 1000 by CPU time and reported 215fps when
    // the client itself said 140. Anything outside the profiled pass shows up as the gap here.
    uint32_t windowMs = now - s_windowStartMs;
    double wallFps = windowMs ? 1000.0 * static_cast<double>(s_windowFrames) / static_cast<double>(windowMs) : 0.0;
    double wallMs = s_windowFrames ? static_cast<double>(windowMs) / static_cast<double>(s_windowFrames) : 0.0;
    double unmeasuredMs = wallMs - avgFrameMs;

    SLogWrite(log, "-- %u frames over %ums: wall %.3fms/frame (%.1f fps), measured %.3fms, unmeasured %.3fms (%.1f%%)", s_windowFrames, windowMs, wallMs, wallFps, avgFrameMs, unmeasuredMs, wallMs > 0.0 ? 100.0 * unmeasuredMs / wallMs : 0.0);

    if (s_windowFintOutOfRange) {
        SLogWrite(log, "   fint_ out of int32 range: %u/%u calls (fast path fell back to lrintf)", s_windowFintOutOfRange, s_windowFrames);
    }

    if (s_windowBatches) {
        double batchesPerFrame = static_cast<double>(s_windowBatches) / static_cast<double>(s_windowFrames);
        double setupMs = static_cast<double>(s_windowTicks[RenderPhase_M2Setup]) / ticksPerMs;
        double perBatchUs = s_windowBatches ? 1000.0 * setupMs / static_cast<double>(s_windowBatches) : 0.0;
        if (s_windowOpaqueBefore) {
            double before = static_cast<double>(s_windowOpaqueBefore) / static_cast<double>(s_windowFrames);
            double after = static_cast<double>(s_windowOpaqueAfter) / static_cast<double>(s_windowFrames);

            SLogWrite(log, "   opaque draws: %.1f -> %.1f per frame (%.0f%% fewer)", before, after, before > 0.0 ? 100.0 * (before - after) / before : 0.0);
        }
    }
    SLogWrite(log, "  phase          calls/frame   total ms   per-frame   in-frame %");

    // Phases are nested (frame wraps everything), so each is reported as a share of the frame
    // bucket, which is the only honest denominator available.
    for (int32_t i = 1; i < RenderPhase_Count; i++) {
        if (!s_windowTicks[i]) {
            continue;
        }

        double totalMs = static_cast<double>(s_windowTicks[i]) / ticksPerMs;
        double share = frameTicks ? 100.0 * static_cast<double>(s_windowTicks[i]) / static_cast<double>(frameTicks) : 0.0;

        SLogWrite(log, "  %-14s %10.2f %10.3f %10.4f %9.1f%%", s_phaseNames[i], static_cast<double>(s_windowCalls[i]) / static_cast<double>(s_windowFrames), totalMs, totalMs / static_cast<double>(s_windowFrames), share);
    }

    SLogWrite(log, "  %-14s %10.2f %10.3f %10.4f %9.1f%% (frame = measured total, not wall clock)", s_phaseNames[RenderPhase_Frame], static_cast<double>(s_windowCalls[RenderPhase_Frame]) / static_cast<double>(s_windowFrames), frameMsTotal, avgFrameMs, 100.0);

    // DIAG: which render group issues the draw calls. Sums to the Gx batch submissions per frame, so it
    // can be checked directly against the DXVK HUD's draw-call count.
    uint64_t totalDraws = 0;

    for (int32_t i = 0; i < RenderPhase_Count; i++) {
        totalDraws += s_windowDraws[i];
    }

    if (totalDraws) {
        SLogWrite(log, "  Gx draws/frame: %.1f total", static_cast<double>(totalDraws) / static_cast<double>(s_windowFrames));

        for (int32_t i = 0; i < RenderPhase_Count; i++) {
            if (!s_windowDraws[i]) {
                continue;
            }

            double draws = static_cast<double>(s_windowDraws[i]) / static_cast<double>(s_windowFrames);

            // Index 0 is RenderPhase_Frame, i.e. a draw issued outside any phase scope.
            const char* name = i ? s_phaseNames[i] : "(no scope)";
            SLogWrite(log, "    %-14s %8.1f/frame %6.1f%%", name, draws, 100.0 * static_cast<double>(s_windowDraws[i]) / static_cast<double>(totalDraws));
        }
    }

    SLogClose(log);

    memset(s_windowTicks, 0, sizeof(s_windowTicks));
    memset(s_windowCalls, 0, sizeof(s_windowCalls));
    memset(s_windowDraws, 0, sizeof(s_windowDraws));
    s_windowBatches = 0;
    s_windowOpaqueBefore = 0;
    s_windowOpaqueAfter = 0;
    s_windowFintOutOfRange = 0;
    s_windowFrames = 0;
    s_windowStartMs = now;
}