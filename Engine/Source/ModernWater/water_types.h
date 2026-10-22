#ifndef ENGINE_SOURCE_MODERNWATER_WATER_TYPES_H
#define ENGINE_SOURCE_MODERNWATER_WATER_TYPES_H

#include <cstdint>

// Not in the original 3.3.5a binary. The water classes the modern pass shades separately, and the layout of
// one entry in waterdata.bin.
//
// A class is not a liquid type id: the client draws a lake, a river and the sea through the same procedural
// material, so the class is recovered from the liquid's own properties (see water_classify.h) and then carried
// to the shading pass as a stencil reference. One shading pass per class, because absorption and scattering are
// per class and a ps_3_0 shader cannot index a constant table.
//
// The struct layouts below are the on-disk format of waterdata.bin, produced by the WXL module's
// tools/convert_forever_water.py. Do not add fields: water_data.cpp static_asserts the sizes and the loader
// rejects a file whose record sizes differ, so a new field would make the shipped data file unloadable. Magma,
// which has no preset in that data, is configured in code (see the magma preset in water_renderer.cpp) instead.

enum class WaterClass : uint8_t {
    None = 0,
    Lake = 1,
    River = 2,
    Ocean = 3,
    Interior = 4,
    Magma = 5,
};

constexpr int kWaterClassCount = 6;
constexpr int kWaterPresetTiles = 4;
constexpr int kWaterMaxTiles = 8;
constexpr int kWaterMaskSlots = 6;
constexpr int32_t kWaterNoIndex = -1;

// The class mask the renderer reports and the stencil reference it writes are both a single 32-bit word, so neither
// count can exceed it. assert rather than comment, because the overflow would be silent.
static_assert(kWaterClassCount <= 32, "the water class mask is one 32-bit word");
static_assert(kWaterMaxTiles <= 32, "the drawn-tile mask is one 32-bit word");

// The Forever data's own liquid ids, kept so a preset can be looked up by id and so the converter's output can
// be checked against what this build expects.
constexpr uint32_t kForeverGenericLake = 1240;
constexpr uint32_t kForeverGenericOcean = 1250;
constexpr uint32_t kForeverGenericRiver = 1288;
constexpr uint32_t kForeverWmoInterior = 1290;

enum class WaterMaskSlot : int {
    HighFoam = 0,
    MidFoam = 1,
    LowFoam = 2,
    ShoreFoam = 3,
    DepthFoam = 4,
    RiverFoam = 5,
};

struct WaterFftTile {
    uint32_t foreverTileId;
    float size;
    float amplitude;
    float windMultiplier;
    float windAlignment;
    float foam[3];
    float oxygen[3];
};

struct WaterPreset {
    uint32_t foreverLiquidId;
    float absorption[4];
    float scatteringIntensities[4];
    float scatteringTop[4];
    float scatteringBottom[4];
    float depthFadeFoam[4];
    float shoreFoam[4];
    float waveFoam[4];
    float waveFoamScaling[4];
    float flow[4];
    float roughness[4];
    int32_t tiles[kWaterPresetTiles];
    int32_t masks[kWaterMaskSlots];
};

struct WaterMask {
    uint32_t foreverFileDataId;
    uint32_t size;
    uint32_t mipCount;
    float tintLow[3];
    float tintHigh[3];
};

// Interior water, ocean water and lake water are separate classes here, because the shipped waterdata.bin carries a
// separate preset for each (lake 1240, river 1288, ocean 1250, WMO interior 1290) with its own absorption, colours,
// wave tiles and foam masks. The WXL module folded ocean and interior into lake, which threw those two presets away
// and made an indoor pool look like a lake. Each class is looked up by its own id.
//
// Magma has no id here because it has no entry in the data file at all: lava is configured in code (see the magma
// preset in water_renderer.cpp), since it is not water and its numbers are not in the modern client's water data.
inline uint32_t ForeverLiquidOf(WaterClass waterClass) {
    switch (waterClass) {
        case WaterClass::Lake:
            return kForeverGenericLake;
        case WaterClass::River:
            return kForeverGenericRiver;
        case WaterClass::Ocean:
            return kForeverGenericOcean;
        case WaterClass::Interior:
            return kForeverWmoInterior;
        default:
            return 0;
    }
}

#endif
