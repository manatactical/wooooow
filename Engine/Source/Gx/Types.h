#ifndef ENGINE_SOURCE_GX_TYPES_H
#define ENGINE_SOURCE_GX_TYPES_H

#include <cstddef>
#include <cstdint>
#include "Engine/Source/Gx/CGxDevice/Types.h"
#include "Engine/Source/Storm/H/stpl.h"

struct CGxBuf;
struct CGxPool;
class CGxQuery;

enum EGxApi {
    GxApi_OpenGl = 0,
    GxApi_D3d9 = 1,
    GxApi_D3d9Ex = 2,
    GxApi_D3d10 = 3,
    GxApi_D3d11 = 4,
    GxApi_GLL = 5,
    GxApis_Last = 6,
};

enum EGxBlend {
    GxBlend_Opaque = 0,
    GxBlend_AlphaKey = 1,
    GxBlend_Alpha = 2,
    GxBlend_Add = 3,
    GxBlend_Mod = 4,
    GxBlend_Mod2x = 5,
    GxBlend_ModAdd = 6,
    GxBlend_InvSrcAlphaAdd = 7,
    GxBlend_InvSrcAlphaOpaque = 8,
    GxBlend_SrcAlphaOpaque = 9,
    GxBlend_NoAlphaAdd = 10,
    GxBlend_ConstantAlpha = 11,
    GxBlends_Last = 12,
};

enum EGxTexFormat {
    GxTex_Unknown = 0,
    GxTex_Abgr8888 = 1,
    GxTex_Argb8888 = 2,
    GxTex_Argb4444 = 3,
    GxTex_Argb1555 = 4,
    GxTex_Rgb565 = 5,
    GxTex_Dxt1 = 6,
    GxTex_Dxt3 = 7,
    GxTex_Dxt5 = 8,
    GxTex_Uv88 = 9,
    GxTex_Gr1616F = 10,
    GxTex_R32F = 11,
    GxTex_D24X8 = 12,
    // Not in the original binary. Depth-stencil formats DepthCache needs to sample the depth buffer;
    // appended after the 12340 formats so the existing 13 values stay put, so these numbers differ from 15595.
    GxTex_Intz = 13,
    GxTex_Rawz = 14,
    GxTex_Resz = 15,
    GxTex_AtiD24 = 16,
    GxTex_D16 = 17,
    GxTex_D32 = 18,
    GxTex_R16 = 19,
    GxTex_R24X8 = 20,
    GxTexFormats_Last = 21,
};

enum EGxVertexBufferFormat {
    GxVBF_P = 0,
    GxVBF_PN = 1,
    GxVBF_PNC = 2,
    GxVBF_PNT = 3,
    GxVBF_PNCT = 4,
    GxVBF_PNT2 = 5,
    GxVBF_PNCT2 = 6,
    GxVBF_PC = 7,
    GxVBF_PCT = 8,
    GxVBF_PCT2 = 9,
    GxVBF_PT = 10,
    GxVBF_PT2 = 11,
    GxVBF_PBNT2 = 12,
    GxVBF_PNC2T2 = 13,
    GxVertexBufferFormats_Last = 14,
};

enum EGxBuffer {
    GxBuffers_Color = 0,
    GxBuffers_Depth = 1,
    GxBuffers_Last = 2,
};

enum EGxOverride {
    GxOverride_PixelShader = 0,
    GxOverride_Unk1 = 1,
    GxOverride_Unk2 = 2,
    GxOverride_Unk3 = 3,
    GxOverride_Unk4 = 4,
    GxOverride_Unk5 = 5,
    GxOverride_Unk6 = 6,
    GxOverride_Unk7 = 7,
    GxOverride_Unk8 = 8,
    GxOverrides_Last = 9,
};

enum EGxQueryParam {
    GxQueryParam_Unk0 = 0,
    GxQueryParam_Unk1 = 1,
    GxQueryParams_Last = 2,
};

enum EGxTexCommand {
    GxTex_Lock = 0,
    GxTex_Latch = 1,
    GxTex_Unlock = 2,
    GxTex_3 = 3,
    GxTexCommands_Last = 4,
};

enum EGxWM {
    GxWM_Size = 0,
    GxWM_DisplayChange = 1,
    GxWM_Destroy = 2,
    GxWM_SetFocus = 3,
    GxWM_KillFocus = 4,
};

enum EGxTexGen {
    GxTexGen_Disabled = 0,
    GxTexGen_ObjectLinear = 1,
    GxTexGen_EyeLinear = 2,
    GxTexGen_Unk3 = 3,
    GxTexGen_ReflectionMap = 4,
    GxTexGen_Normal = 5,
    GxTexGen_SphereMap = 6,
    GxTexGens_Last = 7,
};

enum EGxTexFilter {
    GxTex_Nearest = 0,
    GxTex_Linear = 1,
    GxTex_NearestMipNearest = 2,
    GxTex_LinearMipNearest = 3,
    GxTex_LinearMipLinear = 4,
    GxTex_Anisotropic = 5,
    GxTexFilters_Last = 6,
};

enum EGxTexWrapMode {
    GxTex_Clamp = 0,
    GxTex_Wrap = 1,
};

enum EGxXform {
    GxXform_Tex0 = 0,
    GxXform_Tex1 = 1,
    GxXform_Tex2 = 2,
    GxXform_Tex3 = 3,
    GxXform_Tex4 = 4,
    GxXform_Tex5 = 5,
    GxXform_Tex6 = 6,
    GxXform_Tex7 = 7,
    GxXform_World = 8,
    GxXform_Projection = 9,
    GxXform_View = 10,
    GxXforms_Last = 11,
};

enum BlitFormat {
    BlitFormat_Unknown = 0,
    BlitFormat_Abgr8888 = 1,
    BlitFormat_Argb8888 = 2,
    BlitFormat_Argb4444 = 3,
    BlitFormat_Argb1555 = 4,
    BlitFormat_Rgb565 = 5,
    BlitFormat_Dxt1 = 6,
    BlitFormat_Dxt3 = 7,
    BlitFormat_Dxt5 = 8,
    BlitFormat_Uv88 = 9,
    BlitFormat_Gr1616F = 10,
    BlitFormat_R32F = 11,
    BlitFormat_D24X8 = 12,
    BlitFormats_Last = 13,
};

class CGxBatch {
    public:
    /* 0000 */ EGxPrim m_primType;
    /* 0004 */ uint32_t m_start;
    /* 0008 */ uint32_t m_count;
    /* 000C */ uint16_t m_minIndex;
    /* 000E */ uint16_t m_maxIndex;
};

enum EGxColorFormat {
    GxCF_argb = 0,
    GxCF_rgba = 1,
    GxColorFormats_Last = 2,
};

// Not in the original binary. Ported from 15595 (Cataclysm). The indices matter: ChooseShaderModel fills
// CGxCaps::m_shaderProfiles with these, and ShaderCreate builds the .bls path from the matching name, so
// the numbering must match 15595's even though 12340 has no equivalent enum.
enum EGxShaderProfile {
    GxShProfile_None = 0,
    GxShProfile_VsFf = 1,
    GxShProfile_PsFf = 2,
    GxShProfile_Vs11 = 3,
    GxShProfile_Vs20 = 4,
    GxShProfile_Vs30 = 5,
    GxShProfile_Vs40 = 6,
    GxShProfile_Vs50 = 7,
    GxShProfile_Ps11 = 8,
    GxShProfile_Ps14 = 9,
    GxShProfile_Ps20 = 10,
    GxShProfile_Ps30 = 11,
    GxShProfile_Ps40 = 12,
    GxShProfile_Ps50 = 13,
    GxShProfile_Gs40 = 14,
    GxShProfile_Gs50 = 15,
    GxShProfile_Hs50 = 16,
    GxShProfile_Ds50 = 17,
    GxShProfile_Cs40 = 18,
    GxShProfile_Cs50 = 19,
    GxShProfile_Arbvp1 = 20,
    GxShProfile_Arbvp1Cg12 = 21,
    GxShProfile_Arbfp1 = 22,
    GxShProfile_Glslv = 23,
    GxShProfile_Glslf = 24,
    GxShProfile_Opencl = 25,
    GxShProfile_Vp20 = 26,
    GxShProfile_Vp30 = 27,
    GxShProfile_Vp40 = 28,
    GxShProfile_Gp4vp = 29,
    GxShProfile_Fp10 = 30,
    GxShProfile_Fp20 = 31,
    GxShProfile_Fp25 = 32,
    GxShProfile_Fp30 = 33,
    GxShProfile_Fp40 = 34,
    GxShProfile_Gp4fp = 35,
    GxShProfile_Gp4gp = 36,
    GxShProfile_Cuda = 37,
    GxShProfiles_Last = 38,
};

class CGxCaps {
    public:
    // Not in the original binary. Two rounds of growth versus 12340, both deliberate; the whole block is
    // re-anchored so no field reads a neighbour's storage.
    //
    // Round 1: the eight DepthCache depth formats were appended to EGxTexFormat, so m_texFmt/m_rttFormat
    // are [GxTexFormats_Last] and everything below them moved. Round 2: m_texTarget/m_texMaxSize grew from
    // 4 to GxTexTargets_Last for the volume-texture flag the water front-end gates on, and the 15595 caps
    // below m_numShaderTmus were appended.
    //
    // CGxDevice members after m_caps are re-anchored by the matching delta (see CGxDevice.h). Unlike 15595
    // the arrays stay indexed by this tree's EGxTexFormat ordering, so the 15595 slot numbers differ.
    /* 0000 */ int32_t m_numTmus;
    /* 0004 */ int32_t m_pixelCenterOnEdge;
    /* 0008 */ int32_t m_texelCenterOnEdge;
    /* 000C */ int32_t m_numStreams;
    /* 0010 */ int32_t m_streamOffset;
    /* 0014 */ EGxColorFormat m_colorFormat;
    /* 0018 */ int32_t unk_0018;
    /* 001C */ uint32_t m_maxIndex;
    /* 0020 */ int32_t m_generateMipMaps;
    /* 0024 */ int32_t m_texFmt [GxTexFormats_Last];
    /* 0078 */ int32_t m_texTarget [GxTexTargets_Last];
    /* 008C */ uint32_t m_texNonPow2;
    /* 0090 */ uint32_t m_texMaxSize [GxTexTargets_Last];
    /* 00A4 */ int32_t m_rttFormat [GxTexFormats_Last];
    /* 00F8 */ uint32_t unk_00B0;
    /* 00FC */ int32_t m_shaderTargets [6];
    /* 0114 */ uint32_t m_shaderConstants [6];
    /* 012C */ int32_t m_texFilterTrilinear;
    /* 0130 */ int32_t m_texFilterAnisotropic;
    /* 0134 */ uint32_t m_maxTexAnisotropy;
    /* 0138 */ int32_t m_depthBias;
    /* 013C */ int32_t m_colorWrite;
    /* 0140 */ int32_t m_maxClipPlanes;
    /* 0144 */ int32_t m_hwCursor;
    /* 0148 */ int32_t m_occlusionQuery;
    /* 014C */ int32_t m_pointParameters;
    /* 0150 */ float m_pointScaleMax;
    /* 0154 */ int32_t m_pointSprite;
    /* 0158 */ uint32_t m_blendFactor;
    /* 015C */ uint32_t unused_0114 [5];
    /* 0170 */ int32_t m_oglRtAlpha;
    /* 0174 */ int32_t m_stereoAvailable;
    /* 0178 */ int32_t m_shaderAlphaRef;
    /* 017C */ int32_t unk_0134;
    /* 0180 */ int32_t unk_0138;
    // Appended from 15595; everything above is 12340's layout shifted by the two growth rounds.
    /* 0184 */ int32_t m_numVertexTmus;
    /* 0188 */ int32_t m_numShaderTmus;
    /* 018C */ uint32_t m_vendorId;
    /* 0190 */ uint32_t m_deviceId;
    /* 0194 */ uint32_t m_numRenderTargets;
    /* 0198 */ int32_t m_shaderModel;
    /* 019C */ int32_t m_shaderProfiles [GxShProfiles_Last];
    /* 0234 */ int32_t m_vertexTexFmt [GxTexFormats_Last];
    /* 0288 */ int32_t m_pcfFormat [GxTexFormats_Last];
    // Not in the original binary. Ported from 15595 (unk_0300/unk_0314); DepthCache::IsSupported gates on
    // unk_0300 and DepthCache::CreateTextures gates on unk_0314. Both mean "can the depth buffer be read back
    // and resolved", which 15595 sets to m_d3dNVAPI || m_texFmt[GxTex_Resz].
    /* 02DC */ int32_t unk_0300;
    /* 02E0 */ int32_t unk_0314;
};

// The whole point of re-anchoring is that the layout is now load-bearing: CGxDevice's offsets below m_caps
// and every table indexed by GxTexFormats_Last assume these sizes. A silent drift here is what produced the
// m_d3dVertexDecl overrun that crashed the client on launch, so fail the build instead of the runtime.
static_assert(sizeof(CGxCaps) == 0x2E4, "CGxCaps layout changed - re-anchor the CGxDevice members below m_caps");

class CGxQuery {
    public:
    /* 0000 */ void* m_apiSpecific;
    /* 0004 */ EGxQueryType m_type;
    /* 0008 */ TSLink<CGxQuery> m_link;

    CGxQuery(EGxQueryType type);
};

// INLINED
inline CGxQuery::CGxQuery(EGxQueryType type) {
    this->m_apiSpecific = nullptr;
    this->m_type = type;
}

enum EGxPoolTarget {
    GxPoolTarget_Vertex = 0,
    GxPoolTarget_Index = 1,
    GxPoolTargets_Last = 2,
};

enum EGxPoolUsage {
    GxPoolUsage_Static = 0,
    GxPoolUsage_Dynamic = 1,
    GxPoolUsage_Stream = 2,
    GxPoolUsages_Last = 3,
};

enum EGxRenderState {
    GxRs_PolygonOffset = 0,
    GxRs_MatDiffuse = 1,
    GxRs_MatEmissive = 2,
    GxRs_MatSpecular = 3,
    GxRs_MatSpecularExp = 4,
    GxRs_NormalizeNormals = 5,
    GxRs_BlendingMode = 6,
    GxRs_AlphaRef = 7,
    GxRs_FogStart = 8,
    GxRs_FogEnd = 9,
    GxRs_FogColor = 10,
    GxRs_Lighting = 11,
    GxRs_Fog = 12,
    GxRs_DepthTest = 13,
    GxRs_DepthFunc = 14,
    GxRs_DepthWrite = 15,
    GxRs_ColorWrite = 16,
    GxRs_Culling = 17,
    GxRs_ClipPlaneMask = 18,
    GxRs_Multisample = 19,
    GxRs_ScissorTest = 20,
    GxRs_Texture0 = 21,
    GxRs_Texture1 = 22,
    GxRs_Texture2 = 23,
    GxRs_Texture3 = 24,
    GxRs_Texture4 = 25,
    GxRs_Texture5 = 26,
    GxRs_Texture6 = 27,
    GxRs_Texture7 = 28,
    GxRs_Texture8 = 29,
    GxRs_Texture9 = 30,
    GxRs_Texture10 = 31,
    GxRs_Texture11 = 32,
    GxRs_Texture12 = 33,
    GxRs_Texture13 = 34,
    GxRs_Texture14 = 35,
    GxRs_Texture15 = 36,
    GxRs_ColorOp0 = 37,
    GxRs_ColorOp1 = 38,
    GxRs_ColorOp2 = 39,
    GxRs_ColorOp3 = 40,
    GxRs_ColorOp4 = 41,
    GxRs_ColorOp5 = 42,
    GxRs_ColorOp6 = 43,
    GxRs_ColorOp7 = 44,
    GxRs_AlphaOp0 = 45,
    GxRs_AlphaOp1 = 46,
    GxRs_AlphaOp2 = 47,
    GxRs_AlphaOp3 = 48,
    GxRs_AlphaOp4 = 49,
    GxRs_AlphaOp5 = 50,
    GxRs_AlphaOp6 = 51,
    GxRs_AlphaOp7 = 52,
    GxRs_TexGen0 = 53,
    GxRs_TexGen1 = 54,
    GxRs_TexGen2 = 55,
    GxRs_TexGen3 = 56,
    GxRs_TexGen4 = 57,
    GxRs_TexGen5 = 58,
    GxRs_TexGen6 = 59,
    GxRs_TexGen7 = 60,
    GxRs_TextureShader0 = 61,
    GxRs_TextureShader1 = 62,
    GxRs_TextureShader2 = 63,
    GxRs_TextureShader3 = 64,
    GxRs_TextureShader4 = 65,
    GxRs_TextureShader5 = 66,
    GxRs_TextureShader6 = 67,
    GxRs_TextureShader7 = 68,
    GxRs_TextureCoord0 = 69,
    GxRs_TextureCoord1 = 70,
    GxRs_TextureCoord2 = 71,
    GxRs_TextureCoord3 = 72,
    GxRs_TextureCoord4 = 73,
    GxRs_TextureCoord5 = 74,
    GxRs_TextureCoord6 = 75,
    GxRs_TextureCoord7 = 76,
    GxRs_VertexShader = 77,
    GxRs_PixelShader = 78,
    GxRs_PointScale = 79,
    GxRs_PointScaleAttenuation = 80,
    GxRs_PointScaleMin = 81,
    GxRs_PointScaleMax = 82,
    GxRs_PointSprite = 83,
    Unk84 = 84,
    GxRs_ColorMaterial = 85,
    GxRenderStates_Last = 86,
};

enum EGxShTarget {
    GxSh_Vertex = 0,
    GxSh_Hull = 1,
    GxSh_Domain = 2,
    GxSh_Geometry = 3,
    GxSh_Pixel = 4,
    GxSh_Compute = 5,
    GxShTargets_Last = 6,
};

typedef intptr_t (*GxWindowProc)(void* window, uint32_t message, uintptr_t wparam, intptr_t lparam);

enum EGxuDrawListCategory {
    GxuCat_0 = 0,
    GxuCat_1 = 1,
    GxuCat_2 = 2,
    GxuCats_Last = 3,
};

enum EGxuDrawListSorting {
    GxuSort_B2F = 0,
    GxuSort_VtxStream = 1,
    GxuSorts_Last = 2,
};

#endif
