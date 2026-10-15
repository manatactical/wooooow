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
    RenderPhase_Count,
};

const char* RenderProfilerPhaseName(RenderProfilePhase phase);

// Counts CM2SceneRender::DrawBatch invocations, so per-batch overhead can be judged: a phase that
// costs 3ms split over 20 batches is a different problem from the same 3ms over 2000.
void RenderProfilerCountM2Batch();

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