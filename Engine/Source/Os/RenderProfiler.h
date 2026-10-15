// Not in the original binary. Diagnostic only: a per-phase CPU timer for the world render pass.
//
// Purpose: the main thread sits at ~99% of one core, but nobody knows which phase spends it.
// Threading any phase is wasted work until this says which one is hot, and Amdahl bounds the gain.
//
// Overhead: two OsGetAsyncTimeClocks() calls per scope, and GetAsyncTimeClocks is a
// QueryPerformanceCounter on the calibrated timing method, i.e. tens of nanoseconds. With ~16
// phases that is under a microsecond per frame, so the numbers stay honest.
#ifndef ENGINE_SOURCE_OS_RENDERPROFILER_H
#define ENGINE_SOURCE_OS_RENDERPROFILER_H

#include <cstdint>

// DIAG: counts fint_ calls that fall outside int32 range, where the fast path falls back to lrintf.
// Should stay 0; if it does not, the fast path is not safe for that call site.
void RenderProfilerCountFintOutOfRange();

enum RenderProfilePhase {
    // Outer scope: the whole CGWorldFrame::OnWorldRender, the real per-frame budget.
    RenderPhase_Frame = 0,
    RenderPhase_WorldRender,   // CGWorldFrame::OnWorldRender, dispatched from the layer loop
    RenderPhase_UiLayers,      // the per-layer paint loop (contains worldRender)
    RenderPhase_GxuFont,
    RenderPhase_Present,
    RenderPhase_WorldScene,     // CWorld::Render -> CWorldScene::Render, everything below it
    RenderPhase_M2Draw,         // the scene->Draw(M2PASS_*) passes
    RenderPhase_M2Setup,        // inner: the redundant per-batch state setup inside DrawBatch
    RenderPhase_DrawLists,      // Gxu sort+flush, detail doodads
    RenderPhase_SpellVisuals,
    RenderPhase_Particulate,
    RenderPhase_Nameplates,
    RenderPhase_FfxFinish,

    // Inner: phases of CWorldScene::Render, shared of RenderPhase_WorldScene.
    RenderPhase_UpdatePools,
    RenderPhase_Occluders,
    RenderPhase_ViewerMapObj,
    RenderPhase_CullSort,
    RenderPhase_CullPortal,
    RenderPhase_CullMapObj,
    RenderPhase_SortedModels,
    RenderPhase_M2Animate,
    RenderPhase_Chunks,
    RenderPhase_MapObjGroups,
    RenderPhase_Horizon,
    RenderPhase_Portals,
    RenderPhase_Entities,
    RenderPhase_Liquid,

    // Inner: the six per-bucket culls inside CullSortTable's 64-iteration loop.
    RenderPhase_CullTblChunks,
    RenderPhase_CullTblMapObj,
    RenderPhase_CullTblLiquid,
    RenderPhase_CullTblEntitys,
    RenderPhase_CullTblDoodads,
    RenderPhase_CullTblOcclusion,

    // Inner: the three walks inside UpdateOcclusion, so we learn whether the cost is the scanline
    // clip-buffer scatter (ClipBufferUpdate, vectorizable) or occluder/volume iteration.
    RenderPhase_OcclHoledGuarded,   // walk 1: holed chunks with the depth-range guard
    RenderPhase_OcclHoledAll,       // walk 2: remaining holed chunks, ClipBufferUpdate unconditionally
    RenderPhase_OcclOccluders,      // walk 3: occluder ClipBufferUpdate + recycle/visible list
    RenderPhase_CullBufferUpdate,   // ClipBufferUpdate bodies only, from any caller

    // Inner: inside UpdateBufferChunk, the last fork. Projection is 12 mul-adds per vertex; the scatter
    // is up to 16 segments x up to 384 scanlines per chunk. Whichever dominates decides the fix.
    RenderPhase_OcclProj,           // the vertex projection loop
    RenderPhase_OcclScatter,        // the segment scanline scatter loops (holes and non-holes)
    RenderPhase_Count,
};

const char* RenderProfilerPhaseName(RenderProfilePhase phase);

// DIAG: times one ClipBufferUpdate body.
class RenderProfileClipBufferScope {
    public:
    RenderProfileClipBufferScope();
    ~RenderProfileClipBufferScope();

    private:
    uint64_t m_start;
};

// DIAG: times the two halves of UpdateBufferChunk, so projection can be separated from the scanline scatter.
class RenderProfileOcclusionSubScope {
    public:
    explicit RenderProfileOcclusionSubScope(uint64_t& accumulator);
    ~RenderProfileOcclusionSubScope();

    private:
    uint64_t& m_accumulator;
    uint64_t m_start;
};

// Accumulators the sub-scope writes into, owned by the profiler.
uint64_t& RenderProfilerOcclProjTicks();
uint64_t& RenderProfilerOcclScatterTicks();

// Counts CM2SceneRender::DrawBatch invocations, so per-batch overhead can be judged: a phase that
// costs 3ms split over 20 batches is a different problem from the same 3ms over 2000.
void RenderProfilerCountM2Batch();

// DIAG: opaque draw calls in the current pass, before and after the adjacent-run merge, so the win is
// measured directly rather than predicted.
void RenderProfilerCountOpaqueDrawCalls(uint32_t before, uint32_t after);

// Accumulates into the current frame, whose buckets were cleared by BeginFrame.
class RenderProfileScope {
    public:
    explicit RenderProfileScope(RenderProfilePhase phase);
    ~RenderProfileScope();

    private:
    RenderProfilePhase m_phase;
    uint64_t m_start;
};

// Wraps the whole world render pass. EndFrame() may write the log, so it is rate limited.
// Keep BeginFrame/EndFrame as the FIRST and LAST statements executed in the enclosing frame
// function, so the interval check never depends on a scope's lifetime.
void RenderProfilerBeginFrame();
void RenderProfilerEndFrame();

void RenderProfilerSetEnabled(int32_t enabled);
int32_t RenderProfilerEnabled();

#endif