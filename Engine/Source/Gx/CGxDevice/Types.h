#ifndef ENGINE_SOURCE_GX_CGXDEVICE_TYPES_H
#define ENGINE_SOURCE_GX_CGXDEVICE_TYPES_H

#include <cstddef>
#include <cstdint>
#include "Engine/Source/Tempest/C2iVector.h"

enum EGxVertexAttrib {
    GxVA_Position = 0,
    GxVA_BlendWeight = 1,
    GxVA_BlendIndices = 2,
    GxVA_Normal = 3,
    GxVA_Color0 = 4,
    GxVA_Color1 = 5,
    GxVA_TexCoord0 = 6,
    GxVA_TexCoord1 = 7,
    GxVA_TexCoord2 = 8,
    GxVA_TexCoord3 = 9,
    GxVA_TexCoord4 = 10,
    GxVA_TexCoord5 = 11,
    GxVA_TexCoord6 = 12,
    GxVA_TexCoord7 = 13,
    GxVAs_Last = 14,
};

class CGxVertexAttrib {
    public:
    /* 0000 */ EGxVertexAttrib m_attrib;
    /* 0004 */ uint32_t m_type;
    /* 0008 */ uint32_t m_offset;
    /* 000C */ uint32_t m_bufSize;
    // NOT IN BINARY: which vertex stream this attribute comes from; cata's CGxVertexAttrib has it and the
    // D3d11 backend reads it to mark input elements as per-instance. Appended rather than inserted so the
    // 3.3.5a tables in Buffer.cpp keep their 4-value initialisers (the field stays 0 = single stream).
    /* 0010 */ uint32_t m_stream;
};

class CGxMonitorMode {
    public:
    /* 0000 */ C2iVector m_size;
    /* 0008 */ uint32_t m_bpp;
    /* 000C */ uint32_t m_refreshRate;
    // NOT IN BINARY: cata's mode description carries the DXGI refresh-rate rational and scanline/scaling
    // hints, which its D3d11 mode enumeration fills in. Appended, so the 3.3.5a fields above are unchanged.
    /* 0010 */ uint32_t m_refreshRateDenom;
    /* 0014 */ uint32_t m_scanlineOrdering;
    /* 0018 */ uint32_t m_scaling;

    CGxMonitorMode();
};

enum EGxMasterEnables {
    GxMasterEnable_Lighting = 0,
    GxMasterEnable_Fog = 1,
    GxMasterEnable_DepthTest = 2,
    GxMasterEnable_DepthWrite = 3,
    GxMasterEnable_ColorWrite = 4,
    GxMasterEnable_Culling = 5,
    GxMasterEnable_DoubleBuffering = 6,
    GxMasterEnable_NormalProjection = 7,
    GxMasterEnable_PolygonFill = 8,
    GxMasterEnables_Last = 9,
};

enum EGxPoolHintBits {
    GxPoolHintBit_Unk0 = 0,
    GxPoolHintBit_Unk1 = 1,
    GxPoolHintBit_Unk2 = 2,
    GxPoolHintBit_Unk3 = 3,
};

enum EGxPrim {
    GxPrim_Points = 0,
    GxPrim_Lines = 1,
    GxPrim_LineStrip = 2,
    GxPrim_Triangles = 3,
    GxPrim_TriangleStrip = 4,
    GxPrim_TriangleFan = 5,
    GxPrims_Last = 6,
};

enum EGxQueryType {
    GxQueryType_Occlusion = 0,
};

enum EGxTexTarget {
    GxTex_2d = 0,
    GxTex_CubeMap = 1,
    GxTex_Rectangle = 2,
    GxTex_NonPow2 = 3,
    // NOT IN BINARY: 3.3.5a has no volume textures, but the D3d11 backend is written against cata's
    // target list and its caps tables are indexed by feature level per target. Appended so the three
    // 3.3.5a values keep theirs. The two spare slots exist only because cata's ICreateTargets/ISetCaps
    // write m_texTarget[5] and [6]; the array must be sized to match or those writes run past the caps.
    GxTex_Volume = 4,
    GxTex_5 = 5,
    GxTex_6 = 6,
    GxTexTargets_Last = 7,
};

#endif
