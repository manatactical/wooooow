#include "Engine/Source/ModernAssets/Db2/LightTables.h"

#include "Engine/Source/ModernAssets/ModernAssetsLog.h"

#include <cstring>
#include <mutex>

// Ported from WarcraftXL wxl-db2 schemas/LightStore.cpp (GPL-3.0-or-later, Copyright (C) 2026
// WarcraftXL).
//
// EVERY COLUMN IS DECLARED, including the ones nothing reads yet: the reader cross-checks the declared
// field count against the file, and dropping a column would silently shift every field after it.

namespace modern::db2::light
{
    namespace
    {
        constexpr Field kLightFields[] = {
            { "GameCoords", 3 }, { "GameFalloffStart" }, { "GameFalloffEnd" },
            { "ContinentID" }, { "LightParamsID", 8 },
        };

        constexpr Field kLightDataFields[] = {
            { "LightParamID" },   // inline lookup column in this build (the file says so, not a CSV)
            { "Time" },
            { "DirectColor" }, { "AmbientColor" },
            { "SkyTopColor" }, { "SkyMiddleColor" }, { "SkyBand1Color" }, { "SkyBand2Color" },
            { "SkySmogColor" }, { "SkyFogColor" },
            { "SunColor" }, { "CloudSunColor" }, { "CloudEmissiveColor" },
            { "CloudLayer1AmbientColor" }, { "CloudLayer2AmbientColor" },
            { "OceanCloseColor" }, { "OceanFarColor" }, { "RiverCloseColor" }, { "RiverFarColor" },
            { "ShadowOpacity" },
            { "FogEnd" }, { "FogScaler" }, { "FogDensity" },
            { "FogHeight" }, { "FogHeightScaler" }, { "FogHeightDensity" }, { "FogZScalar" },
            { "MainFogStartDist" }, { "MainFogEndDist" },
            { "SunFogAngle" }, { "CloudDensity" },
            { "ColorGradingFileDataID" }, { "DarkerColorGradingFileDataID" },
            { "HorizonAmbientColor" }, { "GroundAmbientColor" },
            { "EndFogColor" }, { "EndFogColorDistance" }, { "FogStartOffset" },
            { "SunFogColor" }, { "SunFogStrength" },
            { "FogHeightColor" }, { "EndFogHeightColor" },
            { "Field_10_0_0_44649_042" }, { "Field_12_0_0_63854_043" },
            { "FogHeightCoefficients", 4 }, { "MainFogCoefficients", 4 },
            { "HeightDensityFogCoeff", 4 },
        };

        constexpr Field kLightParamsFields[] = {
            { "OverrideCelestialSphere", 3 }, { "OverrideSunPosition", 3 },
            { "HighlightSky" }, { "LightSkyboxID" }, { "CloudTypeID" }, { "Glow" },
            { "WaterShallowAlpha" }, { "WaterDeepAlpha" }, { "OceanShallowAlpha" }, { "OceanDeepAlpha" },
            { "Flags" }, { "SsaoSettingsID" },
            { "SunPolar" }, { "SunAzimuth" }, { "SunAttenuationStart" }, { "SunAttenuationEnd" },
            { "Field_12_0_0_63534_016" }, { "Field_12_0_0_63534_017" }, { "Field_12_0_0_63534_018" },
            { "Field_12_0_0_63534_019" }, { "Field_12_0_0_63534_020" }, { "Field_12_0_0_63534_021" },
            { "Field_12_0_0_63534_022" }, { "Field_12_0_0_63534_023" }, { "Field_12_0_0_63534_024" },
            { "Field_12_0_0_63534_025" }, { "Field_12_0_0_63534_026" }, { "Field_12_0_0_63534_027" },
            { "Field_12_0_0_63534_028" }, { "Field_12_0_0_63534_029" },
            { "Field_12_0_1_65617_030" }, { "Field_12_0_1_65617_031" },
        };

        // LightData names its parameter set once per row; the relation says which field carries it so a
        // consumer can cross tables without knowing the column order.
        const Relation kLightDataRelations[] = {
            { "LightParamID", RelationSource::Field, "LightParamID", 0, "lightparams" },
        };

        Definition MakeLight()
        {
            return Definition{ "light", "light.db2", 0,
                               std::vector<Field>(std::begin(kLightFields), std::end(kLightFields)) };
        }

        Definition MakeLightData()
        {
            return Definition{ "lightdata", "lightdata.db2", 0,
                               std::vector<Field>(std::begin(kLightDataFields), std::end(kLightDataFields)),
                               std::vector<Relation>(std::begin(kLightDataRelations),
                                                     std::end(kLightDataRelations)) };
        }

        Definition MakeLightParams()
        {
            return Definition{ "lightparams", "lightparams.db2", 0,
                               std::vector<Field>(std::begin(kLightParamsFields),
                                                  std::end(kLightParamsFields)) };
        }

        StoreStatus g_status;
        Table       g_light;
        Table       g_data;
        Table       g_params;
        std::once_flag g_once;

        void LoadOnce()
        {
            const Definition lightDef  = MakeLight();
            const Definition dataDef   = MakeLightData();
            const Definition paramsDef = MakeLightParams();

            std::string error;
            if (!ValidateDefinitions({ lightDef, dataDef, paramsDef }, &error))
            {
                g_status.failed = true;
                g_status.error = error;
                MODERN_ASSETS_LOG_WARN("db2-light: %s", error.c_str());
                return;
            }

            bool any = false;

            if (g_light.Load(lightDef, &error)) { any = true; g_status.lights = static_cast<uint32_t>(g_light.Rows().size()); }
            else MODERN_ASSETS_LOG_INFO("db2-light: light.db2 not loaded (%s)", error.c_str());

            if (g_data.Load(dataDef, &error)) { any = true; g_status.data = static_cast<uint32_t>(g_data.Rows().size()); }
            else MODERN_ASSETS_LOG_INFO("db2-light: lightdata.db2 not loaded (%s)", error.c_str());

            if (g_params.Load(paramsDef, &error)) { any = true; g_status.params = static_cast<uint32_t>(g_params.Rows().size()); }
            else MODERN_ASSETS_LOG_INFO("db2-light: lightparams.db2 not loaded (%s)", error.c_str());

            g_status.loaded = any;
            g_status.failed = !any;
            if (any)
                MODERN_ASSETS_LOG_INFO("db2-light: loaded light=%u lightdata=%u lightparams=%u",
                                       g_status.lights, g_status.data, g_status.params);
        }

        void EnsureLoaded() { std::call_once(g_once, LoadOnce); }

        uint32_t U32(const Table& table, const wdc5::Row& row, const char* field, size_t element = 0)
        {
            return table.Value(row, field, element);
        }

        float F32(const Table& table, const wdc5::Row& row, const char* field, size_t element = 0)
        {
            // C++17: no std::bit_cast, and the column is a bit pattern rather than an integer.
            const uint32_t bits = table.Value(row, field, element);
            float value = 0.0f;
            std::memcpy(&value, &bits, sizeof(value));
            return value;
        }
    }

    const StoreStatus& Status()
    {
        EnsureLoaded();
        return g_status;
    }

    StoreStatus LoadFrom(const uint8_t* light, size_t lightSize,
                         const uint8_t* data, size_t dataSize,
                         const uint8_t* params, size_t paramsSize)
    {
        const Definition lightDef  = MakeLight();
        const Definition dataDef   = MakeLightData();
        const Definition paramsDef = MakeLightParams();

        StoreStatus status;
        std::string error;
        if (!ValidateDefinitions({ lightDef, dataDef, paramsDef }, &error))
        {
            status.failed = true;
            status.error = error;
            return status;
        }

        g_light = Table();
        g_data = Table();
        g_params = Table();

        bool any = false;
        if (light && g_light.LoadBytes(lightDef, light, lightSize, &error))
        { any = true; status.lights = static_cast<uint32_t>(g_light.Rows().size()); }
        else status.error = error;

        if (data && g_data.LoadBytes(dataDef, data, dataSize, &error))
        { any = true; status.data = static_cast<uint32_t>(g_data.Rows().size()); }
        else if (status.error.empty()) status.error = error;

        if (params && g_params.LoadBytes(paramsDef, params, paramsSize, &error))
        { any = true; status.params = static_cast<uint32_t>(g_params.Rows().size()); }
        else if (status.error.empty()) status.error = error;

        status.loaded = any;
        status.failed = !any;
        return status;
    }

    const Table* Lights() { EnsureLoaded(); return g_light.Loaded() ? &g_light : nullptr; }
    const Table* Data()   { EnsureLoaded(); return g_data.Loaded() ? &g_data : nullptr; }
    const Table* Params() { EnsureLoaded(); return g_params.Loaded() ? &g_params : nullptr; }

    bool FindLight(uint32_t lightId, LightPoint& out)
    {
        EnsureLoaded();
        if (!g_light.Loaded()) return false;
        const wdc5::Row* row = g_light.Find(lightId);
        if (!row) return false;

        out.lightId = row->id;
        for (size_t i = 0; i < 3; ++i) out.coords[i] = F32(g_light, *row, "GameCoords", i);
        out.falloffStart = F32(g_light, *row, "GameFalloffStart");
        out.falloffEnd   = F32(g_light, *row, "GameFalloffEnd");
        out.continentId  = U32(g_light, *row, "ContinentID");
        out.paramsId     = U32(g_light, *row, "LightParamsID", 0);
        return true;
    }

    bool FindParams(uint32_t paramsId, LightParamsRow& out)
    {
        EnsureLoaded();
        if (!g_params.Loaded()) return false;
        const wdc5::Row* row = g_params.Find(paramsId);
        if (!row) return false;

        out.paramsId = row->id;
        out.flags    = U32(g_params, *row, "Flags");
        out.sunPolar = F32(g_params, *row, "SunPolar");
        out.sunAzimuth = F32(g_params, *row, "SunAzimuth");
        return true;
    }

    bool RowsForParams(uint32_t paramsId, std::vector<const wdc5::Row*>& out)
    {
        EnsureLoaded();
        out.clear();
        if (!g_data.Loaded()) return false;

        // The table carries one row per parameter set per time stamp; the file's own parent link is
        // the authority, so this walks the relationship rather than trusting a colour's position.
        for (const wdc5::Row& row : g_data.Rows())
        {
            const uint32_t key = g_data.RelationKey(row, "LightParamID");
            if (key == paramsId) out.push_back(&row);
        }
        return !out.empty();
    }
}
