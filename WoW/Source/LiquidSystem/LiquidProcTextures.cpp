#include "Liquid.h"
#include "Engine/Source/Base/Handle.h"
#include "Engine/Source/Gx/CGxDevice/CGxDevice.h"
#include "Engine/Source/Gx/CGxTexFlags.h"
#include "Engine/Source/Gx/Gx.h"
#include "Engine/Source/Services/Texture.h"
#include "Engine/Source/ShaderEffect/DepthCache.h"
#include "Engine/Source/Tempest/CiRect.h"
#include "Engine/Source/Tempest/CMath.h"
#include "Engine/Source/Tempest/NTempest.h"
#include "WoW/Source/WorldClient/DayNight__DNInfo.h"

// Not in the original binary. Ported from 15595 (Cataclysm), Stage D.1. 12340's s_unk_D43B48/s_unk_D43B4C/
// s_unk_D43B50 were dead - declared, cached, and read, but written by nothing - so they are replaced by 15595's
// named state rather than kept alongside it.

// STATIC: 0x00E4E908/0x140D37BD0
uint8_t Liquid::s_wmoWaterTexValid;

// STATIC: 0x00E4E909/0x140D37BD1
uint8_t Liquid::s_riverDepthTexValid;

// STATIC: 0x00E4E90A/0x140D37BD2
uint8_t Liquid::s_oceanDepthTexValid;

// STATIC: 0x00E4E974/0x140D37C80
uint32_t Liquid::s_kawasePingPong;

// STATIC: 0x00E4E978/0x140D37C84
uint32_t Liquid::s_displMapCount;

// STATIC: 0x00E4E97C/0x140D37C88
CTexture* Liquid::s_displMaps[4];

// STATIC: 0x00E4E98C/0x140D37CA0
CGxFrameBuffer* Liquid::s_displMapFBs[4];

// STATIC: 0x00E4E99C/0x140D37CC8
uint8_t Liquid::s_displMapDirty;

// STATIC: 0x00E4E9A0/0x140D37CD0
CGxShader* Liquid::s_fourTapBlurVS;

// STATIC: 0x00E4E9A4/0x140D37CD8
CGxShader* Liquid::s_fourTapBlurPS;

// STATIC: 0x00E4E9A8/0x140D37CE0
CGxShader* Liquid::s_propagateDisplMapVS;

// STATIC: 0x00E4E9AC/0x140D37CE8
CGxShader* Liquid::s_propagateDisplMapPS;

// STATIC: 0x00E4E9B0/0x140D37CF0
CGxShader* Liquid::s_perturbDisplMapVS;

// STATIC: 0x00E4E9B4/0x140D37CF8
CGxShader* Liquid::s_perturbDisplMapPS;

// STATIC: 0x00E4E9B8/0x140D37D00
CGxShader* Liquid::s_perturbDisplMapMeshVS;

// STATIC: 0x00E4E9BC/0x140D37D08
CGxShader* Liquid::s_perturbDisplMapMeshPS;

// STATIC: 0x00E4E9C0/0x140D37D10
CGxShader* Liquid::s_clearDisplMapVS;

// STATIC: 0x00E4E9C4/0x140D37D18
CGxShader* Liquid::s_clearDisplMapPS;

// STATIC: 0x00E4E9C8/0x140D37D20
CTexture* Liquid::s_perturbTex;

// STATIC: 0x00E4EA48/0x140D37DC0
TSGrowableArray<Liquid::DisplMapPerturbation*> Liquid::s_eventPerturbations[2];

// STATIC: 0x00E4EA68/0x140D37DF0
TSGrowableArray<Liquid::DisplMapPerturbation*> Liquid::s_perturbations[2];

// STATIC: 0x00E4EA88/0x140D37E20
TSGrowableArray<Liquid::DisplMapPerturbationMesh*> Liquid::s_eventPerturbationMeshes[2];

// STATIC: 0x00E4EAA8/0x140D37E50
TSGrowableArray<Liquid::DisplMapPerturbationMesh*> Liquid::s_perturbationMeshes[2];

// STATIC: 0x00E4E92C/0x140D37BF8
static uint32_t s_displMapLastTime[2];

// STATIC: 0x00E4E924/0x140D37BF0
static float s_displMapInterpolation[2];

// STATIC: 0x00E4E91C/0x140D37BE8
static uint32_t s_displMapTexSize[2];

// STATIC: 0x00E4E914/0x140D37BE0
static uint32_t s_displMapPeriod[2];

// STATIC: 0x00E4E90C/0x140D37BD8
static uint32_t s_displMapCurrent[2];

// STATIC: 0x00E4E934/0x140D37C00
static CTexture* s_noiseVolTex;

// STATIC: 0x00E4E938/0x140D37C08
static CGxFrameBuffer* s_blurredReflectionFB;

// STATIC: 0x00E4E93C/0x140D37C10
static CGxFrameBuffer* s_blurredBackBufferFB;

// STATIC: 0x00E4E940/0x140D37C18
static CGxFrameBuffer* s_blurTempFB;

// STATIC: 0x00E4E944/0x140D37C28
static CTexture* s_blurredReflectionBuffer;

// STATIC: 0x00E4E948/0x140D37C28
static CTexture* s_blurredBackBuffer;

// STATIC: 0x00E4E94C/0x140D37C30
static CTexture* s_blurTemp;

// STATIC: 0x00E4E950/0x140D37C38
static CGxFrameBuffer* s_fakeReflectionFB;

// STATIC: 0x00E4E954/0x140D37C40
static CGxFrameBuffer* s_realReflectionFB;

// STATIC: 0x00E4E958/0x140D37C48
static CTexture* s_depthReflectionBuffer;

// STATIC: 0x00E4E95C/0x140D37C50
static CTexture* s_fakeReflectionBuffer;

// STATIC: 0x00E4E960/0x140D37C58
static CTexture* s_realReflectionBuffer;

// STATIC: 0x00E4E964/0x140D37C60
static CTexture* s_backBuffer;

// STATIC: 0x00E4E9CC/0x140D37D28
static C3Vector s_displMapPos[2];

// STATIC: 0x00E4E9E4/0x140D37D40
static C3Vector s_displMapRawPos[2];

// STATIC: 0x00E4E9FC/0x140D37D58
static C2Vector s_displMapSize[2];

// STATIC: 0x00BC705C
static const float s_displMapScale[2] = { 1.0f, 1.0f };

// STATIC: 0x00E4E99D/0x140D37CC9
static uint8_t s_perturbationsReserved;

// STATIC: 0x00E4EA1C/0x140D37D80
static CDataAllocator s_perturbationAllocator(sizeof(Liquid::DisplMapPerturbation), 64);

// STATIC: 0x00E4EA30/0x140D37DA0
static CDataAllocator s_perturbationMeshAllocator(sizeof(Liquid::DisplMapPerturbationMesh), 16);

// STATIC: 0x00E4E944/0x140D37C28
// 12340 address kept above; these two live here rather than in the old s_unk_D43B group.

// STATIC: 0x00D43BD0
CTexture* Liquid::s_wmoWaterTex;

// STATIC: 0x00D43BD4
CTexture* Liquid::s_oceanDepthTex;

// STATIC: 0x00D43BD8
CTexture* Liquid::s_riverDepthTex;

// VERIFY: 901024cc manual
// OFFSET: 0x008A2980
// OFFSET: 0x0071A770/0x1403EEC70
void Liquid::DestroyProceduralTextures(int32_t materials) {
    g_theGxDevicePtr->RemoveTextureRecreationCallback(Liquid::OnTexturesDirty);
    g_theGxDevicePtr->RemoveDeviceRestoredCallback(Liquid::OnTexturesDirty);
    Liquid::UnCacheProceduralTextures();

    if (s_realReflectionBuffer) {
        TextureRelease(s_realReflectionBuffer);
        s_realReflectionBuffer = nullptr;
    }

    if (s_fakeReflectionBuffer) {
        TextureRelease(s_fakeReflectionBuffer);
        s_fakeReflectionBuffer = nullptr;
    }

    if (s_depthReflectionBuffer) {
        TextureRelease(s_depthReflectionBuffer);
        s_depthReflectionBuffer = nullptr;
    }

    if (s_backBuffer) {
        TextureRelease(s_backBuffer);
        s_backBuffer = nullptr;
    }

    if (s_blurTemp) {
        TextureRelease(s_blurTemp);
        s_blurTemp = nullptr;
    }

    if (s_blurredBackBuffer) {
        TextureRelease(s_blurredBackBuffer);
        s_blurredBackBuffer = nullptr;
    }

    if (s_blurredReflectionBuffer) {
        TextureRelease(s_blurredReflectionBuffer);
        s_blurredReflectionBuffer = nullptr;
    }

    if (s_realReflectionFB) {
        g_theGxDevicePtr->FrameBufferDelete(s_realReflectionFB);
    }

    if (s_fakeReflectionFB) {
        g_theGxDevicePtr->FrameBufferDelete(s_fakeReflectionFB);
    }

    if (s_blurTempFB) {
        g_theGxDevicePtr->FrameBufferDelete(s_blurTempFB);
    }

    if (s_blurredBackBufferFB) {
        g_theGxDevicePtr->FrameBufferDelete(s_blurredBackBufferFB);
    }

    if (s_blurredReflectionFB) {
        g_theGxDevicePtr->FrameBufferDelete(s_blurredReflectionFB);
    }

    if (Liquid::s_fourTapBlurVS) {
        g_theGxDevicePtr->ShaderDestroy(Liquid::s_fourTapBlurVS);
        Liquid::s_fourTapBlurVS = nullptr;
    }

    if (Liquid::s_fourTapBlurPS) {
        g_theGxDevicePtr->ShaderDestroy(Liquid::s_fourTapBlurPS);
        Liquid::s_fourTapBlurPS = nullptr;
    }

    if (Liquid::s_clearDisplMapVS) {
        g_theGxDevicePtr->ShaderDestroy(Liquid::s_clearDisplMapVS);
        Liquid::s_clearDisplMapVS = nullptr;
    }

    if (Liquid::s_clearDisplMapPS) {
        g_theGxDevicePtr->ShaderDestroy(Liquid::s_clearDisplMapPS);
        Liquid::s_clearDisplMapPS = nullptr;
    }

    if (Liquid::s_perturbDisplMapVS) {
        g_theGxDevicePtr->ShaderDestroy(Liquid::s_perturbDisplMapVS);
        Liquid::s_perturbDisplMapVS = nullptr;
    }

    if (Liquid::s_perturbDisplMapPS) {
        g_theGxDevicePtr->ShaderDestroy(Liquid::s_perturbDisplMapPS);
        Liquid::s_perturbDisplMapPS = nullptr;
    }

    if (Liquid::s_perturbDisplMapMeshVS) {
        g_theGxDevicePtr->ShaderDestroy(Liquid::s_perturbDisplMapMeshVS);
        Liquid::s_perturbDisplMapMeshVS = nullptr;
    }

    if (Liquid::s_perturbDisplMapMeshPS) {
        g_theGxDevicePtr->ShaderDestroy(Liquid::s_perturbDisplMapMeshPS);
        Liquid::s_perturbDisplMapMeshPS = nullptr;
    }

    if (Liquid::s_propagateDisplMapVS) {
        g_theGxDevicePtr->ShaderDestroy(Liquid::s_propagateDisplMapVS);
        Liquid::s_propagateDisplMapVS = nullptr;
    }

    if (Liquid::s_propagateDisplMapPS) {
        g_theGxDevicePtr->ShaderDestroy(Liquid::s_propagateDisplMapPS);
        Liquid::s_propagateDisplMapPS = nullptr;
    }

    if (Liquid::s_perturbTex) {
        TextureRelease(Liquid::s_perturbTex);
        Liquid::s_perturbTex = nullptr;
    }

    if (materials) {
        if (s_riverDepthTex) {
            TextureRelease(s_riverDepthTex);
            s_riverDepthTex = nullptr;
        }

        if (s_oceanDepthTex) {
            TextureRelease(s_oceanDepthTex);
            s_oceanDepthTex = nullptr;
        }

        if (s_wmoWaterTex) {
            TextureRelease(s_wmoWaterTex);
            s_wmoWaterTex = nullptr;
        }
    }

    if (s_noiseVolTex) {
        TextureRelease(s_noiseVolTex);
        s_noiseVolTex = nullptr;
    }

    for (uint32_t i = 0; i < 2 * Liquid::s_displMapCount; i++) {
        if (Liquid::s_displMaps[i]) {
            TextureRelease(Liquid::s_displMaps[i]);
            Liquid::s_displMaps[i] = nullptr;
        }

        if (Liquid::s_displMapFBs[i]) {
            g_theGxDevicePtr->FrameBufferDelete(Liquid::s_displMapFBs[i]);
        }
    }

    Liquid::s_displMapCount = 0;
    Liquid::ClearEventPerturbations();

    // DIAG: CreateProceduralTextures logs what it built; without this the teardown half is invisible, so a
    // double-create (leaked framebuffers/shaders) looks identical to a balanced destroy-and-recreate in gx.log.
    // Counts are logged after the fact because both TextureRelease and FrameBufferDelete null through their
    // reference; anything still non-NULL here is a leak.
    g_theGxDevicePtr->Log("liquid proc tex: destroyed (materials=%d); tex back=%d blurBack=%d fake=%d real=%d blurReal=%d depth=%d displMap=%d, fb fake=%d real=%d blurReal=%d blurBack=%d blurTemp=%d\n",
        materials,
        s_backBuffer != nullptr, s_blurredBackBuffer != nullptr, s_fakeReflectionBuffer != nullptr,
        s_realReflectionBuffer != nullptr, s_blurredReflectionBuffer != nullptr, s_depthReflectionBuffer != nullptr,
        Liquid::s_displMaps[0] != nullptr,
        s_fakeReflectionFB != nullptr, s_realReflectionFB != nullptr, s_blurredReflectionFB != nullptr,
        s_blurredBackBufferFB != nullptr, s_blurTempFB != nullptr);
}

// OFFSET: 0x0071AF30/0x1403EF3A0
// 12340's version cached its (never-created) s_unk_D43B50/s_unk_D43B4C arrays. Those are gone, so this now
// mirrors 15595's: the three procedural depth textures plus the noise volume and displacement maps.
void Liquid::CacheProceduralTextures() {
    CGxTexFlags flags;

    TextureCacheNewTexture(s_riverDepthTex);
    TextureCacheNewTexture(s_oceanDepthTex);
    TextureCacheNewTexture(s_wmoWaterTex);

    if (s_noiseVolTex) {
        TextureCacheNewTexture(s_noiseVolTex);
    }

    for (uint32_t i = 0; i < 2 * Liquid::s_displMapCount; i++) {
        if (Liquid::s_displMaps[i]) {
            TextureCacheNewTexture(Liquid::s_displMaps[i]);
        }
    }
}

// OFFSET: 0x00717470/0x1403EA9F0
void Liquid::UnCacheProceduralTextures() {
    if (s_riverDepthTex) {
        TextureCacheRemoveTexture(s_riverDepthTex);
    }

    if (s_oceanDepthTex) {
        TextureCacheRemoveTexture(s_oceanDepthTex);
    }

    if (s_wmoWaterTex) {
        TextureCacheRemoveTexture(s_wmoWaterTex);
    }

    if (s_noiseVolTex) {
        TextureCacheRemoveTexture(s_noiseVolTex);
    }

    for (uint32_t i = 0; i < 2 * Liquid::s_displMapCount; i++) {
        if (Liquid::s_displMaps[i]) {
            TextureCacheRemoveTexture(Liquid::s_displMaps[i]);
        }
    }
}

// OFFSET: 0x007173E0
void Liquid::OnTexturesDirty() {
    Liquid::s_displMapDirty = 1;
    Liquid::SignalConfigChanged(1);
}

// OFFSET: 0x00717770/0x1403EAD90
static CTexture* CreateBackBuffer() {
    // 12340's CRect has no constructor, only the four public floats (minY, minX, maxY, maxX).
    CRect rect = { 0.0f, 0.0f, 0.0f, 0.0f };
    g_theGxDevicePtr->CapsWindowSize(rect);

    CGxTexFlags flags;
    flags.m_renderTarget = 1;
    return TextureCreate(GxTex_NonPow2, static_cast<uint32_t>(rect.maxX), static_cast<uint32_t>(rect.maxY), 1, GxTex_Argb8888, GxTex_Argb8888, flags, nullptr, GxTexNullUserFunc, "LiquidProc BackBufferTex", 0);
}

// OFFSET: 0x00717820/0x1403EAE40
static CTexture* CreateReflectionBuffer(const char* name) {
    CRect rect = { 0.0f, 0.0f, 0.0f, 0.0f };
    g_theGxDevicePtr->CapsWindowSize(rect);

    CGxTexFlags flags;
    flags.m_renderTarget = 1;
    uint32_t downscale = Liquid::GetReflectionDownscale();
    uint32_t width = static_cast<uint32_t>(rect.maxX) / (1 << downscale);
    uint32_t height = static_cast<uint32_t>(rect.maxY) / (1 << downscale);
    return TextureCreate(GxTex_NonPow2, width, height, 1, GxTex_Argb8888, GxTex_Argb8888, flags, nullptr, GxTexNullUserFunc, name, 0);
}

// OFFSET: 0x007178E0/0x1403EAF10
static CTexture* CreateDepthReflectionBuffer(const char* name) {
    CRect rect = { 0.0f, 0.0f, 0.0f, 0.0f };
    g_theGxDevicePtr->CapsWindowSize(rect);

    CGxTexFlags flags;
    flags.m_renderTarget = 1;
    uint32_t scale = 1 << Liquid::GetReflectionDownscale();
    EGxTexFormat format = g_theGxDevicePtr->DeviceApi() != GxApi_D3d9 ? GxTex_D32 : GxTex_D24X8;

    if (g_theGxDevicePtr->DeviceApi() == GxApi_D3d10) {
        format = GxTex_D16;
    }

    uint32_t width = static_cast<uint32_t>(rect.maxX) / scale;
    uint32_t height = static_cast<uint32_t>(rect.maxY) / scale;
    return TextureCreate(GxTex_NonPow2, width, height, 1, format, format, flags, nullptr, GxTexNullUserFunc, name, 0);
}

// OFFSET: 0x00717320
static CTexture* CreateBlurBuffer(const char* name) {
    CGxTexFlags flags;
    flags.m_renderTarget = 1;
    return TextureCreate(512, 512, GxTex_Argb8888, GxTex_Argb8888, flags, nullptr, GxTexNullUserFunc, name, 0);
}

// OFFSET: 0x00718100/0x1403EBBC0
static CTexture* CreateNoiseVolTex() {
    CGxTexFlags flags;
    flags.m_wrapU = 1;
    flags.m_wrapV = 1;
    // 15595 sets wrapW here too; 12340's CGxTexFlags now has the field (carved from unused m_unk16).
    flags.m_wrapW = 1;

    TSGrowableArray<uint32_t> noise;
    noise.SetCount(0x40000);

    SFileImpl* file = nullptr;

    if (SFileOpenFile("XTextures\\FX\\noise.blob", &file)) {
        SFileReadFile(file, noise.m_data, 4 * noise.Count(), nullptr, nullptr, 0);
        SFileCloseFile(file);
    }

    CTexture* texture = TextureCreate(GxTex_Volume, 128, 128, 16, GxTex_Argb8888, GxTex_Argb8888, flags, noise.m_data, GxTexNullUserFunc, "proceduralNoiseVolTex", 0);
    GxTexUpdate(TextureGetGxTex(texture, 0, nullptr), 0, 0, 0, 0, 1);
    return texture;
}

// OFFSET: 0x0071A6C0/0x1403EEAD0
void Liquid::CreateDynamicDisplacementMap() {
    Liquid::s_displMapCount = 1;
    s_displMapTexSize[0] = 1024;
    s_displMapPeriod[0] = 50;

    if (Liquid::GetRippleDetail() > 1) {
        Liquid::s_displMapCount = 2;
        s_displMapTexSize[0] = 1024;
        s_displMapTexSize[1] = 1024;
        s_displMapPeriod[0] = 25;
        s_displMapPeriod[1] = 50;
    }

    CGxTexFlags flags;
    flags.m_renderTarget = 1;

    for (uint32_t i = 0; i < 2 * Liquid::s_displMapCount; i++) {
        char name[256];
        SStrPrintf(name, sizeof(name), "proceduralDisplMap%d", i);
        Liquid::s_displMaps[i] = TextureCreate(s_displMapTexSize[i >> 1], s_displMapTexSize[i >> 1], GxTex_Gr1616F, GxTex_Gr1616F, flags, nullptr, GxTexNullUserFunc, name, 0);
        Liquid::s_displMapFBs[i] = GxFrameBufferCreate(nullptr, TextureGetGxTex(Liquid::s_displMaps[i], 0, nullptr));
    }

    g_theGxDevicePtr->ShaderCreate(&Liquid::s_propagateDisplMapVS, GxSh_Vertex, "Shaders\\Vertex", "PropagateDisplMap", 1);
    g_theGxDevicePtr->ShaderCreate(&Liquid::s_propagateDisplMapPS, GxSh_Pixel, "Shaders\\Pixel", "PropagateDisplMap", 1);
    g_theGxDevicePtr->ShaderCreate(&Liquid::s_perturbDisplMapVS, GxSh_Vertex, "Shaders\\Vertex", "PerturbDisplMap", 1);
    g_theGxDevicePtr->ShaderCreate(&Liquid::s_perturbDisplMapPS, GxSh_Pixel, "Shaders\\Pixel", "PerturbDisplMap", 1);
    g_theGxDevicePtr->ShaderCreate(&Liquid::s_perturbDisplMapMeshVS, GxSh_Vertex, "Shaders\\Vertex", "PerturbDisplMapMesh", 1);
    g_theGxDevicePtr->ShaderCreate(&Liquid::s_perturbDisplMapMeshPS, GxSh_Pixel, "Shaders\\Pixel", "PerturbDisplMapMesh", 1);
    g_theGxDevicePtr->ShaderCreate(&Liquid::s_clearDisplMapVS, GxSh_Vertex, "Shaders\\Vertex", "ClearDisplMap", 1);
    g_theGxDevicePtr->ShaderCreate(&Liquid::s_clearDisplMapPS, GxSh_Pixel, "Shaders\\Pixel", "ClearDisplMap", 1);

    CStatus status;
    CGxTexFlags texFlags;
    texFlags.m_filter = GxTex_LinearMipNearest;
    Liquid::s_perturbTex = TextureCreate("XTextures\\Ripples\\perturbTex", texFlags, &status, 0);
    Liquid::s_displMapDirty = 1;
}

// OFFSET: 0x0071A8F0/0x1403EEDF0
void Liquid::CreateProceduralTextures(int32_t materials) {
    int32_t backBuffer = Liquid::GetWaterDetail() != 0;
    int32_t blur = Liquid::GetWaterDetail() != 0;
    int32_t noise = Liquid::GetWaterDetail() != 0;
    int32_t dynDisplMap = Liquid::GetWaterDetail() > 1 && Liquid::GetRippleDetail();
    Liquid::s_oceanDepthTexValid = 0;
    Liquid::s_riverDepthTexValid = 0;
    Liquid::s_wmoWaterTexValid = 0;

    if (materials) {
        CGxTexFlags riverFlags;
        riverFlags.m_bit20 = 1;
        s_riverDepthTex = TextureCreate(8, 64, GxTex_Argb8888, GxTex_Argb8888, riverFlags, reinterpret_cast<void*>(1), ExtWaterDepthTexCallback, "proceduralRiverDepthTex", 0);

        CGxTexFlags oceanFlags;
        oceanFlags.m_bit20 = 1;
        s_oceanDepthTex = TextureCreate(8, 64, GxTex_Argb8888, GxTex_Argb8888, oceanFlags, nullptr, ExtWaterDepthTexCallback, "proceduralOceanDepthTex", 0);

        CGxTexFlags wmoFlags;
        wmoFlags.m_bit20 = 1;
        s_wmoWaterTex = TextureCreate(8, 64, GxTex_Argb8888, GxTex_Argb8888, wmoFlags, nullptr, WmoWaterTexCallback, "proceduralWmoWaterTex", 0);
    }

    if (noise) {
        s_noiseVolTex = CreateNoiseVolTex();
    }

    if (backBuffer) {
        s_backBuffer = CreateBackBuffer();
    }

    if (blur) {
        g_theGxDevicePtr->ShaderCreate(&Liquid::s_fourTapBlurVS, GxSh_Vertex, "Shaders\\Vertex", "FourTapBlur", 1);
        g_theGxDevicePtr->ShaderCreate(&Liquid::s_fourTapBlurPS, GxSh_Pixel, "Shaders\\Pixel", "FourTapBlur", 1);

        s_blurTemp = CreateBlurBuffer("blurTemp");
        s_blurTempFB = GxFrameBufferCreate(nullptr, TextureGetGxTex(s_blurTemp, 0, nullptr));

        s_blurredBackBuffer = CreateBlurBuffer("blurredBack");
        s_blurredBackBufferFB = GxFrameBufferCreate(nullptr, TextureGetGxTex(s_blurredBackBuffer, 0, nullptr));

        if (g_theGxDevicePtr->DeviceApi() == GxApi_D3d10 || g_theGxDevicePtr->DeviceFormat().m_multisampleCount > 1) {
            s_depthReflectionBuffer = CreateDepthReflectionBuffer("LiquidProc Reflection Depth");
        }

        CGxTex* depth = s_depthReflectionBuffer ? TextureGetGxTex(s_depthReflectionBuffer, 0, nullptr) : g_theGxDevicePtr->BackBufferDepthTex();

        if (Liquid::GetWaterDetail() >= 3) {
            s_realReflectionBuffer = CreateReflectionBuffer("LiquidProc Real Reflection");
            s_realReflectionFB = GxFrameBufferCreate(depth, TextureGetGxTex(s_realReflectionBuffer, 0, nullptr));
            s_blurredReflectionBuffer = CreateBlurBuffer("blurredReflection");
            s_blurredReflectionFB = GxFrameBufferCreate(nullptr, TextureGetGxTex(s_blurredReflectionBuffer, 0, nullptr));
        }

        s_fakeReflectionBuffer = CreateReflectionBuffer("LiquidProc Fake Reflection");
        s_fakeReflectionFB = GxFrameBufferCreate(depth, TextureGetGxTex(s_fakeReflectionBuffer, 0, nullptr));
    }

    if (dynDisplMap) {
        Liquid::CreateDynamicDisplacementMap();
        s_displMapCurrent[0] = 0;
        s_displMapCurrent[1] = 0;

        // Reserve the perturbator arrays up front so the per-frame simulation (Stage D.2) never grows them mid
        // frame. 15595 does this once, guarded by s_perturbationsReserved.
        if (!s_perturbationsReserved) {
            if (Liquid::s_eventPerturbations[0].Count() + 96 > Liquid::s_eventPerturbations[0].m_alloc) {
                Liquid::s_eventPerturbations[0].Reserve(Liquid::s_eventPerturbations[0].Count() + 96, 0);
            }

            if (Liquid::s_perturbations[0].Count() + 64 > Liquid::s_perturbations[0].m_alloc) {
                Liquid::s_perturbations[0].Reserve(Liquid::s_perturbations[0].Count() + 64, 0);
            }

            if (Liquid::s_eventPerturbationMeshes[0].Count() > Liquid::s_eventPerturbationMeshes[0].m_alloc) {
                Liquid::s_eventPerturbationMeshes[0].Reserve(Liquid::s_eventPerturbationMeshes[0].Count(), 0);
            }

            if (Liquid::s_perturbationMeshes[0].Count() > Liquid::s_perturbationMeshes[0].m_alloc) {
                Liquid::s_perturbationMeshes[0].Reserve(Liquid::s_perturbationMeshes[0].Count(), 0);
            }

            if (Liquid::s_eventPerturbations[1].Count() > Liquid::s_eventPerturbations[1].m_alloc) {
                Liquid::s_eventPerturbations[1].Reserve(Liquid::s_eventPerturbations[1].Count(), 0);
            }

            if (Liquid::s_perturbations[1].Count() > Liquid::s_perturbations[1].m_alloc) {
                Liquid::s_perturbations[1].Reserve(Liquid::s_perturbations[1].Count(), 0);
            }

            if (Liquid::s_eventPerturbationMeshes[1].Count() > Liquid::s_eventPerturbationMeshes[1].m_alloc) {
                Liquid::s_eventPerturbationMeshes[1].Reserve(Liquid::s_eventPerturbationMeshes[1].Count(), 0);
            }

            if (Liquid::s_perturbationMeshes[1].Count() + 16 > Liquid::s_perturbationMeshes[1].m_alloc) {
                Liquid::s_perturbationMeshes[1].Reserve(Liquid::s_perturbationMeshes[1].Count() + 16, 0);
            }

            s_perturbationsReserved = 1;
        }
    }

    Liquid::CacheProceduralTextures();
    g_theGxDevicePtr->AddTextureRecreationCallback(Liquid::OnTexturesDirty);
    g_theGxDevicePtr->AddDeviceRestoredCallback(Liquid::OnTexturesDirty);

    // DIAG: the buffer set above is the whole point of this stage, and every branch that drops one is a silent
    // no-op - a NULL reflection buffer just renders the old path. Log the detail level, which tiers were asked
    // for, and which textures/framebuffers actually came back, so a live run says whether D.1 built anything.
    g_theGxDevicePtr->Log("liquid proc tex: detail=%d, materials=%d, back=%d blur=%d noise=%d displMap=%d; tex back=%d blurBack=%d fake=%d real=%d blurReal=%d depth=%d displMap=%d, fb fake=%d real=%d blurReal=%d blurBack=%d blurTemp=%d displMap=%d, count=%u\n",
        Liquid::GetWaterDetail(), materials, backBuffer, blur, noise, dynDisplMap,
        s_backBuffer != nullptr, s_blurredBackBuffer != nullptr, s_fakeReflectionBuffer != nullptr,
        s_realReflectionBuffer != nullptr, s_blurredReflectionBuffer != nullptr, s_depthReflectionBuffer != nullptr,
        Liquid::s_displMaps[0] != nullptr,
        s_fakeReflectionFB != nullptr, s_realReflectionFB != nullptr, s_blurredReflectionFB != nullptr,
        s_blurredBackBufferFB != nullptr, s_blurTempFB != nullptr, Liquid::s_displMapFBs[0] != nullptr,
        Liquid::s_displMapCount);
}

// OFFSET: 0x0071B2A0/0x1403EF800
// NOTE: lives in Liquid.cpp, not here, because s_configDirty / s_materialsDirty / s_needBackBuffer /
// s_needDynDisplMap / s_configChangedCallbacks are file-scope statics of Liquid.cpp.


// OFFSET: 0x00717520/0x1403EAAA0
void Liquid::GrabBackBuffer() {
    if (!Liquid::NeedBackBuffer() || !s_backBuffer) {
        return;
    }

    CGxTex* tex = TextureGetGxTex(s_backBuffer, 0, nullptr);

    if (tex) {
        // 12340's GxTexStretch(dest, destRect, sourceRect, level, face) stretches the current back buffer into
        // dest; 15595 passes the rects and level/face as NULL/0.
        GxTexStretch(tex, nullptr, nullptr, 0, 0);
    }
}

// OFFSET: 0x00717AB0/0x1403EB110
void Liquid::SetDisplMapInfo(const C3Vector& pos, const C2Vector& size) {
    for (uint32_t i = 0; i < Liquid::s_displMapCount; i++) {
        float texSize = static_cast<float>(s_displMapTexSize[i]);
        float width = size.x * s_displMapScale[i];
        float height = s_displMapScale[i] * size.y;

        s_displMapPos[i].x = pos.x - width * 0.5f;
        s_displMapPos[i].y = pos.y - height * 0.5f;
        s_displMapPos[i].z = pos.z;
        s_displMapSize[i].x = width;
        s_displMapSize[i].y = height;

        s_displMapPos[i].x = floorf(s_displMapPos[i].x * (texSize / width)) / (texSize / width);
        s_displMapPos[i].y = floorf(s_displMapPos[i].y * (texSize / height)) / (texSize / height);
    }
}

// OFFSET: 0x007175F0/0x1403EAB70
// 15595 sets s_displMapRawPos in the per-frame perturbator update (the interpolation source), not here, so this
// only reads it.
void Liquid::GetDisplMapInfoRaw(uint32_t index, C3Vector& pos, C2Vector& size) {
    pos = s_displMapRawPos[index];
    size = s_displMapSize[index];
}

// OFFSET: 0x00717BD0/0x1403EB2A0
void Liquid::ClearPerturbatorCache() {
    s_perturbationAllocator.Clear(typeid(Liquid::DisplMapPerturbation).raw_name(), SLOG_OBJECT, 1);
    s_perturbationMeshAllocator.Clear(typeid(Liquid::DisplMapPerturbationMesh).raw_name(), SLOG_OBJECT, 1);
}

// OFFSET: 0x00718BB0/0x1403EC940
// Faithful to 15595, including the loop bound: it walks the per-surface arrays up to s_displMapCount, which is
// the ping-pong map count (1 or 2), not the always-2 surface count. So with rippleDetail 1 only surface 0 is
// cleared. 15595 does the same, and its only live caller is UpdateDisplMap (Stage D.2). In this tree the arrays
// are empty until the perturbator path exists, so the bound is not yet observable.
// NOTE: DestroyProceduralTextures zeroes s_displMapCount immediately before calling this (as 15595 does), so the
// teardown call is a no-op in 15595 too.
void Liquid::ClearEventPerturbations() {
    for (uint32_t i = 0; i < Liquid::s_displMapCount; i++) {
        for (uint32_t j = 0; j < Liquid::s_eventPerturbations[i].Count(); j++) {
            s_perturbationAllocator.PutData(Liquid::s_eventPerturbations[i].m_data[j], nullptr, 0);
        }

        for (uint32_t j = 0; j < Liquid::s_eventPerturbationMeshes[i].Count(); j++) {
            DisplMapPerturbationMesh* mesh = Liquid::s_eventPerturbationMeshes[i].m_data[j];

            if (mesh->m_indices.m_data) {
                SMemFree(mesh->m_indices.m_data, typeid(uint16_t).raw_name(), SLOG_OBJECT, 0);
            }

            if (mesh->m_vertices.m_data) {
                SMemFree(mesh->m_vertices.m_data, typeid(C3Vector).raw_name(), SLOG_OBJECT, 0);
            }

            s_perturbationMeshAllocator.PutData(mesh, nullptr, 0);
        }

        if (Liquid::s_eventPerturbations[i].m_data) {
            SMemFree(Liquid::s_eventPerturbations[i].m_data, typeid(DisplMapPerturbation*).raw_name(), SLOG_OBJECT, 0);
        }

        Liquid::s_eventPerturbations[i].m_alloc = 0;
        Liquid::s_eventPerturbations[i].m_count = 0;
        Liquid::s_eventPerturbations[i].m_data = nullptr;

        if (Liquid::s_eventPerturbationMeshes[i].m_data) {
            SMemFree(Liquid::s_eventPerturbationMeshes[i].m_data, typeid(DisplMapPerturbationMesh*).raw_name(), SLOG_OBJECT, 0);
        }

        Liquid::s_eventPerturbationMeshes[i].m_alloc = 0;
        Liquid::s_eventPerturbationMeshes[i].m_count = 0;
        Liquid::s_eventPerturbationMeshes[i].m_data = nullptr;
    }
}

// OFFSET: 0x00717360/0x1403EA8D0
int32_t Liquid::HasDynamicDisplMap() {
    if ((Liquid::s_displMapCount > 1 && (!Liquid::s_displMaps[2] || !Liquid::s_displMaps[3])) || !Liquid::s_displMaps[0] || !Liquid::s_displMaps[1]) {
        return 0;
    }

    return 1;
}

// OFFSET: 0x007173A0/0x1403EA910
uint32_t Liquid::GetDynamicDisplMapCnt() {
    return Liquid::s_displMapCount;
}

// OFFSET: 0x007173B0/0x1403EA920
CGxTex* Liquid::GetDynamicDisplMap(uint32_t index) {
    return TextureGetGxTex(Liquid::s_displMaps[s_displMapCurrent[index % 2]], 0, nullptr);
}

// OFFSET: 0x007173D0/0x1403EA940
float Liquid::GetDynamicDisplMapInterpolation(uint32_t index) {
    return s_displMapInterpolation[index % 2];
}

// OFFSET: 0x00717C10/0x1403EB300
C2iVector Liquid::GetDisplMapTexSize(uint32_t index) {
    C2iVector size;
    size.x = s_displMapTexSize[index];
    size.y = s_displMapTexSize[index];
    return size;
}

// OFFSET: 0x00717560/0x1403EAAE0
CTexture* Liquid::GetBackBuffer() {
    return s_backBuffer;
}

// OFFSET: 0x00717570/0x1403EAAF0
CTexture* Liquid::GetBlurredBackBuffer() {
    return s_blurredBackBuffer;
}

// OFFSET: 0x00717590/0x1403EAB10
CTexture* Liquid::GetRealReflectionBuffer() {
    return s_realReflectionBuffer;
}

// OFFSET: 0x007175A0/0x1403EAB20
CTexture* Liquid::GetBlurredReflectionBuffer() {
    return s_blurredReflectionBuffer;
}

// OFFSET: 0x007175C0/0x1403EAB40
CTexture* Liquid::GetFakeReflectionBuffer() {
    return s_fakeReflectionBuffer;
}

// OFFSET: 0x00717580/0x1403EAB00
CGxFrameBuffer* Liquid::GetBlurredBackBufferFB() {
    return s_blurredBackBufferFB;
}

// OFFSET: 0x007175B0/0x1403EAB30
CGxFrameBuffer* Liquid::GetBlurredReflectionBufferFB() {
    return s_blurredReflectionFB;
}

// OFFSET: 0x007175D0/0x1403EAB50
CGxFrameBuffer* Liquid::GetRealReflectionFB() {
    return s_realReflectionFB;
}

// OFFSET: 0x007175E0/0x1403EAB60
CGxFrameBuffer* Liquid::GetFakeReflectionFB() {
    return s_fakeReflectionFB;
}

// OFFSET: 0x007179D0/0x1403EB020
// 15595 tracks validity per procedural depth texture in s_*Valid; 12340 packed the same four flags into
// s_unk_D43B48[1..3] and slot 0 covered the (never-created) s_unk_D43B50 array.
void Liquid::UpdateProceduralTextures() {
    if (!Liquid::s_oceanDepthTexValid && s_oceanDepthTex) {
        CGxTex* tex = TextureGetGxTex(s_oceanDepthTex, 0, nullptr);

        if (tex) {
            CiRect rect(0, 0, tex->m_height, tex->m_width);
            GxTexUpdate(tex, rect, 0);
            Liquid::s_oceanDepthTexValid = 1;
        }
    }

    if (!Liquid::s_riverDepthTexValid && s_riverDepthTex) {
        CGxTex* tex = TextureGetGxTex(s_riverDepthTex, 0, nullptr);

        if (tex) {
            CiRect rect(0, 0, tex->m_height, tex->m_width);
            GxTexUpdate(tex, rect, 0);
            Liquid::s_riverDepthTexValid = 1;
        }
    }

    if (!Liquid::s_wmoWaterTexValid && s_wmoWaterTex) {
        CGxTex* tex = TextureGetGxTex(s_wmoWaterTex, 0, nullptr);

        if (tex) {
            CiRect rect(0, 0, tex->m_height, tex->m_width);
            GxTexUpdate(tex, rect, 0);
            Liquid::s_wmoWaterTexValid = 1;
        }
    }
}

// OFFSET: 0x008A2AA0
void Liquid::InvalidateProceduralTextures() {
    Liquid::s_oceanDepthTexValid = 0;
    Liquid::s_riverDepthTexValid = 0;
    Liquid::s_wmoWaterTexValid = 0;
}

// VERIFY: 331f7c3c manual
// OFFSET: 0x008A2AC0
void Liquid::WmoWaterTexCallback(EGxTexCommand command, uint32_t width, uint32_t height, uint32_t depth, uint32_t mipLevel, void* userArg, uint32_t& texelStrideInBytes, const void*& texels) {
    // STATIC: 0x00D43BE0
    static CImVector s_texels[64 * 8];

    if (command != GxTex_Latch) {
        return;
    }

    DayNight::DNInfo* info = DayNight::GetInfo();
    CImVector band = info->m_bands.m_waterColors[3];
    uint8_t shallowAlpha = CMath::fint_(info->m_bands.m_waterShallowAlpha * 255.0f);
    uint8_t deepAlpha = CMath::fint_(info->m_bands.m_waterDeepAlpha * 255.0f);
    int32_t alphaStep = ((deepAlpha - shallowAlpha) << 8) >> 6;
    int32_t alpha = shallowAlpha << 8;

    CImVector color;
    color.b = band.b;
    color.g = band.g;
    color.r = band.r;

    CImVector white;
    white.b = 0xFF;
    white.g = 0xFF;
    white.r = 0xFF;

    for (uint32_t y = 0; y < 64; y++) {
        for (uint32_t x = 0; x < 4; x++) {
            color.a = alpha >> 8;
            s_texels[y * 8 + x] = color;
        }

        for (uint32_t x = 4; x < 8; x++) {
            white.a = alpha >> 8;
            s_texels[y * 8 + x] = white;
        }

        alpha += alphaStep;
    }

    texelStrideInBytes = 8 * sizeof(CImVector);
    texels = s_texels;
}

// VERIFY: aa313e0a manual
// OFFSET: 0x008A2BF0
void Liquid::ExtWaterDepthTexCallback(EGxTexCommand command, uint32_t width, uint32_t height, uint32_t depth, uint32_t mipLevel, void* userArg, uint32_t& texelStrideInBytes, const void*& texels) {
    // STATIC: 0x00D443E8
    static CImVector s_texels[64 * 8];

    if (command != GxTex_Latch) {
        return;
    }

    DayNight::DNInfo* info = DayNight::GetInfo();

    uint8_t shallowAlpha[2];
    shallowAlpha[0] = CMath::fint_(info->m_bands.m_oceanShallowAlpha * 255.0f);
    shallowAlpha[1] = CMath::fint_(info->m_bands.m_waterShallowAlpha * 255.0f);

    uint8_t deepAlpha[2];
    deepAlpha[0] = CMath::fint_(info->m_bands.m_oceanDeepAlpha * 255.0f);
    deepAlpha[1] = CMath::fint_(info->m_bands.m_waterDeepAlpha * 255.0f);

    uintptr_t type = reinterpret_cast<uintptr_t>(userArg);
    CImVector shallowColor = info->m_bands.m_waterColors[type * 2];
    CImVector deepColor = info->m_bands.m_waterColors[type * 2 + 1];

    int32_t alphaStep = ((deepAlpha[type] - shallowAlpha[type]) << 8) >> 6;
    int32_t redStep = ((deepColor.r - shallowColor.r) << 8) >> 6;
    int32_t blueStep = ((deepColor.b - shallowColor.b) << 8) >> 6;
    int32_t greenStep = ((deepColor.g - shallowColor.g) << 8) >> 6;

    int32_t alpha = shallowAlpha[type] << 8;
    int32_t red = shallowColor.r << 8;
    int32_t green = shallowColor.g << 8;
    int32_t blue = shallowColor.b << 8;

    CImVector* row = s_texels;

    for (uint32_t y = 0; y < height; y++) {
        CImVector color;
        color.b = blue >> 8;
        color.g = green >> 8;
        color.r = red >> 8;
        color.a = alpha >> 8;

        if (y == height - 1 && type == 0) {
            C3Vector rgb(color);
            C3Vector hsv;
            RGBtoHSV(rgb, hsv);
            hsv.z *= 0.9f;
            HSVtoRGB(hsv, rgb);
            color = rgb;
        }

        for (uint32_t x = 0; x < width; x++) {
            row[x] = color;
        }

        green += greenStep;
        blue += blueStep;
        alpha += alphaStep;
        red += redStep;
        row += width;
    }

    texelStrideInBytes = width * sizeof(CImVector);
    texels = s_texels;
}
