#ifndef ENGINE_SOURCE_MODERNASSETS_M2_M2SKINCONTRACT_H
#define ENGINE_SOURCE_MODERNASSETS_M2_M2SKINCONTRACT_H

#include <cstdint>

// Rebuilds the material / texture-unit contract a modern skin omits.
//
// A modern skin encodes each batch's material in its own shaderId and leaves the model's
// textureCoordCombos array EMPTY -- the coordinate set a texture unit samples is packed into the
// shaderId instead of looked up. The client's effect builder does the opposite: it reads
// textureCoordCombos[batch.textureCoordComboIndex] to decide which vertex shader to use, and its
// combiner path reads textureCombinerCombos[batch.shaderId] as a base index.
//
// Hand a client an empty textureCoordCombos and every batch indexes entry 0 of nothing; hand it a
// packed modern shaderId and the combiner path reads a wild offset. The visible result is a model that
// loads, carries batches, and draws nothing.
//
// This runs at skin-finalize time, where the header and the pointer-fixed skin are both live, and
// rewrites each batch into the shape the client's own effect builder understands:
//   - a packed blend-bit shaderId (below 0x8000) has its blend pair and coordinate set decoded
//   - a modern effect index (0x8000 and up) is mapped onto the nearest contract the client implements
//   - textureCoordCombos / textureCombinerCombos are synthesised to hold what the batches now point at
//
// Ported from WarcraftXL wxl-modern-m2 compat/Skin.cpp (GPL-3.0-or-later, Copyright (C) 2026
// WarcraftXL).
namespace modern::m2
{
    struct SkinContractResult
    {
        uint32_t batchesRewritten = 0;
        uint32_t coordCombosBuilt = 0;
        uint32_t combinerCombosBuilt = 0;
        /// Batches whose pack could not be expressed and were parked (zero geometry) instead.
        uint32_t batchesParked = 0;
    };

    /**
     * @brief Rewrites a modern skin's batches and synthesises the lookup arrays the client reads.
     *
     * The model header's array storage is replaced with freshly allocated arrays, because the new
     * tables are larger than the empty ones the file shipped. The caller must be at a point where the
     * header's arrays are still body-relative offsets and no consumer has read them yet.
     *
     * @param body        the whole model body (the header's arrays point inside it).
     * @param bodySize    body byte size.
     * @param capacity    writable bytes at body; the synthesised tables are appended past the body.
     * @param outBodySize receives the new body size.
     * @return how much was rebuilt.
     */
    SkinContractResult RebuildSkinContract(uint8_t* body, uint32_t bodySize, uint32_t capacity,
                                          uint32_t& outBodySize);

    /**
     * @brief Rewrites a modern skin's batches, in place, as raw file data.
     *
     * Must run on the skin's bytes before the client walks them: once M2Init has turned the batch
     * array into a pointer, the records are no longer addressable as file data. Only a model that went
     * through the modern repack has batches in this encoding; a stock skin must not be passed here.
     *
     * @param skin      the whole skin file, writable.
     * @param skinSize  its byte size.
     */
    SkinContractResult RebuildSkinBatches(uint8_t* skin, uint32_t skinSize);
}

#endif
