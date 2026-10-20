#ifndef WOW_SOURCE_WORLDCLIENT_DETAILDOODAD_H
#define WOW_SOURCE_WORLDCLIENT_DETAILDOODAD_H

#include <cstddef>
#include <cstdint>
#include "Engine/Source/Gx/CGxTex.h"
#include "Engine/Source/Gx/CGxDevice/CGxDevice.h"
#include "Engine/Source/Storm/H/stpl.h"
#include "Engine/Source/Gx/CGxBuf.h"
#include "Engine/Source/Tempest/C3Vector.h"
#include "Engine/Source/Tempest/C44Matrix.h"
#include "Engine/Source/Tempest/C4Vector.h"
#include "Engine/Source/Tempest/CImVector.h"

struct CGxPool;
class CMapChunk;
class CGxShader;
class CDetailDoodadData;
class CDetailDoodadInst;
class CTexture;

class CDetailDoodad {
    public:
    struct SShaderConstants;

    static CGxTex* s_alphaRampTexture;
    static int32_t s_bPoolsDirty;
    static int32_t s_bUseShaders;
    static TSGrowableArray<CDetailDoodadData*> s_doodadList;
    static CGxPool* s_gxIndexPool;
    static CGxPool* s_gxVertexPool;
    static TSGrowableArray<CGxBuf*> s_freeGxBufs;
    static uint32_t s_idxBufItems;
    static uint32_t* s_instHeap;
    static CGxShader* s_pixelShader[8];
    static SShaderConstants s_shaderConstants;
    static CGxShader* s_vertexShader[6];
    static uint32_t s_vtxBufItems;

    static CDetailDoodadInst* AllocInst();
    static void CreateAlphaRampTexture(const void*& texels);
    static void Destroy();
    static void DestroyModels();
    static void FreeInst(CDetailDoodadInst* inst);
    static void FreeGxBufMat();
    static void Initialize();
    static int32_t InitializeRenderState();
    static void InitializeShaderConstants();
    static int32_t LoadModel(uint32_t id);
    static void SetupVertexShader(const C44Matrix& world, uint32_t index);
    static void UpdateAlphaRampTexture(EGxTexCommand command, uint32_t width, uint32_t height, uint32_t face, uint32_t level, void* userArg, uint32_t& texelStrideInBytes, const void*& texels);
    static void UpdatePools();
};

struct CDetailDoodad::SShaderConstants {
    struct SPointLight {
        /* 0000 */ C3Vector m_position;
        /* 000C */ float m_positionW;
        /* 0010 */ C3Vector m_color;
        /* 001C */ float m_colorW;
        /* 0020 */ C3Vector m_attenuation;
        /* 002C */ float m_attenuationW;
    };

    /* 0000 */ C44Matrix m_worldView;
    /* 0040 */ C44Matrix m_projection;
    /* 0080 */ C4Vector m_fog;
    /* 0090 */ C4Vector m_fade;
    /* 00A0 */ C4Vector m_sunDir;
    /* 00B0 */ C3Vector m_sunAmbient;
    /* 00BC */ float m_sunAmbientW;
    /* 00C0 */ C3Vector m_sunDiffuse;
    /* 00CC */ float m_sunDiffuseW;
    /* 00D0 */ C3Vector m_sunSpecular;
    /* 00DC */ float m_sunSpecularW;
    /* 00E0 */ SPointLight m_pointLights [3];
};

class CDetailDoodadInstAdd {
    public:
    /* 0000 */ uint16_t unused_0000;
    /* 0002 */ uint16_t m_triIndex;
    /* 0004 */ uint32_t m_doodadId;
    /* 0008 */ C3Vector m_position;
    /* 0014 */ float m_rotation;
    /* 0018 */ float m_scale;
    /* 001C */ C3Vector m_normal;
    /* 0028 */ CImVector m_color;

    CDetailDoodadInstAdd();
};

// Not in the original 3.3.5a binary. One doodad's contiguous run inside a batch: a batch is grouped by
// texture, but a texture can be shared by several doodad ids, and each id needs its own constant row.
class CDetailDoodadInstRun {
    public:
    /* 0000 */ uint32_t m_doodadId;
    /* 0004 */ uint32_t m_indexStart;
    /* 0008 */ uint32_t m_indexCount;
};

class CDetailDoodadInstBatch {
    public:
    /* 0000 */ CTexture* m_texture;
    /* 0004 */ uint32_t m_vertexCount;
    /* 0008 */ uint32_t m_indexCount;
    /* 000C */ CGxBuf* m_vertexBuf;
    /* 0010 */ CGxBuf* m_indexBuf;
    /* 0014 */ TSGrowableArray<CDetailDoodadInstAdd> m_adds;

    // Not in the original binary. The batch's own pool and buffers, rather than a unit out of the 256-entry
    // shared free list. A stream buffer is renamed on every lock, which resets m_hasData and forces a full
    // CPU re-bake of every plant in the batch on the next frame; at long grass distances the free list
    // empties and most of the world's grass takes that path. Owning them for the batch's own lifetime -- the
    // chunk's -- removes the re-bake and hands the 256 units back to the rare pop-in batch. See the plan's
    // section 3.1. m_bakeHash is how staleness is decided; nothing writes an add once the batch is drawn.
    CGxPool* m_vertexPool;
    CGxPool* m_indexPool;
    uint64_t m_bakeHash;
    uint64_t m_bakedHash;
    uint32_t m_bakedAdds;

    // Not in the original binary. The doodad runs the bake emitted, in draw order, each with the index
    // range it owns, so the caller can bind that doodad's constant row before its sub-draw.
    TSGrowableArray<CDetailDoodadInstRun> m_runs;

    CDetailDoodadInstBatch();
    ~CDetailDoodadInstBatch();
    void Draw();
    void FillIndices();
    void FillVertices();
    void Free();
    void FreeBuffers();

    uint32_t AddIndexCount(uint32_t doodadId);
    void BakeRuns();
    void BuildOwnBuffers();
    uint64_t HashAdds() const;
};

class CDetailDoodadInst {
    public:
    /* 0000 */ int32_t m_objectIndex;
    /* 0004 */ CDetailDoodadInstBatch m_batches[4];
    /* 0094 */ uint32_t m_age;
    /* 0098 */ CMapChunk* m_chunk;
    /* 009C */ TSLink<CDetailDoodadInst> m_link;

    CDetailDoodadInst();
    ~CDetailDoodadInst();
    void AddDoodad(uint32_t id, const C3Vector& position, float rotation, float scale, const C3Vector& normal, uint16_t triIndex, const CImVector& color);
    void DrawBatches();
    int32_t HasBatches();
    void FreeBuffers();
    void Free();
};

// INLINED
inline CDetailDoodadInstAdd::CDetailDoodadInstAdd() {
}

// INLINED
inline void CDetailDoodadInstBatch::FreeBuffers() {
    if (!this->m_vertexBuf) {
        this->m_bakeHash = 0;
        return;
    }

    if (this->m_vertexBuf->m_pool == CDetailDoodad::s_gxVertexPool) {
        // A unit out of the shared list: hand both halves back, in the order AcquireBuffers pops them.
        *CDetailDoodad::s_freeGxBufs.New() = this->m_vertexBuf;
        *CDetailDoodad::s_freeGxBufs.New() = this->m_indexBuf;
    } else {
        // Not in the original binary. Our own buffers are not in the free list, so the identity test above
        // cannot reclaim them: they have to be destroyed here or a chunk going away leaks them for the
        // session. Each batch has its own pool sized to its own counts, so the pool goes with the buffers --
        // which also keeps a density change from accumulating pools.
        if (this->m_indexBuf) {
            g_theGxDevicePtr->BufDestroy(this->m_indexBuf);
        }

        g_theGxDevicePtr->BufDestroy(this->m_vertexBuf);

        if (this->m_indexPool) {
            g_theGxDevicePtr->PoolDestroy(this->m_indexPool);
        }

        if (this->m_vertexPool) {
            g_theGxDevicePtr->PoolDestroy(this->m_vertexPool);
        }

        this->m_indexPool = nullptr;
        this->m_vertexPool = nullptr;
    }

    this->m_indexBuf = nullptr;
    this->m_vertexBuf = nullptr;
    this->m_bakeHash = 0;
}

// INLINED
inline void CDetailDoodadInstBatch::Free() {
    this->m_texture = nullptr;
    this->m_vertexCount = 0;
    this->m_indexCount = 0;
    this->m_adds.SetCount(0);
    this->m_adds.m_chunk = 64;
    this->m_runs.SetCount(0);
    this->m_bakedHash = 0;
    this->m_bakedAdds = 0;
    this->FreeBuffers();
}

// INLINED
inline CDetailDoodadInstBatch::~CDetailDoodadInstBatch() {
    this->Free();
}

#endif
