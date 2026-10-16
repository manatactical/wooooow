#ifndef WOW_SOURCE_WORLDCLIENT_WORLDPARAM_H
#define WOW_SOURCE_WORLDCLIENT_WORLDPARAM_H

#include <cstdint>
#include "WoW/Source/ConsoleDevice/ConsoleDevice.h"

struct CVar;

class CWorldParam {
    public:
    static CVar* cvar_maxLights;
    static CVar* cvar_lod;
    static CVar* cvar_mapShadows;
    static CVar* cvar_shadowLevel;
    static CVar* cvar_texLodBias;
    static CVar* cvar_farClip;
    static CVar* cvar_nearClip;
    static CVar* cvar_specular;
    static CVar* cvar_mapObjLightLOD;
    static CVar* cvar_particleDensity;
    static CVar* cvar_waterLOD;
    static CVar* cvar_baseMip;
    static CVar* cvar_horizonFarClip;
    static CVar* cvar_horizonNearClip;
    static CVar* cvar_showFootprints;
    static CVar* cvar_bspCache;
    static CVar* cvar_footstepBias;
    static CVar* cvar_occlusion;
    static CVar* cvar_worldPoolUsage;
    static CVar* cvar_groundEffectDensity;
    static CVar* cvar_groundEffectDist;
    static CVar* cvar_terrainAlphaBitDepth;
    static CVar* cvar_farClipOverride;
    static CVar* cvar_objectFade;
    static CVar* cvar_objectFadeZFill;
    static CVar* cvar_environmentDetail;
    static CVar* cvar_hwPCF;
    static CVar* cvar_extShadowQuality;

    // Not in the original binary. See ShadowTextureSizeCallback / ShadowModeCallback.
    static CVar* cvar_shadowTextureSize;
    static CVar* cvar_shadowMode;
    static bool ShadowTextureSizeCallback(CVar* var, const char* oldValue, const char* value, void* arg);
    static bool ShadowModeCallback(CVar* var, const char* oldValue, const char* value, void* arg);
    static void SyncShadowModeCVar();
    static CVar* cvar_projectedTextures;
    static CVar* cvar_gxTextureCacheSize;

    // Not in the original binary. Ported from 15595 (Cataclysm) water settings. 12340's original water cvars
    // do not exist here, so these are additive; 15595 replaced its own waterLOD with them. See the note on
    // CWorldParam::waterLOD above.
    static CVar* cvar_waterDetail;
    static CVar* cvar_rippleDetail;
    static CVar* cvar_reflectionMode;
    static CVar* cvar_reflectionDownscale;

    static bool BaseMipCallback(CVar* var, const char* oldValue, const char* value, void* arg);
    static bool BspCacheCallback(CVar* var, const char* oldValue, const char* value, void* arg);
    static bool CloudLODCallback(CVar* var, const char* oldValue, const char* value, void* arg);
    static void Destroy();
    static bool EnvironmentDetailCallback(CVar* var, const char* oldValue, const char* value, void* arg);
    static bool ExtShadowQualityCallback(CVar* var, const char* oldValue, const char* value, void* arg);
    static bool FarClipCallback(CVar* var, const char* oldValue, const char* value, void* arg);
    static bool FarClipOverrideCallback(CVar* var, const char* oldValue, const char* value, void* arg);
    static bool FootstepBiasCallback(CVar* var, const char* oldValue, const char* value, void* arg);
    static bool GroundEffectDensityCallback(CVar* var, const char* oldValue, const char* value, void* arg);
    static bool GroundEffectDistCallback(CVar* var, const char* oldValue, const char* value, void* arg);
    static bool GxTextureCacheSizeCallback(CVar* var, const char* oldValue, const char* value, void* arg);
    static bool HardwareOcclusionCallback(CVar* var, const char* oldValue, const char* value, void* arg);
    static bool HorizonFarClipScaleCallback(CVar* var, const char* oldValue, const char* value, void* arg);
    static bool HorizonNearClipScaleCallback(CVar* var, const char* oldValue, const char* value, void* arg);
    static bool HWPCFCallback(CVar* var, const char* oldValue, const char* value, void* arg);
    static void Initialize();
    static bool LodCallback(CVar* var, const char* oldValue, const char* value, void* arg);
    static bool MapObjLightLODCallback(CVar* var, const char* oldValue, const char* value, void* arg);
    static bool MapShadowsCallback(CVar* var, const char* oldValue, const char* value, void* arg);
    static bool MaxLightsCallback(CVar* var, const char* oldValue, const char* value, void* arg);
    static bool NearClipCallback(CVar* var, const char* oldValue, const char* value, void* arg);
    static bool ObjectFadeCallback(CVar* var, const char* oldValue, const char* value, void* arg);
    static bool ObjectFadeZFillCallback(CVar* var, const char* oldValue, const char* value, void* arg);
    static bool ParticleDensityCallback(CVar* var, const char* oldValue, const char* value, void* arg);
    static bool ProjectedTexturesCallback(CVar* var, const char* oldValue, const char* value, void* arg);
    static void SetDefaultParticleDensity();
    static void SetDefaultProjectedTextures();
    static void SetDefaults(OPTION_PAGE page);
    static bool ShadowLevelCallback(CVar* var, const char* oldValue, const char* value, void* arg);
    static bool ShowFootprintsCallback(CVar* var, const char* oldValue, const char* value, void* arg);
    static bool SpecularCallback(CVar* var, const char* oldValue, const char* value, void* arg);
    static bool TerrainAlphaBitDepthCallback(CVar* var, const char* oldValue, const char* value, void* arg);
    static bool TexLodBiasCallback(CVar* var, const char* oldValue, const char* value, void* arg);
    static bool TextureFilteringCallback(CVar* var, const char* oldValue, const char* value, void* arg);
    static bool WaterLodCallback(CVar* var, const char* oldValue, const char* value, void* arg);
    // Not in the original binary. Ported from 15595 (Cataclysm) water settings callbacks.
    static bool WaterDetailCallback(CVar* var, const char* oldValue, const char* value, void* arg);
    static bool RippleDetailCallback(CVar* var, const char* oldValue, const char* value, void* arg);
    static bool ReflectionModeCallback(CVar* var, const char* oldValue, const char* value, void* arg);
    static bool ReflectionDownscaleCallback(CVar* var, const char* oldValue, const char* value, void* arg);
    static bool WorldPoolUsageCallback(CVar* var, const char* oldValue, const char* value, void* arg);
};

#endif
