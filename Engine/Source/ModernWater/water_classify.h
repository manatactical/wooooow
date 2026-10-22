#pragma once

#include "Engine/Source/ModernWater/water_types.h"

#include <cstdint>

constexpr uint32_t kLiquidMaterialWater = 1;
// The client's procedural (animated) water material. Material 1 is the still-water variant; most outdoor liquid in a
// normal zone is drawn through this one, so a classifier that accepts only material 1 shades almost nothing.
constexpr uint32_t kLiquidMaterialProcWater = 3;
// Ocean sound-bank water is folded into the lake class (see ClassifyLiquid).
constexpr uint32_t kLiquidSoundBankOcean = 1;
constexpr uint32_t kLiquidTypeFastWater = 9;
// Liquid type 17 is WMO interior water. It is intentionally never surfaced as its
// own class; the classifier folds it into the lake/ocean result (see ClassifyLiquid).
constexpr uint32_t kLiquidTypeWmoWaterInterior = 17;
constexpr const char* kRiverTextureFolder = "\\river\\";
constexpr const char* kOceanTextureFolder = "\\ocean\\";

inline char AsciiLower(char c)
{
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

inline bool StartsWithIgnoringCase(const char* text, const char* prefix)
{
    for (; *prefix; ++text, ++prefix)
        if (AsciiLower(*text) != AsciiLower(*prefix))
            return false;
    return true;
}

inline bool ContainsIgnoringCase(const char* text, const char* part)
{
    if (!text)
        return false;
    for (; *text; ++text)
        if (StartsWithIgnoringCase(text, part))
            return true;
    return false;
}

// Which class a liquid belongs to.
//
// The material has to be a water material, not a hardcoded id: the liquid DB uses material 1 for still water and
// material 3 for the client's *procedural* water, and material 3 is what most outdoor lakes and rivers actually
// use. Accepting only material 1 (as an earlier version did) classified nearly every real zone as "not water", so
// the pass never saw anything to draw.
//
// The material id is the only reliable signal that this is water at all; the surface texture is a secondary hint.
// An earlier version also required the texture path to contain "\river\" or "\ocean\", which is true of the Forever
// data the WXL module shipped but not of ordinary 3.3.5a lake water -- that filter rejected the common case.
inline WaterClass ClassifyLiquid(uint32_t liquidTypeId, uint32_t soundBank, uint32_t materialId, const char* texture0)
{
    (void)soundBank;

    if (materialId != kLiquidMaterialWater && materialId != kLiquidMaterialProcWater)
        return WaterClass::None;

    // WMO interior water has no surface of its own, so it is decided by type before anything looks at a texture.
    if (liquidTypeId == kLiquidTypeWmoWaterInterior)
        return WaterClass::Interior;

    if (liquidTypeId == kLiquidTypeFastWater)
        return WaterClass::River;

    if (ContainsIgnoringCase(texture0, kOceanTextureFolder))
        return WaterClass::Ocean;

    if (ContainsIgnoringCase(texture0, kRiverTextureFolder))
        return WaterClass::River;

    return WaterClass::Lake;
}
inline const char* WaterClassLabel(WaterClass waterClass)
{
    switch (waterClass)
    {
    case WaterClass::Lake:
        return "lake";
    case WaterClass::River:
        return "river";
    case WaterClass::Ocean:
        return "ocean";
    case WaterClass::Interior:
        return "interior";
    default:
        return "none";
    }
}
