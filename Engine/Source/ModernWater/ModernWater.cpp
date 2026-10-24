#include "Engine/Source/ModernWater/ModernWater.h"

#include "Engine/Source/ConsoleBase/ConsoleVar.h"
#include "Engine/Source/ConsoleBase/Types.h"
#include "Engine/Source/Gx/CGxDevice/CGxDevice.h"
#include "Engine/Source/ModernWater/log.h"
#include "Engine/Source/ModernWater/water_classify.h"
#include "Engine/Source/ModernWater/water_data.h"

#include "WoW/Source/DB/DBClient/DBClient.h"
#include "WoW/Source/DB/WowClientDB.h"
#include "WoW/Source/DB/Rec/LiquidTypeRec.h"
#include "WoW/Source/LiquidSystem/Liquid.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

// Set by Init. Null before the world frame exists, in which case WaterSettings uses the defaults.
CVar* s_cvarWaves;
CVar* s_cvarWind;
CVar* s_cvarReflections;
CVar* s_cvarSunGlow;
CVar* s_cvarSpecular;

// The liquid type a settings object belongs to. CSettingsBank stores one settings per liquid type at that type's
// own index, so the index is the type id -- no back-pointer needed and no layout guessed at. This is the whole of
// what the WXL module needed a guarded memory walk and a settings-bank signature check to recover.
uint32_t LiquidTypeOf(const Liquid::CMaterialSettings* settings) {
    TSGrowableArray<Liquid::CMaterialSettings*>& bank = Liquid::CSettingsBank::s_settings;

    for (uint32_t type = 0; type < bank.Count(); ++type) {
        if (bank[type] == settings) {
            return type;
        }
    }

    return 0;
}

// Classification is per settings object and the same handful of objects are reused by every liquid in the map, so
// the answer is remembered. Without this, every liquid draw in the frame would re-resolve the type record.
//
// The cache is keyed on the pointer and the type, because a pointer can be freed and the address reused by a
// settings object of a different type -- comparing the type as well makes a stale entry impossible to hit.
struct Classified {
    const void* settings;
    uint32_t type;
    WaterClass waterClass;
};

std::vector<Classified> s_classified;

constexpr WaterClass kSingleWaterClass = WaterClass::River;

WaterClass ClassifyUncached(const Liquid::CInstance* instance) {
    if (!instance || !instance->m_settings) {
        return WaterClass::None;
    }

    const uint32_t type = LiquidTypeOf(instance->m_settings);

    if (!type) {
        return WaterClass::None;
    }

    const LiquidTypeRec* record = g_liquidTypeDB.GetRecord(static_cast<int32_t>(type));

    if (!record) {
        return WaterClass::None;
    }

    // Only the water materials are shaded: still water (1) and the client's procedural water (3). Magma, slime and the
    // rest keep the client's own shader: they are not water, and the water model would turn them into a wrong lake
    // rather than a better lava. ClassifyLiquid makes the same cut; rejecting material 3 here as well left every
    // procedural-water liquid unshaded, which is a stretch of water that simply disappears from the modern pass.
    const uint32_t materialId = static_cast<uint32_t>(record->m_materialID);

    if (materialId != kLiquidMaterialWater && materialId != kLiquidMaterialProcWater) {
        return WaterClass::None;
    }

    // Fast water is the client's own name for a river, and it is the only distinction the liquid data makes that
    // matters to the shading: a river is narrower, faster and reads more turquoise than a lake.
    WaterClass waterClass = ClassifyLiquid(type, static_cast<uint32_t>(record->m_soundBank), materialId,
                                                instance->m_settings->m_textureNames[0]);

    // TEMPORARY experiment: every water is shaded as one class, so a frame is a single run and no two classes can fight
    // over an overlap. Set to WaterClass::None to restore the per-type classes (lake, river, ocean, interior).
    if (kSingleWaterClass != WaterClass::None && waterClass != WaterClass::None) {
        waterClass = kSingleWaterClass;
    }

    // One line per new liquid type, which is all it takes to see why a zone's water is not shaded: the type, its
    // material, the surface texture the classifier looked at, and the class it settled on. Without this a misclassified
    // liquid is invisible -- the pass simply never sees it.
    VF_LOG_INFO("modern water: liquid type %u, material %u, sound bank %u, surface '%s' -> %s", type, materialId,
                static_cast<unsigned>(record->m_soundBank), instance->m_settings->m_textureNames[0],
                WaterClassLabel(waterClass));

    return waterClass;
}

}  // namespace

namespace ModernWater {

// Not in the original binary. Registers the device-restored callback, so a device reset or a resolution change
// frees the pass's default-pool textures instead of leaving them dangling. Idempotent, because a world frame is
// built again after a UI reload.
void Init() {
    static bool s_registered;

    // Registering again returns the existing var, so a rebuilt world frame keeps what the player set.
    s_cvarWaves = CVar::Register("waterWaves", "modern water wave height: 0 flat, 0.7 default, higher is rougher", 1, "0.7", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
    s_cvarWind = CVar::Register("waterWind", "modern water wind speed, in yards per second: 3 default, higher is choppier", 1, "3", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);

    s_cvarReflections = CVar::Register("waterReflections", "modern water reflection strength: 0 sky only, 0.5 default, higher is more mirror-like", 1, "0.5", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
    s_cvarSunGlow = CVar::Register("waterSunGlow", "modern water sun reflection glow: 0 off, 5 default, higher is brighter", 1, "5", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);

    s_cvarSpecular = CVar::Register("waterSpecular", "modern water sun glint strength: 0 off, 0.15 default, higher is brighter", 1, "0.15", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);

    if (!s_registered) {
        s_registered = true;
        g_theGxDevicePtr->AddDeviceRestoredCallback([]() { GlobalWaterRenderer().ReleaseDefaultPool(); });
    }

    // The water data: zone palettes, wave spectra and the foam masks, converted from the modern client's water by the
    // WXL module's tools/convert_forever_water.py. It is read once, because it is 8 MB and nothing in it changes between
    // frames or maps. Without it the pass stands down and the client's own water stays, which is why the file ships in
    // the client's MPQ chain (patch-Z.mpq) rather than being optional.
    //
    // The path is bare so it resolves at the archive root of whichever MPQ carries it; Load falls back to a loose file
    // beside the executable for iterating without a repack.
    if (!GlobalWaterData().Loaded()) {
        GlobalWaterData().Load("waterdata.bin");
    }
}

WaterTimeInputs WaterSettings() {
    // The WXL module's wxl-modern-water.ini defaults, minus the switches that only existed because it was a DLL
    // (Enable, EngineHooks, Overlay, LogLevel) and minus WaterQuality, which is fixed at the medium preset the
    // file shipped with. Waves, wind, foam, reflections, specular, clarity and zone tint keep their documented
    // values so the water looks like the module's screenshot rather than like a default someone guessed.
    WaterTimeInputs settings;
    settings.quality = 2;
    settings.waves = s_cvarWaves ? std::max(s_cvarWaves->m_floatValue, 0.0f) : 0.7f;
    settings.wind = s_cvarWind ? std::max(s_cvarWind->m_floatValue, 0.0f) : 3.0f;
    settings.foam = 0.6f;
    settings.reflections = s_cvarReflections ? std::max(s_cvarReflections->m_floatValue, 0.0f) : 0.5f;
    settings.sunGlow = s_cvarSunGlow ? std::max(s_cvarSunGlow->m_floatValue, 0.0f) : 5.0f;
    settings.specular = s_cvarSpecular ? std::max(s_cvarSpecular->m_floatValue, 0.0f) : 0.15f;
    settings.clarity = 0.2f;
    settings.zoneColors = 0.7f;
    settings.ripples = 0.5f;
    return settings;
}

WaterClass ClassifyLiquidInstance(const Liquid::CInstance* instance) {
    if (!instance || !instance->m_settings) {
        return WaterClass::None;
    }

    const uint32_t type = LiquidTypeOf(instance->m_settings);

    for (const Classified& known : s_classified) {
        if (known.settings == instance->m_settings && known.type == type) {
            return known.waterClass;
        }
    }

    const WaterClass waterClass = ClassifyUncached(instance);
    s_classified.push_back({instance->m_settings, type, waterClass});

    if (!GlobalWaterData().Preset(waterClass) && waterClass != WaterClass::None) {
        // The class resolved but the data file has no preset for it, which means waterdata.bin is missing or from
        // a different build. Shading it anyway would multiply by zero absorption and draw a black lake, so this is
        // reported once and the class is dropped back to the client's material.
        VF_LOG_ERROR("modern water: the water data has no preset for %s; the client's water is used instead",
                     WaterClassLabel(waterClass));
        s_classified.back().waterClass = WaterClass::None;
        return WaterClass::None;
    }

    return waterClass;
}

// TEMPORARY: with WOWWATER_TRACE set, every 300th armed frame appends the frame's sorted liquid draw list to
// water-trace.txt beside the executable: draw order, liquid type, class, material and height.
void TraceLiquidDrawList(Liquid::CInstance* const* instances, uint32_t count) {
    static const bool enabled = std::getenv("WOWWATER_TRACE") != nullptr;
    static uint32_t frame = 0;

    if (!enabled || (frame++ % 300) != 0) {
        return;
    }

    FILE* file = std::fopen("water-trace.txt", "a");

    if (!file) {
        return;
    }

    std::fprintf(file, "--- frame %u, %u liquid instances\n", frame, count);

    for (uint32_t index = 0; index < count; ++index) {
        const Liquid::CInstance* instance = instances[index];
        const uint32_t type = LiquidTypeOf(instance->m_settings);
        const LiquidTypeRec* record = type ? g_liquidTypeDB.GetRecord(static_cast<int32_t>(type)) : nullptr;
        std::fprintf(file, "%3u type %2u material %d class %-8s height %.2f settings %p\n", index, type,
                     record ? static_cast<int>(record->m_materialID) : -1,
                     WaterClassLabel(ClassifyLiquidInstance(instance)), instance->m_transform.d3,
                     static_cast<const void*>(instance->m_settings));
    }

    std::fclose(file);
}

bool TagLiquidInstance(const Liquid::CInstance* instance) {
    return ClassifyLiquidInstance(instance) != WaterClass::None;
}

bool WaterPassArmed() {
    return GlobalWaterRenderer().Armed();
}

static bool s_clientRipplesSuppressed = false;

bool ClientRipplesSuppressed() {
    return s_clientRipplesSuppressed;
}

void SetClientRipplesSuppressed(bool suppressed) {
    s_clientRipplesSuppressed = suppressed;
}

}  // namespace ModernWater
