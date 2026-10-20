#include "WorldParam.h"
#include <cstdio>
#include <cstring>
#include <new>
#include "Engine/Source/Gx/CGxDevice/CGxDevice.h"
#include "Engine/Source/Services/GfxSingletonManager.h"
#include "Engine/Source/Services/Texture.h"
#include "Engine/Source/ShaderEffect/ShaderEffect.h"
#include "Engine/Source/ShaderEffect/ShadowCache.h"
#include "WoW/Common/AaBsp.h"
#include "WoW/Source/UIUtil/PortraitButton.h"
#include "WoW/Source/WorldClient/Map.h"
#include "WoW/Source/WorldClient/Types.h"
#include "WoW/Source/WorldClient/World.h"
#include "Engine/Source/ConsoleBase/ConsoleVar.h"
#include "Engine/Source/ConsoleBase/IConsole.h"
#include "Engine/Source/Storm/H/storm.h"
#include "WoW/Source/ConsoleDevice/ConsoleDevice.h"

// STATIC: 0x00D1C414
static uint32_t s_mapObjLightLOD;

// STATIC: 0x00CD856C
CVar* CWorldParam::cvar_maxLights;

// STATIC: 0x00CD8570
CVar* CWorldParam::cvar_lod;

// STATIC: 0x00CD8574
CVar* CWorldParam::cvar_mapShadows;

// STATIC: 0x00CD8578
CVar* CWorldParam::cvar_shadowLevel;

// Not in the original binary. See ShadowTextureSizeCallback.
CVar* CWorldParam::cvar_shadowTextureSize;
CVar* CWorldParam::cvar_shadowMode;
CVar* CWorldParam::cvar_cascadedShadows;
CVar* CWorldParam::cvar_shadowBandRange;

// Not in the original binary. See CUnitOutline::Enabled.
CVar* CWorldParam::cvar_unitOutline;

// Not in the original binary. See CAntiAlias::Enabled.
CVar* CWorldParam::cvar_antiAlias;

// Not in the original binary. See CAntiAlias's quality preset table.
CVar* CWorldParam::cvar_fxaaQuality;

// STATIC: 0x00CD857C
CVar* CWorldParam::cvar_texLodBias;

// STATIC: 0x00CD8580
CVar* CWorldParam::cvar_farClip;

// STATIC: 0x00CD8584
CVar* CWorldParam::cvar_nearClip;

// STATIC: 0x00CD858C
CVar* CWorldParam::cvar_specular;

// STATIC: 0x00CD8590
CVar* CWorldParam::cvar_mapObjLightLOD;

// STATIC: 0x00CD8594
CVar* CWorldParam::cvar_particleDensity;

// STATIC: 0x00CD8598
CVar* CWorldParam::cvar_waterLOD;

// STATIC: 0x00CD859C
CVar* CWorldParam::cvar_baseMip;

// STATIC: 0x00CD85A0
CVar* CWorldParam::cvar_horizonFarClip;

// STATIC: 0x00CD85A4
CVar* CWorldParam::cvar_horizonNearClip;

// STATIC: 0x00CD85A8
CVar* CWorldParam::cvar_showFootprints;

// STATIC: 0x00CD85AC
CVar* CWorldParam::cvar_bspCache;

// STATIC: 0x00CD85B0
CVar* CWorldParam::cvar_footstepBias;

// STATIC: 0x00CD85B4
CVar* CWorldParam::cvar_occlusion;

// STATIC: 0x00CD85B8
CVar* CWorldParam::cvar_worldPoolUsage;

// STATIC: 0x00CD85BC
CVar* CWorldParam::cvar_groundEffectDensity;

// STATIC: 0x00CD85C0
CVar* CWorldParam::cvar_groundEffectDist;

// STATIC: 0x00CD85C4
CVar* CWorldParam::cvar_terrainAlphaBitDepth;

// STATIC: 0x00CD85C8
CVar* CWorldParam::cvar_farClipOverride;

// STATIC: 0x00CD85CC
CVar* CWorldParam::cvar_objectFade;

// STATIC: 0x00CD85D0
CVar* CWorldParam::cvar_objectFadeZFill;

// STATIC: 0x00CD85D4
CVar* CWorldParam::cvar_environmentDetail;

// STATIC: 0x00CD85D8
CVar* CWorldParam::cvar_hwPCF;

// STATIC: 0x00CD85DC
CVar* CWorldParam::cvar_extShadowQuality;

// STATIC: 0x00CD85E0
CVar* CWorldParam::cvar_projectedTextures;

// STATIC: 0x00CD85E4
CVar* CWorldParam::cvar_gxTextureCacheSize;

// VERIFY: 54f5415a manual
// OFFSET: 0x004023C0
bool CWorldParam::TextureFilteringCallback(CVar* var, const char* oldValue, const char* value, void* arg) {
    int32_t mode = SStrToInt(value);

    if (mode >= 0 && mode < 6) {
        return true;
    }

    char buffer[256];
    SStrPrintf(buffer, sizeof(buffer), "Texture filtering mode must be in range 0 to %d.", 6);
    ConsoleWrite(buffer, DEFAULT_COLOR);

    return false;
}

// VERIFY: 14adfded auto
// OFFSET: 0x0078D610
bool CWorldParam::LodCallback(CVar* var, const char* oldValue, const char* value, void* arg) {
    if (SStrToInt(value)) {
        ConsoleWrite("Terrain LOD enabled.", DEFAULT_COLOR);
        CWorld::enables |= Enable_Lod;
    } else {
        ConsoleWrite("Terrain LOD disabled.", DEFAULT_COLOR);
        CWorld::enables &= ~Enable_Lod;
    }

    return true;
}

// VERIFY: fb93d195 auto
// OFFSET: 0x0078D660
bool CWorldParam::MapShadowsCallback(CVar* var, const char* oldValue, const char* value, void* arg) {
    if (SStrToInt(value)) {
        ConsoleWrite("Terrain shadows enabled.", DEFAULT_COLOR);
        CWorld::enables |= Enable_Shadow;
    } else {
        ConsoleWrite("Terrain shadows disabled.", DEFAULT_COLOR);
        CWorld::enables &= ~Enable_Shadow;
    }

    return true;
}

// VERIFY: 4ef1e46f auto
// OFFSET: 0x0078D6B0
bool CWorldParam::MaxLightsCallback(CVar* var, const char* oldValue, const char* value, void* arg) {
    uint32_t lights = SStrToInt(value);

    if (lights < 1 || lights > 4) {
        ConsoleWriteA("MaxLights must be in range 1 - %i.", DEFAULT_COLOR, 4);

        return false;
    }

    CWorld::maxLights = lights;

    return true;
}

// VERIFY: bbb3c156 auto
// OFFSET: 0x0078D6F0
bool CWorldParam::ShadowLevelCallback(CVar* var, const char* oldValue, const char* value, void* arg) {
    if (SStrToInt(value) > 1) {
        ConsoleWrite("Shadow mip level must be in range 0 - 1.", DEFAULT_COLOR);

        return false;
    }

    ConsoleWrite("Shadow mip level changed upon restart.", DEFAULT_COLOR);

    return true;
}

// VERIFY: 964fa6bd auto
// OFFSET: 0x0078D730
bool CWorldParam::TexLodBiasCallback(CVar* var, const char* oldValue, const char* value, void* arg) {
    float bias = SStrToFloat(value);

    if (bias < -1.0f || bias > 1.0f) {
        ConsoleWrite("TexLodBias must be in range -1.0 - 1.0.", DEFAULT_COLOR);

        return false;
    }

    World::SetTexLodBias(bias);

    return true;
}

// VERIFY: b4a11982 auto
// OFFSET: 0x0078D780
bool CWorldParam::FarClipCallback(CVar* var, const char* oldValue, const char* value, void* arg) {
    World::SetFarClip(SStrToFloat(value));

    return true;
}

// VERIFY: b99f7609 auto
// OFFSET: 0x0078D7A0
bool CWorldParam::NearClipCallback(CVar* var, const char* oldValue, const char* value, void* arg) {
    World::SetNearClip(SStrToFloat(value));

    return true;
}

// VERIFY: 452fa715 auto
// OFFSET: 0x0078D7C0
bool CWorldParam::HorizonFarClipScaleCallback(CVar* var, const char* oldValue, const char* value, void* arg) {
    float scale = SStrToFloat(value);

    if (scale < 3.0f) {
        scale = 3.0f;
    } else if (scale > 6.0f) {
        scale = 6.0f;
    }

    World::SetHorizonFarClipScale(scale);

    return true;
}

// VERIFY: bd34982b auto
// OFFSET: 0x0078D810
bool CWorldParam::HorizonNearClipScaleCallback(CVar* var, const char* oldValue, const char* value, void* arg) {
    float scale = SStrToFloat(value);

    if (scale < 0.01f) {
        scale = 0.01f;
    } else if (scale > 1.0f) {
        scale = 1.0f;
    }

    World::SetHorizonNearClipScale(scale);

    return true;
}

// VERIFY: 7db07d04 auto
// OFFSET: 0x0078D860
bool CWorldParam::ParticleDensityCallback(CVar* var, const char* oldValue, const char* value, void* arg) {
    float density = SStrToFloat(value);

    if (density < 0.1f || density > 1.0f) {
        ConsoleWrite("Value must be between 0.1 and 1.0.", DEFAULT_COLOR);

        return false;
    }

    ParticleSystemManager::SetScaler(density);

    return true;
}

// VERIFY: aafd8987 auto
// OFFSET: 0x0078D8B0
bool CWorldParam::WaterLodCallback(CVar* var, const char* oldValue, const char* value, void* arg) {
    World::s_waterLOD = 0;

    if (SStrToInt(value)) {
        ConsoleWrite("waterLOD fixed to 0", DEFAULT_COLOR);

        return false;
    }

    return true;
}

// VERIFY: 6626e5c9 auto
// OFFSET: 0x0078D8F0
bool CWorldParam::ShowFootprintsCallback(CVar* var, const char* oldValue, const char* value, void* arg) {
    if (SStrToInt(value)) {
        ConsoleWrite("Showing foot prints.", DEFAULT_COLOR);
        CWorld::enables |= 0x400;
    } else {
        ConsoleWrite("Hiding foot prints.", DEFAULT_COLOR);
        CWorld::enables &= ~0x400;
    }

    return true;
}

// VERIFY: 127cd82e manual
// OFFSET: 0x0078D940
bool CWorldParam::FootstepBiasCallback(CVar* var, const char* oldValue, const char* value, void* arg) {
    float bias;
    sscanf(value, "%f", &bias);

    if (bias < 0.0f || bias > 1.0f) {
        ConsoleWrite("Footstep bias must be in the range (0, 1)", DEFAULT_COLOR);

        return false;
    }

    char buffer[256];
    sprintf(buffer, "Footstep bias set to %f", bias);
    ConsoleWrite(buffer, DEFAULT_COLOR);

    return true;
}

// VERIFY: a8041b4a manual
// OFFSET: 0x0078D9D0
bool CWorldParam::HardwareOcclusionCallback(CVar* var, const char* oldValue, const char* value, void* arg) {
    if (SStrToInt(value)) {
        ConsoleWrite("Using hardware occlusion test.", DEFAULT_COLOR);
    } else {
        ConsoleWrite("Disabled hardware occlusion test.", DEFAULT_COLOR);
    }

    return true;
}

// VERIFY: 8f56c0f0 auto
// OFFSET: 0x0078DA10
bool CWorldParam::WorldPoolUsageCallback(CVar* var, const char* oldValue, const char* value, void* arg) {
    if (SStrCmpI(value, "Dynamic", STORM_MAX_STR)) {
        ConsoleWrite("WorldPoolUsage must be Stream, Dynamic or Static.", DEFAULT_COLOR);

        return false;
    }

    ConsoleWrite("WorldPoolUsage set on restart.", DEFAULT_COLOR);

    return true;
}

// VERIFY: b80c920b auto
// OFFSET: 0x0078DA50
bool CWorldParam::TerrainAlphaBitDepthCallback(CVar* var, const char* oldValue, const char* value, void* arg) {
    uint32_t depth = SStrToInt(value);

    if (depth == 4 || depth == 8) {
        char buffer[256];
        sprintf(buffer, "Alpha map bit depth set to %dbit on restart.", depth);
        ConsoleWrite(buffer, DEFAULT_COLOR);

        return true;
    }

    ConsoleWrite("Alpha map bit depth must be 4 or 8.", DEFAULT_COLOR);

    return false;
}

// VERIFY: 38a0f503 auto
// OFFSET: 0x0078DAB0
bool CWorldParam::GroundEffectDensityCallback(CVar* var, const char* oldValue, const char* value, void* arg) {
    int32_t density = SStrToInt(value);

    if (density < 16 || density > 256) {
        char buffer[256];
        sprintf(buffer, "Ground effect density must be in range %d to %d.", 16, 256);
        ConsoleWriteA(buffer, DEFAULT_COLOR);

        return false;
    }

    World::SetDetailDoodadDensity(density);

    return true;
}

// VERIFY: 7685b115 auto
// OFFSET: 0x0078DB10
bool CWorldParam::GroundEffectDistCallback(CVar* var, const char* oldValue, const char* value, void* arg) {
    float dist = SStrToFloat(value);

    if (dist < 0.0f || dist > 140.0f) {
        char buffer[256];
        sprintf(buffer, "Ground effect distance must be in range %0f to %0f.", 0.0, 140.0);
        ConsoleWriteA(buffer, DEFAULT_COLOR);

        return false;
    }

    World::SetDetailDoodadDist(dist);

    return true;
}

// VERIFY: 4551a2de auto
// OFFSET: 0x0078DB90
bool CWorldParam::ObjectFadeCallback(CVar* var, const char* oldValue, const char* value, void* arg) {
    if (SStrToInt(value)) {
        ConsoleWrite("Object distance fade enabled.", DEFAULT_COLOR);
        CWorld::enables |= 0x4000;
    } else {
        ConsoleWrite("Object distance fade disabled.", DEFAULT_COLOR);
        CWorld::enables &= ~0x4000;
    }

    return true;
}

// VERIFY: 2278f1b3 auto
// OFFSET: 0x0078DBE0
bool CWorldParam::ObjectFadeZFillCallback(CVar* var, const char* oldValue, const char* value, void* arg) {
    if (SStrToInt(value)) {
        ConsoleWrite("Object distance fade ZFill pass enabled.", DEFAULT_COLOR);
        CWorld::enables |= 0x8000;
    } else {
        ConsoleWrite("Object distance fade ZFill pass disabled.", DEFAULT_COLOR);
        CWorld::enables &= ~0x8000;
    }

    return true;
}

// VERIFY: 1c43c03c manual
// OFFSET: 0x0078DC30
bool CWorldParam::FarClipOverrideCallback(CVar* var, const char* oldValue, const char* value, void* arg) {
    if (SStrToInt(value) > 1) {
        ConsoleWrite("farClipOverride must be 0 or 1.", DEFAULT_COLOR);

        return false;
    }

    return true;
}

// VERIFY: 82f0dd4a auto
// OFFSET: 0x0078DC60
bool CWorldParam::EnvironmentDetailCallback(CVar* var, const char* oldValue, const char* value, void* arg) {
    float detail = SStrToFloat(value);

    if (detail < 0.5f) {
        detail = 0.5f;
    } else if (detail > 1.5f) {
        detail = 1.5f;
    }

    CWorldScene::ScaleFadeDist(detail);

    return true;
}

// Not in the original binary. The 3.3.5a UI drives both shadow resolution and shadow method through
// shadowTextureSize and shadowMode (see Interface\FrameXML\VideoOptionsPanels.lua SHADOW_LEVELS), but
// neither was ever registered. Missing registration made GetCVar throw. shadowTextureSize is the live
// knob: it stores the requested texel size and re-creates the shadow textures. shadowMode is a
// read-only mirror of the mode extShadowQuality picked, so the panel can display it without the
// ShadowQuality slider dropping us off the cascaded shadow maps (mode 5) that extShadowQuality selects.
bool CWorldParam::ShadowTextureSizeCallback(CVar* var, const char* oldValue, const char* value, void* arg) {
    int32_t size = SStrToInt(value);

    // 0 is the "let the quality tier decide" value, so it bypasses the range check on purpose.
    if (size != 0 && (size < static_cast<int32_t>(SHADOW_TEXTURE_SIZE_MIN) || size > static_cast<int32_t>(SHADOW_TEXTURE_SIZE_MAX))) {
        char buffer[128];
        sprintf(buffer, "Shadow texture size must be 0 (quality tier default) or %u-%u.", SHADOW_TEXTURE_SIZE_MIN, SHADOW_TEXTURE_SIZE_MAX);
        ConsoleWrite(buffer, DEFAULT_COLOR);
        return false;
    }

    CShadowCache::SetShadowTextureSize(static_cast<uint32_t>(size));

    char buffer[128];
    sprintf(buffer, "Shadow texture size set to %d.", size);
    ConsoleWrite(buffer, DEFAULT_COLOR);

    return true;
}

// Not in the original binary. Guards the mirror against its own write: SyncShadowModeCVar goes through
// CVar::Set, which runs ShadowModeCallback, which would otherwise call back into SyncShadowModeCVar.
static bool s_syncingShadowMode;

// Not in the original binary. Refuses every real write and re-asserts the live mode instead, so the panel's
// ShadowQuality slider cannot store a value that disagrees with the mode that actually took effect. This is
// reject-by-callback rather than the CVar read-only flag on purpose: the flag makes the Lua SetCVar binding
// raise "is read-only", and the panel calls SetCVar unconditionally. A false return is what discards the
// value, because CVar::Set early-outs when the callback returns false. Our own write passes because
// SyncShadowModeCVar raises the guard first; the registration call is harmless only because
// cvar_shadowMode is still null at that point, so the sync it triggers does nothing.
bool CWorldParam::ShadowModeCallback(CVar* var, const char* oldValue, const char* value, void* arg) {
    if (s_syncingShadowMode) {
        return true;
    }

    CWorldParam::SyncShadowModeCVar();

    return false;
}

// Not in the original binary. Mirrors the live shadow mode into the shadowMode CVar so the panel's
// ShadowQuality slider reports what is really in effect. Deliberately never sets the read-only flag, since
// that would make the panel's own SetCVar call raise an error; ShadowModeCallback is what refuses writes.
void CWorldParam::SyncShadowModeCVar() {
    CVar* var = CWorldParam::cvar_shadowMode;

    if (!var || s_syncingShadowMode) {
        return;
    }

    s_syncingShadowMode = true;

    // GetShadowMode() is not usable here: it answers SHADOW_LOWEST whenever texture recreation is pending,
    // and SetShadowMode/SetShadowTextureSize set that flag immediately before this runs. Displaying the
    // stored mode is the whole point of the mirror, so read it directly.
    char modeValue[16];
    SStrPrintf(modeValue, sizeof(modeValue), "%u", static_cast<uint32_t>(CShadowCache::GetShadowModeRaw()));

    var->Set(modeValue, true, false, false, false);

    s_syncingShadowMode = false;
}

// Not in the original binary. Turns the cascaded shadow maps (SHADOW_VERY_HIGH) into the tiled band path
// used by the tiers below them. The stored mode is left alone so the quality slider keeps reporting
// Cascaded; CShadowCache::GetEffectiveShadowMode() is what the renderer actually reads.
bool CWorldParam::CascadedShadowsCallback(CVar* var, const char* oldValue, const char* value, void* arg) {
    CShadowCache::s_cascadedShadows = SStrToInt(value) != 0;
    CShadowCache::NotifyTexturesRecreate();

    ConsoleWrite(CShadowCache::s_cascadedShadows ? "Cascaded shadows enabled." : "Cascaded shadows disabled, shadow bands fall back to the tiled path.", DEFAULT_COLOR);

    return true;
}

// Not in the original binary. "outer" pulls in bands 1 and 2 only: texels-per-unit is s_shadowSize /
// (2 * radius), so the 4x density step between bands is exactly the 4x ratio of the stock radii, and
// shrinking the outer radii shrinks the step. The cost is that band coverage ends closer than 640 units.
// "linear" scales all three, which moves the whole shadow draw distance but leaves the seam where it is.
bool CWorldParam::ShadowBandRangeCallback(CVar* var, const char* oldValue, const char* value, void* arg) {
    float scale[3] = { 1.0f, 1.0f, 1.0f };

    if (!SStrCmpI(value, "outer", STORM_MAX_STR)) {
        scale[1] = 0.875f;      // 160 -> 140, density step to band 0 drops from 4x to 3.5x
        scale[2] = 0.53125f;    // 640 -> 340, density step to band 1 drops from 4x to 2.43x
    } else if (!SStrCmpI(value, "linear", STORM_MAX_STR)) {
        scale[0] = scale[1] = scale[2] = 0.53125f;
    } else if (!SStrCmpI(value, "default", STORM_MAX_STR)) {
        // Registered default: the three scales stay 1.0, so Init() uses s_bandRadius unmodified.
    } else {
        uint32_t count = 0;
        const char* entry = value;

        // ':' separated, same shape as particleDensity. sscanf takes the leading number of each field
        // and leaves the cursor on the separator, so no tokenising copy is needed.
        for (; count < 3 && entry; count++) {
            float parsed = 0.0f;

            if (sscanf(entry, "%f", &parsed) != 1 || parsed <= 0.0f) {
                break;
            }

            scale[count] = parsed;
            entry = strchr(entry, ':');

            if (entry) {
                entry++;
            }
        }

        if (count != 3) {
            ConsoleWrite("shadowBandRange needs three positive ':' separated multipliers, or 'outer' / 'linear' / 'default'.", DEFAULT_COLOR);

            return false;
        }
    }

    CShadowCache::s_bandRadiusScale[0] = scale[0];
    CShadowCache::s_bandRadiusScale[1] = scale[1];
    CShadowCache::s_bandRadiusScale[2] = scale[2];
    CShadowCache::NotifyTexturesRecreate();

    char buffer[192];
    sprintf(buffer, "Shadow band radii scaled to %.4g / %.4g / %.4g of %g / %g / %g.", scale[0], scale[1], scale[2], CShadowCache::s_bandRadius[0], CShadowCache::s_bandRadius[1], CShadowCache::s_bandRadius[2]);
    ConsoleWrite(buffer, DEFAULT_COLOR);

    return true;
}

// VERIFY: 782f4c26 manual
// OFFSET: 0x0078DCB0
bool CWorldParam::ExtShadowQualityCallback(CVar* var, const char* oldValue, const char* value, void* arg) {
    uint32_t mode = SStrToInt(value);
    ConsoleWrite(CShadowCache::GetShadowModeDesc(mode), DEFAULT_COLOR);

    if (!CShadowCache::ValidateShadowMode(static_cast<EShadowMode>(mode))) {
        return false;
    }

    CShadowCache::SetShadowMode(static_cast<EShadowMode>(mode));
    CWorldParam::SyncShadowModeCVar();

    return true;
}

// VERIFY: 8b8fa7e3 auto
// OFFSET: 0x0078DCF0
bool CWorldParam::ProjectedTexturesCallback(CVar* var, const char* oldValue, const char* value, void* arg) {
    if (SStrToInt(value)) {
        ConsoleWrite("Projected textures enabled.", DEFAULT_COLOR);
        CWorld::enables2 |= 4;
    } else {
        ConsoleWrite("Projected textures disabled.", DEFAULT_COLOR);
        CWorld::enables2 &= ~4;
    }

    return true;
}

// VERIFY: c1590fd6 manual
// OFFSET: 0x0078DE60
bool CWorldParam::SpecularCallback(CVar* var, const char* oldValue, const char* value, void* arg) {
    if (!SStrToInt(value)) {
        ConsoleWrite("Specular disabled on restart.", DEFAULT_COLOR);
        CWorld::enables &= ~Enable_Specular;

        return true;
    }

    if (g_theGxDevicePtr->Caps().m_shaderTargets[GxSh_Pixel] <= 0) {
        ConsoleWrite("Specular not enabled.  Requires pixel shaders.", DEFAULT_COLOR);

        return false;
    }

    ConsoleWrite("Specular enabled on restart.", DEFAULT_COLOR);
    CWorld::enables |= Enable_Specular;

    return true;
}

// VERIFY: 677c9e7a manual
// OFFSET: 0x0078DED0
bool CWorldParam::MapObjLightLODCallback(CVar* var, const char* oldValue, const char* value, void* arg) {
    uint32_t lod = SStrToInt(value);

    if (lod > 2) {
        ConsoleWrite("MapObjLightLOD must be 0-2", DEFAULT_COLOR);

        return false;
    }

    s_mapObjLightLOD = lod;

    return true;
}

// VERIFY: b09da230 auto
// OFFSET: 0x0078DF00
bool CWorldParam::BaseMipCallback(CVar* var, const char* oldValue, const char* value, void* arg) {
    uint32_t level = SStrToInt(value);

    if (level == g_theGxDevicePtr->DeviceBaseMipLevel()) {
        return true;
    }

    if (level <= 1) {
        g_theGxDevicePtr->DeviceSetBaseMipLevel(level);
        TextureNotifyGxRestart();
        UpdateAllPortraits();
        World::s_texCacheDirty = 1;

        char buffer[256];
        SStrPrintf(buffer, sizeof(buffer), "BaseMip level changed to %d", level);
        ConsoleWrite(buffer, DEFAULT_COLOR);

        return true;
    }

    ConsoleWrite("BaseMip must be 0 or 1", DEFAULT_COLOR);

    return false;
}

// VERIFY: b0d9b0b5 manual
// OFFSET: 0x0078DF90
bool CWorldParam::BspCacheCallback(CVar* var, const char* oldValue, const char* value, void* arg) {
    if (SStrToInt(value)) {
        if (g_BspDigestCache) {
            ConsoleWrite("Enabling BSP node cache (already enabled, so clearing content.)", DEFAULT_COLOR);
            g_BspDigestCache->Reset();
        } else {
            ConsoleWrite("Enabling BSP node cache (first time - starting up)", DEFAULT_COLOR);
            void* mem = SMemAlloc(sizeof(CAaBspDigestCache), __FILE__, __LINE__, 0);
            g_BspDigestCache = mem ? new (mem) CAaBspDigestCache() : nullptr;
        }
    } else if (g_BspDigestCache) {
        ConsoleWrite("Disabling BSP node cache.", DEFAULT_COLOR);
        CAaBspDigestCache* cache = g_BspDigestCache;

        if (cache) {
            cache->Reset();
            delete cache;
        }

        g_BspDigestCache = nullptr;
    } else {
        ConsoleWrite("BSP node cache already disabled.", DEFAULT_COLOR);
    }

    return true;
}

// VERIFY: 6a5bcdbc manual
// OFFSET: 0x0078E070
bool CWorldParam::HWPCFCallback(CVar* var, const char* oldValue, const char* value, void* arg) {
    uint32_t enable = SStrToInt(value);
    uint32_t previous = CWorld::enables2;

    if (enable) {
        if (!g_theGxDevicePtr->Caps().m_rttFormat[12]) {
            ConsoleWrite("Hardware PCF not supported by this graphics card.", DEFAULT_COLOR);

            return false;
        }

        ConsoleWrite("Hardware PCF enabled.", DEFAULT_COLOR);
        CWorld::enables2 |= Enable_HwPcf;
    } else {
        ConsoleWrite("Hardware PCF disabled.", DEFAULT_COLOR);
        CWorld::enables2 &= ~Enable_HwPcf;
    }

    if (previous != CWorld::enables2) {
        CShaderEffect::SetUsePcfFiltering((CWorld::enables2 & Enable_HwPcf) != 0);
        CShadowCache::NotifyTexturesRecreate();
        CMap::InitializePCFShaders();
    }

    return true;
}

// VERIFY: 685d5b59 auto
// OFFSET: 0x0078E110
bool CWorldParam::GxTextureCacheSizeCallback(CVar* var, const char* oldValue, const char* value, void* arg) {
    uint32_t size = SStrToInt(value);

    if (size) {
        World::s_texCacheOverride = size << 20;
        g_theGxDevicePtr->TexSetCacheSize(size << 20);

        char buffer[256];
        sprintf(buffer, "Texture cache size set to %dMB.", size);
        ConsoleWrite(buffer, DEFAULT_COLOR);
    } else {
        World::s_texCacheOverride = 0;
        World::s_texCacheDirty = 1;
        ConsoleWrite("Texture cache size set to default.", DEFAULT_COLOR);
    }

    return true;
}

// VERIFY: 9acf82a1 auto
// OFFSET: 0x0078E400
void CWorldParam::Initialize() {
    CWorldParam::cvar_farClipOverride = CVar::Register("farClipOverride", "Override old world graphic settings", 1, "0", CWorldParam::FarClipOverrideCallback, CATEGORY_GRAPHICS, false, nullptr, false);
    CWorldParam::cvar_lod = CVar::Register("lod", "Video option: Toggle Lod", 1, "0", CWorldParam::LodCallback, CATEGORY_GRAPHICS, false, nullptr, false);
    CWorldParam::cvar_mapShadows = CVar::Register("mapShadows", "Video option: Toggle map shadows", 1, "1", CWorldParam::MapShadowsCallback, CATEGORY_GRAPHICS, false, nullptr, false);
    CWorldParam::cvar_maxLights = CVar::Register("MaxLights", "Max number of hardware lights", 1, "4", CWorldParam::MaxLightsCallback, CATEGORY_GRAPHICS, false, nullptr, false);
    CWorldParam::cvar_shadowLevel = CVar::Register("shadowLevel", "Terrain shadow map mip level", 1, "1", CWorldParam::ShadowLevelCallback, CATEGORY_GRAPHICS, false, nullptr, false);
    CWorldParam::cvar_texLodBias = CVar::Register("texLodBias", "Texture LOD Bias", 1, "0.0", CWorldParam::TexLodBiasCallback, CATEGORY_GRAPHICS, false, nullptr, false);
    CWorldParam::cvar_farClip = CVar::Register("farclip", "Far clip plane distance", 1, "350", CWorldParam::FarClipCallback, CATEGORY_GRAPHICS, false, nullptr, false);
    CWorldParam::cvar_nearClip = CVar::Register("nearclip", "Near clip plane distance", 1, "0.2", CWorldParam::NearClipCallback, CATEGORY_GRAPHICS, false, nullptr, false);
    CWorldParam::cvar_specular = CVar::Register("specular", "Specularity", 1, "0", CWorldParam::SpecularCallback, CATEGORY_GRAPHICS, false, nullptr, false);
    CWorldParam::cvar_mapObjLightLOD = CVar::Register("mapObjLightLOD", "Map object light LOD", 1, "0", CWorldParam::MapObjLightLODCallback, CATEGORY_GRAPHICS, false, nullptr, false);
    CWorldParam::cvar_particleDensity = CVar::Register("particleDensity", "Video option: Particle density", 1, "1.0", CWorldParam::ParticleDensityCallback, CATEGORY_GRAPHICS, false, nullptr, false);
    CWorldParam::cvar_waterLOD = CVar::Register("waterLOD", "Water geometry LOD", 1, "0", CWorldParam::WaterLodCallback, CATEGORY_GRAPHICS, false, nullptr, false);
    CWorldParam::cvar_baseMip = CVar::Register("baseMip", "base mipmap level", 1, "0", CWorldParam::BaseMipCallback, CATEGORY_GRAPHICS, false, nullptr, false);
    CWorldParam::cvar_horizonFarClip = CVar::Register("horizonFarclipScale", "Far clip plane scale for horizon", 1, "4.0", CWorldParam::HorizonFarClipScaleCallback, CATEGORY_GRAPHICS, false, nullptr, false);
    CWorldParam::cvar_horizonNearClip = CVar::Register("horizonNearclipScale", "Near clip plane scale for horizon", 1, "0.7", CWorldParam::HorizonNearClipScaleCallback, CATEGORY_GRAPHICS, false, nullptr, false);
    CWorldParam::cvar_showFootprints = CVar::Register("showfootprints", "toggles rendering of footprints", 1, "1", CWorldParam::ShowFootprintsCallback, CATEGORY_GRAPHICS, false, nullptr, false);
    CWorldParam::cvar_bspCache = CVar::Register("bspcache", "BSP node caching", 1, "1", CWorldParam::BspCacheCallback, CATEGORY_GRAPHICS, false, nullptr, false);
    CWorldParam::cvar_footstepBias = CVar::Register("footstepBias", "Unit footstep depth bias", 1, "0.125", CWorldParam::FootstepBiasCallback, CATEGORY_GRAPHICS, false, nullptr, false);
    CWorldParam::cvar_occlusion = CVar::Register("occlusion", "Use hardware occlusion test", 1, "1", CWorldParam::HardwareOcclusionCallback, CATEGORY_GRAPHICS, false, nullptr, false);
    CWorldParam::cvar_worldPoolUsage = CVar::Register("worldPoolUsage", "CGxPool usage static/dynamic", 1, "Dynamic", CWorldParam::WorldPoolUsageCallback, CATEGORY_GRAPHICS, false, nullptr, false);
    CWorldParam::cvar_terrainAlphaBitDepth = CVar::Register("terrainAlphaBitDepth", "Terrain alpha map bit depth", 1, "8", CWorldParam::TerrainAlphaBitDepthCallback, CATEGORY_GRAPHICS, false, nullptr, false);
    CWorldParam::cvar_groundEffectDensity = CVar::Register("groundEffectDensity", "Ground effect density", 1, "16", CWorldParam::GroundEffectDensityCallback, CATEGORY_GRAPHICS, false, nullptr, false);
    CWorldParam::cvar_groundEffectDist = CVar::Register("groundEffectDist", "Ground effect dist", 1, "70.0", CWorldParam::GroundEffectDistCallback, CATEGORY_GRAPHICS, false, nullptr, false);
    CWorldParam::cvar_objectFade = CVar::Register("objectFade", "Fade objects into view", 1, "1", CWorldParam::ObjectFadeCallback, CATEGORY_GRAPHICS, false, nullptr, false);
    CWorldParam::cvar_objectFadeZFill = CVar::Register("objectFadeZFill", "Fade objects using ZFill pass ", 1, "0", CWorldParam::ObjectFadeZFillCallback, CATEGORY_GRAPHICS, false, nullptr, false);
    CWorldParam::cvar_environmentDetail = CVar::Register("environmentDetail", "Environment detail", 1, "1.0", CWorldParam::EnvironmentDetailCallback, CATEGORY_GRAPHICS, false, nullptr, false);
    CWorldParam::cvar_hwPCF = CVar::Register("hwPCF", "Hardware PCF Filtering", 1, "1", CWorldParam::HWPCFCallback, CATEGORY_GRAPHICS, false, nullptr, false);
    CWorldParam::cvar_extShadowQuality = CVar::Register("extShadowQuality", "Quality of exterior shadows (0-5)", 1, "0", CWorldParam::ExtShadowQualityCallback, CATEGORY_GRAPHICS, false, nullptr, false);

    // Not in the original binary. shadowTextureSize is the game's own name for the shadow map texel
    // count, matching Interface\FrameXML\VideoOptionsPanels.lua. Default 0 means "no explicit choice",
    // which leaves Init()'s quality-tier sizing in charge; registering a real size here would apply it
    // through the override before the tier logic ever runs and silently lose the 2048 default.
    CWorldParam::cvar_shadowTextureSize = CVar::Register("shadowTextureSize", "Shadow map resolution in texels", 1, "0", CWorldParam::ShadowTextureSizeCallback, CATEGORY_GRAPHICS, false, nullptr, false);

    // Not in the original binary. Default 1 keeps the cascaded shadow maps the mode-5 tier always had; the
    // Shadows panel checkbox writes it. Recreated textures are queued by its callback, so toggling it takes
    // effect without a restart. The shadowMode mirror deliberately still reports the stored mode, so the
    // quality slider keeps showing Cascaded while the bands run tiled.
    CWorldParam::cvar_cascadedShadows = CVar::Register("cascadedShadows", "Cascaded shadow maps at the Very High shadow quality tier", 1, "1", CWorldParam::CascadedShadowsCallback, CATEGORY_GRAPHICS, false, nullptr, false);

    // Not in the original binary. Default "default" leaves s_bandRadius untouched, so the stock tiers and
    // the 12340 constants stay intact for anyone who does not opt in.
    CWorldParam::cvar_shadowBandRange = CVar::Register("shadowBandRange", "Shadow band radii: three multipliers, or 'outer' / 'linear' / 'default'", 1, "default", CWorldParam::ShadowBandRangeCallback, CATEGORY_GRAPHICS, false, nullptr, false);

    // Not in the original binary. The unit outline's master switch, read by CUnitOutline::Enabled from the
    // render path. It belongs here with the other graphics cvars: the Advanced panel asks for its default
    // while the login screen is up, which is before any world frame exists, and GetCVarDefault raises on an
    // unregistered name. CUnitOutline::Init registers it too, so Init order cannot be got wrong.
    CWorldParam::cvar_unitOutline = CVar::Register("unitOutline", "Outline the mouseover and target units", 1, "1", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);

    // Not in the original binary. The anti-aliasing mode: 0 off, 1 FXAA. Unlike the unit outline this one
    // defaults to 0, so a client that never touches the options draws exactly what the stock 3.3.5a client
    // drew. CAntiAlias::Init registers it too, so Init order cannot be got wrong.
    CWorldParam::cvar_antiAlias = CVar::Register("antiAlias", "anti-aliasing mode: 0 off, 1 FXAA", 1, "0", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);

    // Not in the original binary. The FXAA quality preset, meaningful only while antiAlias is 1. Registered
    // here for the same reason as the mode. CAntiAlias::Init registers it too, and clamps it itself, so the
    // shader cannot be handed an index off the end of its table.
    CWorldParam::cvar_fxaaQuality = CVar::Register("fxaaQuality", "FXAA quality: 0 low, 1 medium, 2 high, 3 ultra", 1, "2", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);

    // Not in the original binary. The panel reads this, nothing may usefully write it - ShadowModeCallback
    // refuses writes, so it is read-only in effect without the CVar flag that would make Lua raise an error.
    // Marked unk=true so LookupRegistered accepts it. The registration default (mode 0) is not what the panel
    // should see, so it is seeded from the live shadow mode right here - by this point extShadowQuality has
    // already run its callback, because the cvar file is loaded before CVar::Initialize reaches this point.
    CWorldParam::cvar_shadowMode = CVar::Register("shadowMode", "Active shadow method (read-only; set extShadowQuality instead)", 1, "0", CWorldParam::ShadowModeCallback, CATEGORY_GRAPHICS, false, nullptr, true);
    CWorldParam::SyncShadowModeCVar();
    CWorldParam::cvar_projectedTextures = CVar::Register("projectedTextures", "Projected Textures", 1, "0", CWorldParam::ProjectedTexturesCallback, CATEGORY_GRAPHICS, false, nullptr, false);
    CWorldParam::cvar_gxTextureCacheSize = CVar::Register("gxTextureCacheSize", "GX Texture Cache Size", 1, "0", CWorldParam::GxTextureCacheSizeCallback, CATEGORY_GRAPHICS, false, nullptr, false);
    CVar::Register("spellEffectLevel", "Video Option: Spell Effects", 1, "9", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);

    AddConsoleDeviceDefaultCallback(CWorldParam::SetDefaults);

    if (ConsoleDeviceHardwareChanged()) {
        for (uint32_t page = OPTION_PAGE_0; page < NUM_OPTION_PAGES; page++) {
            CWorldParam::SetDefaults(static_cast<OPTION_PAGE>(page));
        }
    }

    CVar* videoOptionsVersion = CVar::Lookup("videoOptionsVersion");

    if (!videoOptionsVersion || videoOptionsVersion->m_intValue < 2) {
        CWorldParam::SetDefaultParticleDensity();
        CWorldParam::SetDefaultProjectedTextures();
    }
}
