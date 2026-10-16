#include "Liquid.h"
#include "Engine/Source/Gx/CGxDevice/CGxDevice.h"
#include "Engine/Source/ShaderEffect/DepthCache.h"
#include "Engine/Source/Storm/H/storm.h"

// STATIC: 0x00D43B34
CDataAllocator Liquid::s_instanceAllocator(sizeof(Liquid::CInstance), 0x20);

// STATIC: 0x00B23F64
float Liquid::s_worldCoordScale = 1.0f;

// STATIC: 0x00B23F68
int32_t Liquid::s_specularEnabled = 1;

// STATIC: 0x00D439E8
IDatabase<LiquidMaterialRec>* Liquid::s_liquidMaterialDB;

// STATIC: 0x00D439EC
IDatabase<LiquidTypeRec>* Liquid::s_liquidTypeDB;

// STATIC: 0x00D439F0
char Liquid::s_shaderPath[256];

// STATIC: 0x00D43AF0
int32_t Liquid::s_shadersEnabled;

// STATIC: 0x00D43AF4
int32_t Liquid::s_useCameraRelativeCoords;

// STATIC: 0x00D43AF8
int32_t Liquid::s_settingsBankRefCount;

// STATIC: 0x00D43AFC
int32_t Liquid::s_materialBankRefCount;

// VERIFY: 96a38663 manual
// OFFSET: 0x008A32C0
Liquid::IEnvironment::~IEnvironment() {
}

// INLINED
Liquid::IGeomFactory::~IGeomFactory() {
}

// INLINED
Liquid::IWaveManager::~IWaveManager() {
}

// INLINED
Liquid::IMaterial::~IMaterial() {
}

// Not in the original binary. Ported from 15595; empty in 15595 too. CMaterialBank::ResetMaterials calls it on
// every material when the water settings change. The 12340 material classes do not override it yet - that is the
// C.2 material stage, so this currently just visits each material and does nothing.
void Liquid::IMaterial::Reset() {
}

// Not in the original binary. Ported from 15595; also empty there.
Liquid::DisplMapPerturbationMesh::DisplMapPerturbationMesh() {
}

// VERIFY: afda035c auto
// OFFSET: 0x008A1770
int32_t Liquid::Initialize(int32_t useCameraRelativeCoords, float worldCoordScale, int32_t shadersEnabled, const char* shaderPath, IDatabase<LiquidTypeRec>* liquidTypeDB, IDatabase<LiquidMaterialRec>* liquidMaterialDB) {
    SStrCopy(Liquid::s_shaderPath, shaderPath, sizeof(Liquid::s_shaderPath));
    Liquid::s_worldCoordScale = worldCoordScale;
    Liquid::s_settingsBankRefCount++;
    Liquid::s_materialBankRefCount++;
    Liquid::s_useCameraRelativeCoords = useCameraRelativeCoords;
    Liquid::s_shadersEnabled = shadersEnabled;
    Liquid::s_specularEnabled = 1;
    Liquid::s_liquidTypeDB = liquidTypeDB;
    Liquid::s_liquidMaterialDB = liquidMaterialDB;
    return 1;
}

// VERIFY: 6bde3ced manual
// OFFSET: 0x008A2400
void Liquid::Shutdown() {
    int32_t count = Liquid::s_materialBankRefCount--;
    Liquid::s_liquidTypeDB = nullptr;
    Liquid::s_liquidMaterialDB = nullptr;

    if (count == 1) {
        Liquid::CMaterialBank::Destroy();
        Liquid::s_shadersEnabled = 0;
    }

    Liquid::CSettingsBank::Destroy();
    Liquid::ClearMaterials();
    Liquid::s_instanceAllocator.Clear(".?AVCInstance@Liquid@@", SLOG_OBJECT, 1);
}

// VERIFY: 58e8bcb0 auto
// OFFSET: 0x008A1700
void Liquid::CInstance::AddRef() {
    this->m_refCount++;
}

// VERIFY: 01aeb3b0 manual
// OFFSET: 0x008A1710
void Liquid::CInstance::Purge() {
    this->m_geomFactory->Purge();
}

// VERIFY: 0c7c5584 auto
// OFFSET: 0x008A1720
void Liquid::SetShadersEnabled(int32_t enabled) {
    Liquid::s_shadersEnabled = enabled;
}

// VERIFY: 6952e6f0 auto
// OFFSET: 0x008A1730
void Liquid::SetSpecularEnabled(int32_t enabled) {
    Liquid::s_specularEnabled = enabled;
}

// VERIFY: e803d76a auto
// OFFSET: 0x008A1740
int32_t Liquid::UseCameraRelativeCoords() {
    return Liquid::s_useCameraRelativeCoords;
}

// VERIFY: d02f3d04 auto
// OFFSET: 0x008A1750
float Liquid::WorldCoordScale() {
    return Liquid::s_worldCoordScale;
}

// VERIFY: 2174e15a auto
// OFFSET: 0x008A1760
const char* Liquid::GetShaderSuffix() {
    return Liquid::s_shaderPath;
}

// VERIFY: 6413183d manual
// OFFSET: 0x008A1A40
Liquid::CInstance::CInstance() {
    this->m_material = nullptr;
    this->m_settings = nullptr;
    this->m_geomFactory = nullptr;
    this->m_waveManager = nullptr;
    this->m_environment = nullptr;
    this->m_bounds.r = 0.0f;
    this->m_queued = 0;
    this->m_refCount = 1;
}

// VERIFY: dfea0c3c manual
// OFFSET: 0x008A1AC0
void Liquid::CInstance::Release() {
    if (this->m_refCount-- == 1) {
        this->m_geomFactory->Release();
        this->m_environment->Release();

        if (this->m_waveManager) {
            this->m_waveManager->Release();
        }

        Liquid::s_instanceAllocator.PutData(this, nullptr, 0);
    }
}

// VERIFY: 43d6fc45 auto
// OFFSET: 0x008A1B00
Liquid::CInstance* Liquid::CInstance::Create() {
    void* mem = Liquid::s_instanceAllocator.GetData(0, ".?AVCInstance@Liquid@@", SLOG_OBJECT);
    return mem ? new (mem) Liquid::CInstance() : nullptr;
}

// Not in the original binary. Ported from 15595 (Cataclysm), settings/config cluster only - see Liquid.h.
// NOTE: 15595 keeps s_materialsDirty / s_configDirty and a callback list to rebuild material banks when the
// water settings change. 12340 has no material bank rebuild path yet (that is the material/renderer stage), so
// the flags are maintained and read but only the fog/detail state and DepthCache ownership take effect.

// STATIC: 0x00E4EADC/0x140D37E94
static int32_t s_cameraRelativeCoords;

// STATIC: 0x00E4EAFC/0x140D37EB4
static int32_t s_configDirty;

// STATIC: 0x00D0063C/0x140D37E44
static int32_t s_materialsDirty = 1;

// STATIC: 0x00E4EAE0/0x140D37E98
static uint32_t s_waterDetail;

// STATIC: 0x00E4EAE4/0x140D37E9C
static uint32_t s_rippleDetail;

// STATIC: 0x00E4EAE8/0x140D37EA0
static uint32_t s_reflectionMode;

// STATIC: 0x00E4EAEC/0x140D37EA4
static uint32_t s_reflectionDownscale;

// STATIC: 0x00E4EAF0/0x140D37EA8
static int32_t s_cameraSubmerged;

// STATIC: 0x00E4EAF4/0x140D37EAC
static int32_t s_needBackBuffer;

// STATIC: 0x00E4EAF8/0x140D37EB0
static int32_t s_needDynDisplMap;

// STATIC: 0x00E4EB00/0x140D37EB8
static float s_underWaterFogStart;

// STATIC: 0x00E4EB04/0x140D37EBC
static float s_underWaterFogEnd;

// STATIC: 0x00E4EB08/0x140D37EC0
static float s_underWaterFogDensity;

// STATIC: 0x00E4EB18/0x140D37ED4
static CImVector s_underWaterFogColor;

// STATIC: 0x00E4EB0C/0x140D37EC8
static float s_aboveWaterFogStart;

// STATIC: 0x00E4EB10/0x140D37ECC
static float s_aboveWaterFogEnd;

// STATIC: 0x00E4EB14/0x140D37ED0
static float s_aboveWaterFogDensity;

// STATIC: 0x00E4EB1C/0x140D37ED8
static CImVector s_aboveWaterFogColor;

// STATIC: 0x00E4EB20/0x140D37EE0
static TSGrowableArray<Liquid::CONFIG_CHANGED_CALLBACK> s_configChangedCallbacks;

// OFFSET: 0x0071ACF0/0x1403EF420
void Liquid::SignalConfigChanged(int32_t materials) {
    int32_t wasDirty = s_configDirty;
    s_configDirty = 1;

    if (!wasDirty) {
        s_materialsDirty = materials;
    } else {
        s_materialsDirty |= materials;
    }
}

// OFFSET: 0x0071AD20/0x1403EF450
void Liquid::SetUnderWaterFog(float start, float end, float density, const CImVector& color) {
    s_underWaterFogStart = start;
    s_underWaterFogEnd = end;
    s_underWaterFogDensity = density;
    s_underWaterFogColor = color;
}

// OFFSET: 0x0071AD60/0x1403EF480
void Liquid::GetUnderWaterFog(float& start, float& end, float& density, CImVector& color) {
    start = s_underWaterFogStart;
    end = s_underWaterFogEnd;
    density = s_underWaterFogDensity;
    color = s_underWaterFogColor;
}

// OFFSET: 0x0071ADA0/0x1403EF4B0
void Liquid::SetAboveWaterFog(float start, float end, float density, const CImVector& color) {
    s_aboveWaterFogStart = start;
    s_aboveWaterFogEnd = end;
    s_aboveWaterFogDensity = density;
    s_aboveWaterFogColor = color;
}

// OFFSET: 0x0071ADE0/0x1403EF4E0
void Liquid::GetAboveWaterFog(float& start, float& end, float& density, CImVector& color) {
    start = s_aboveWaterFogStart;
    end = s_aboveWaterFogEnd;
    density = s_aboveWaterFogDensity;
    color = s_aboveWaterFogColor;
}

// OFFSET: 0x0071AE20/0x1403EF290
uint32_t Liquid::GetWaterDetail() {
    return s_waterDetail;
}

// OFFSET: 0x0071AE30/0x1403EF2A0
uint32_t Liquid::GetRippleDetail() {
    return s_rippleDetail;
}

// OFFSET: 0x0071AE40/0x1403EF2B0
uint32_t Liquid::GetReflectionMode() {
    return s_reflectionMode;
}

// OFFSET: 0x0071AE50/0x1403EF2C0
uint32_t Liquid::GetReflectionDownscale() {
    return s_reflectionDownscale;
}

// OFFSET: 0x0071AE60/0x1403EF2D0
int32_t Liquid::WaterDetailNeedsDepthCache(uint32_t detail) {
    return detail >= 2;
}

// OFFSET: 0x0071AE70/0x1403EF2E0
int32_t Liquid::CameraSubmerged() {
    return s_cameraSubmerged;
}

// OFFSET: 0x0071AE80/0x1403EF2F0
void Liquid::SetCameraSubmerged(int32_t submerged) {
    s_cameraSubmerged = submerged;
}

// OFFSET: 0x0071AE90/0x1403EF300
int32_t Liquid::NeedBackBuffer() {
    return s_needBackBuffer;
}

// OFFSET: 0x0071AEA0/0x1403EF310
void Liquid::SetNeedBackBuffer(int32_t need) {
    s_needBackBuffer = need;
}

// OFFSET: 0x0071AEB0/0x1403EF320
int32_t Liquid::NeedDynDisplMap() {
    return s_needDynDisplMap;
}

// OFFSET: 0x0071AEC0/0x1403EF330
void Liquid::SetNeedDynDisplMap(int32_t need) {
    s_needDynDisplMap = need;
}

// OFFSET: 0x0071AED0/0x1403EF340
int32_t Liquid::GetShadersEnabled() {
    return s_shadersEnabled;
}

// OFFSET: 0x0071AFC0/0x1403EF510
uint32_t Liquid::ValidateWaterDetail(uint32_t detail) {
    if (!detail) {
        return 0;
    }

    if (detail > 3) {
        return 2;
    }

    CGxDevice* device = g_theGxDevicePtr;

    if (detail == 1) {
        if (device->m_caps.m_shaderModel < 4) {
            return 64;
        }
    } else {
        if (device->m_caps.m_shaderModel < 5) {
            return 128;
        }

        if (!DepthCache::IsSupported(nullptr, nullptr, nullptr)) {
            return 8;
        }

        device = g_theGxDevicePtr;
    }

    if (device->m_caps.m_texFmt[GxTex_Gr1616F] != 1 || !device->m_caps.m_texTarget[GxTex_NonPow2]) {
        return 8;
    }

    return device->m_caps.m_texTarget[GxTex_Volume] != 0 ? 0 : 8;
}

// OFFSET: 0x0071B050/0x1403EF5B0
int32_t Liquid::SetWaterDetail(uint32_t detail) {
    if (Liquid::ValidateWaterDetail(detail)) {
        return 0;
    }

    s_materialsDirty = 1;
    s_configDirty |= detail != s_waterDetail;

    if ((s_waterDetail >= 2) != (detail >= 2)) {
        if (detail >= 2) {
            DepthCache::AddRef();
            s_waterDetail = detail;
            return 1;
        }

        DepthCache::Release();
    }

    s_waterDetail = detail;
    return 1;
}

// OFFSET: 0x0071B0C0/0x1403EF630
uint32_t Liquid::ValidateRippleDetail(uint32_t detail) {
    if (!detail) {
        return detail;
    }

    if (detail > 2) {
        return 2;
    }

    if (g_theGxDevicePtr->m_caps.m_shaderModel < 5) {
        return 128;
    }

    return g_theGxDevicePtr->m_caps.m_texFmt[GxTex_Gr1616F] != 1 ? 8 : 0;
}

// OFFSET: 0x0071B100/0x1403EF670
int32_t Liquid::SetRippleDetail(uint32_t detail) {
    if (detail && (detail > 2 || g_theGxDevicePtr->m_caps.m_shaderModel < 5 || g_theGxDevicePtr->m_caps.m_texFmt[GxTex_Gr1616F] != 1)) {
        return 0;
    }

    int32_t same = detail == s_rippleDetail;
    s_rippleDetail = detail;
    s_configDirty |= !same;
    return 1;
}

// OFFSET: 0x0071B150/0x1403EF6C0
uint32_t Liquid::ValidateReflectionMode(uint32_t mode) {
    if (!mode) {
        return mode;
    }

    if (mode > 3) {
        return 2;
    }

    return g_theGxDevicePtr->m_caps.m_shaderModel >= 5 ? 0 : 0x80;
}

// OFFSET: 0x0071B190/0x1403EF6F0
int32_t Liquid::SetReflectionMode(uint32_t mode) {
    if (mode && (mode > 3 || g_theGxDevicePtr->m_caps.m_shaderModel < 5)) {
        return 0;
    }

    int32_t same = mode == s_reflectionMode;
    s_reflectionMode = mode;
    s_configDirty |= !same;
    return 1;
}

// OFFSET: 0x0071B1D0/0x1403EF730
uint32_t Liquid::ValidateReflectionDownscale(uint32_t downscale) {
    if (!downscale) {
        return downscale;
    }

    if (downscale > 4) {
        return 2;
    }

    return g_theGxDevicePtr->m_caps.m_texTarget[GxTex_NonPow2] != 0 ? 0 : 8;
}

// OFFSET: 0x0071B200/0x1403EF760
int32_t Liquid::SetReflectionDownscale(uint32_t downscale) {
    if (downscale && (downscale > 4 || !g_theGxDevicePtr->m_caps.m_texTarget[GxTex_NonPow2])) {
        return 0;
    }

    int32_t same = downscale == s_reflectionDownscale;
    s_reflectionDownscale = downscale;
    s_configDirty |= !same;
    return 1;
}

// OFFSET: 0x0071B3E0/0x1403EFA10
void Liquid::AddConfigChangedCallback(CONFIG_CHANGED_CALLBACK callback) {
    s_configChangedCallbacks.New(callback);
}

// OFFSET: 0x0071B400/0x1403EFA30
void Liquid::RemoveConfigChangedCallback(CONFIG_CHANGED_CALLBACK callback) {
    uint32_t count = s_configChangedCallbacks.Count();

    for (uint32_t i = 0; i < count; i++) {
        if (s_configChangedCallbacks[i] == callback) {
            s_configChangedCallbacks[i] = s_configChangedCallbacks[s_configChangedCallbacks.Count() - 1];
            s_configChangedCallbacks.SetCount(count - 1);
            return;
        }
    }

}

// OFFSET: 0x0071B2A0/0x1403EF800
// Ported from 15595. Runs once per frame from the world scene before liquids draw; the dirty flags are set by
// SignalConfigChanged (water settings changed) and OnTexturesDirty (device restore). Lives here, not in
// LiquidProcTextures.cpp, because the flags and the callback list are file-scope statics of this file.
void Liquid::Update() {
    if (s_configDirty) {
        if (s_materialsDirty) {
            Liquid::CMaterialBank::ResetMaterials();
        }

        for (uint32_t i = 0; i < s_configChangedCallbacks.Count(); i++) {
            s_configChangedCallbacks[i]();
        }

        Liquid::DestroyProceduralTextures(s_materialsDirty);

        if (s_materialsDirty) {
            Liquid::CMaterialBank::Purge();
            Liquid::CSettingsBank::Purge();
        }

        Liquid::CreateProceduralTextures(s_materialsDirty);
        s_configDirty = 0;
        s_materialsDirty = 1;
    }

    s_needBackBuffer = 0;
    s_needDynDisplMap = 0;
    Liquid::s_oceanDepthTexValid = 0;
    Liquid::s_riverDepthTexValid = 0;
    Liquid::s_wmoWaterTexValid = 0;
}
