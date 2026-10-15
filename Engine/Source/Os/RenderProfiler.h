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

    // Inner: what the present phase actually spends its time on. m_fixLag is believed to spin on a D3D
    // event query (a full GPU sync) before Present, which would make it the whole cost.
    RenderPhase_PresentFlush,      // GxuFlushDrawList(GxuCat_2) in GxScenePresent, the caller of ScenePresent
    RenderPhase_PresentFixLag,     // the m_fixLag event-query spin loop
    RenderPhase_PresentSceneEnd,   // ISceneEnd
    RenderPhase_PresentCursor,     // ICursorDraw
    RenderPhase_PresentFpsCap,     // WaitForFPSCap
    RenderPhase_PresentCall,       // the D3D9 Present call itself, plus NvAPI stereo

    // Inner: CShadowQuery::Update. With extShadowQuality 5 (cascaded shadow maps) this is the single
    // largest named phase; these say whether the cost is the frustum setup, the caster query (which
    // walks the world and draws them) or the band pyramid.
    RenderPhase_ShadowPreUpdate,   // CShadowCache::PreUpdate, sun direction in view space
    RenderPhase_ShadowQuery,       // CShadowCache::Update, the whole thing
    RenderPhase_ShadowQueryCall,   // s_queryCallback -> CShadowQuery::QueryCallback, collects casters
    RenderPhase_ShadowRenderCall,  // s_renderCallback -> CShadowQuery::RenderCallback, draws the map
    RenderPhase_ShadowBands,       // GatherBands + RenderBands, the cascade pyramid
    RenderPhase_ShadowPostUpdate,  // CShadowCache::PostUpdate
    RenderPhase_WorldScene,     // CWorld::Render -> CWorldScene::Render, everything below it
    RenderPhase_M2Draw,         // the scene->Draw(M2PASS_*) passes
    RenderPhase_M2Setup,        // inner: the redundant per-batch state setup inside DrawBatch
    RenderPhase_M2Log,          // inner: the per-batch DebugLog, which sits above the setup scope
    RenderPhase_M2BoneUpload,   // inner: shader-constant lock + bone-matrix copy + unlock
    RenderPhase_M2SetState,     // inner: SetIndices + SetBatchVertices + SetShaders
    RenderPhase_M2DrawCall,     // inner: the g_theGxDevicePtr->Draw itself, all three branches
    RenderPhase_DrawLists,      // Gxu sort+flush, detail doodads
    RenderPhase_SpellVisuals,
    RenderPhase_Particulate,
    RenderPhase_Nameplates,
    RenderPhase_FfxFinish,

    // Inner: phases of CWorldScene::Render, shared of RenderPhase_WorldScene.
    RenderPhase_UpdatePools,
    RenderPhase_Occluders,
    RenderPhase_ViewerMapObj,   // s_viewerMapObjDef->RenderWithCallback, i.e. the interior you stand in
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

    // Inner: the parts of CWorldScene::Render that sit outside the named phases above. Without these the
    // draw-group report lumps them all into worldScene and the biggest bucket has no name.
    RenderPhase_GxuScene,        // CWorldScene::s_unk_B2EB68.Render()
    RenderPhase_CollisionDebug,

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

// DIAG: attributes each Gx draw submission to the innermost open scope. The Gx device is the single
// funnel every draw passes through, so tagging there splits the draw-call population by render group
// without editing every call site. Returns RenderPhase_Frame when no scope is open.
int32_t RenderProfilerCurrentPhase();
void RenderProfilerCountDraw(int32_t phase);

// Accumulates into the current frame, whose buckets were cleared by BeginFrame.
class RenderProfileScope {
    public:
        explicit RenderProfileScope(RenderProfilePhase phase);
        ~RenderProfileScope();

    private:
        RenderProfilePhase m_phase;
        uint64_t m_start;

        // To keep "innermost open scope" cheap and correct under nesting, each scope remembers the
        // phase its parent had, and restores it when it exits.
        int32_t m_savedPhase;
};

// Wraps the whole world render pass. EndFrame() may write the log, so it is rate limited.
// Keep BeginFrame/EndFrame as the FIRST and LAST statements executed in the enclosing frame
// function, so the interval check never depends on a scope's lifetime.
void RenderProfilerBeginFrame();
void RenderProfilerEndFrame();

void RenderProfilerSetEnabled(int32_t enabled);
int32_t RenderProfilerEnabled();

#endif