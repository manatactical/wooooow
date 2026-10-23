#include "Engine/Source/ModernWater/ModernWater.h"

#include "Engine/Source/Gx/CGxDevice/CGxDevice.h"
#include "Engine/Source/ModernWater/log.h"
#include "Engine/Source/ModernWater/water_classify.h"
#include "Engine/Source/ModernWater/water_data.h"

#include "WoW/Source/DB/DBClient/DBClient.h"
#include "WoW/Source/DB/WowClientDB.h"
#include "WoW/Source/DB/Rec/LiquidTypeRec.h"
#include "WoW/Source/LiquidSystem/Liquid.h"

#include <cstring>
#include <vector>

namespace {

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

    // Only the water material is shaded. Magma, slime and the rest keep the client's own shader: they are not
    // water, and the water model would turn them into a wrong lake rather than a better lava.
    const uint32_t materialId = static_cast<uint32_t>(record->m_materialID);

    if (materialId != kLiquidMaterialWater) {
        return WaterClass::None;
    }

    // Fast water is the client's own name for a river, and it is the only distinction the liquid data makes that
    // matters to the shading: a river is narrower, faster and reads more turquoise than a lake.
    const WaterClass waterClass = ClassifyLiquid(type, static_cast<uint32_t>(record->m_soundBank), materialId,
                                                instance->m_settings->m_textureNames[0]);

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
    settings.waves = 0.7f;
    settings.wind = 3.0f;
    settings.foam = 1.0f;
    settings.reflections = 0.75f;
    settings.specular = 1.0f;
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
