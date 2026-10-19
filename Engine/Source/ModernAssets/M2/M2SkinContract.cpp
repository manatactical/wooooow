#include "Engine/Source/ModernAssets/M2/M2SkinContract.h"

#include "Engine/Source/ModernAssets/ModernAssetsLog.h"

#include <cstring>
#include <vector>

// Ported from WarcraftXL wxl-modern-m2 compat/Skin.cpp (GPL-3.0-or-later, Copyright (C) 2026
// WarcraftXL): DecodeBlendBits, EnvSplit and DownConvertBatch.
//
// The batch reshape itself is theirs. What is different here is the surrounding ownership: their host
// owns the skin's batch array and can replace it wholesale, while this client's M2Init has already
// turned the header's arrays into pointers by the time a skin is attached. So this runs on the header
// BEFORE that walk (during the model repack), building the two lookup tables the client will read and
// appending them past the body.

namespace modern::m2
{
    namespace
    {
        // A texunit shaderId at or above this is a modern shader-effect index, not a packed blend pair.
        constexpr uint16_t kSourceShaderMin = 0x8000;

        constexpr uint32_t kOffTextures        = 0x50;
        constexpr uint32_t kOffMaterials       = 0x70;
        constexpr uint32_t kOffTexCombos       = 0x80;
        constexpr uint32_t kOffTexUnitLookup   = 0x88;
        constexpr uint32_t kOffTexWeightCombos = 0x90;
        constexpr uint32_t kOffTexTransformCombos = 0x98;
        constexpr uint32_t kOffTexCombinerCombos = 0x130;
        constexpr uint32_t kOffGlobalFlags     = 0x10;

        constexpr uint32_t kFlagUseTextureCombinerCombos = 0x8;

        constexpr uint32_t kBatchBytes = 0x18;
        constexpr uint16_t kMissing = 0xFFFF;

        inline uint32_t Rd32(const void* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }
        inline uint16_t Rd16(const void* p) { uint16_t v; std::memcpy(&v, p, 2); return v; }
        inline uint8_t  Rd8(const void* p) { uint8_t v; std::memcpy(&v, p, 1); return v; }
        inline void     Wr32(void* p, uint32_t v) { std::memcpy(p, &v, 4); }
        inline void     Wr16(void* p, uint16_t v) { std::memcpy(p, &v, 2); }
        inline void     Wr8(void* p, uint8_t v) { std::memcpy(p, &v, 1); }

        /// One batch's fields, in the client's order.
        struct Batch
        {
            uint8_t  flags;
            uint8_t  priorityPlane;
            uint16_t shaderId;
            uint16_t skinSectionIndex;
            uint16_t geosetIndex;
            uint16_t colorIndex;
            uint16_t materialIndex;
            uint16_t materialLayer;
            uint16_t textureCount;
            uint16_t textureComboIndex;
            uint16_t textureCoordComboIndex;
            uint16_t textureWeightComboIndex;
            uint16_t textureTransformComboIndex;
        };

        Batch ReadBatch(const uint8_t* p)
        {
            Batch b;
            b.flags = Rd8(p);
            b.priorityPlane = Rd8(p + 1);
            b.shaderId = Rd16(p + 2);
            b.skinSectionIndex = Rd16(p + 4);
            b.geosetIndex = Rd16(p + 6);
            b.colorIndex = Rd16(p + 8);
            b.materialIndex = Rd16(p + 10);
            b.materialLayer = Rd16(p + 12);
            b.textureCount = Rd16(p + 14);
            b.textureComboIndex = Rd16(p + 16);
            b.textureCoordComboIndex = Rd16(p + 18);
            b.textureWeightComboIndex = Rd16(p + 20);
            b.textureTransformComboIndex = Rd16(p + 22);
            return b;
        }

        void WriteBatch(uint8_t* p, const Batch& b)
        {
            Wr8(p, b.flags);
            Wr8(p + 1, b.priorityPlane);
            Wr16(p + 2, b.shaderId);
            Wr16(p + 4, b.skinSectionIndex);
            Wr16(p + 6, b.geosetIndex);
            Wr16(p + 8, b.colorIndex);
            Wr16(p + 10, b.materialIndex);
            Wr16(p + 12, b.materialLayer);
            Wr16(p + 14, b.textureCount);
            Wr16(p + 16, b.textureComboIndex);
            Wr16(p + 18, b.textureCoordComboIndex);
            Wr16(p + 20, b.textureWeightComboIndex);
            Wr16(p + 22, b.textureTransformComboIndex);
        }

        /// Finds an existing single-entry slot, or kMissing.
        uint16_t LookupSingle(const std::vector<int16_t>& lookup, int16_t v)
        {
            for (size_t n = 0; n < lookup.size(); ++n)
                if (lookup[n] == v) return static_cast<uint16_t>(n);
            return kMissing;
        }

        uint16_t SingleOrDefault(const std::vector<int16_t>& lookup, int16_t v)
        {
            const uint16_t found = LookupSingle(lookup, v);
            return found == kMissing ? 0 : found;
        }

        /// Finds a pair whose two adjacent entries equal a and b, or kMissing.
        uint16_t LookupPair(const std::vector<int16_t>& lookup, int16_t a, int16_t b)
        {
            for (size_t n = 0; n + 1 < lookup.size(); ++n)
                if (lookup[n] == a && lookup[n + 1] == b) return static_cast<uint16_t>(n);
            return kMissing;
        }

        /// Finds a (blend1, blend2) pair in textureCombinerCombos, or kMissing.
        uint16_t FindBlend(const std::vector<uint16_t>& combos, uint16_t b1, uint16_t b2)
        {
            for (size_t n = 0; n + 1 < combos.size(); n += 2)
                if (combos[n] == b1 && combos[n + 1] == b2) return static_cast<uint16_t>(n);
            return kMissing;
        }

        /**
         * @brief Builds the complete coordinate and combiner tables a modern pack can name.
         *
         * The skin's batches are rewritten later, after the model has been through the client's
         * pointer walk, so the tables have to be built here -- and they must already hold every entry
         * the later pass will ask for. Enumerating the small, fixed domain up front is what makes the
         * two passes agree: the skin pass only ever looks indices up, and can never invent one past
         * the end of what the model carries.
         */
        void BuildFullTables(std::vector<int16_t>& coordLookup, std::vector<uint16_t>& blendOverride)
        {
            coordLookup.clear();
            blendOverride.clear();

            // Every coordinate entry the decode below can ask for: the two singles, then the pairs.
            for (int16_t single : { int16_t(-1), int16_t(0) })
            {
                if (LookupSingle(coordLookup, single) == kMissing) coordLookup.push_back(single);
            }
            for (int16_t a : { int16_t(-1), int16_t(0) })
                for (int16_t b : { int16_t(-1), int16_t(0), int16_t(1) })
                    if (LookupPair(coordLookup, a, b) == kMissing)
                    {
                        coordLookup.push_back(a);
                        coordLookup.push_back(b);
                    }

            // Every blend pair a packed shaderId can express: both nibbles in 1..7.
            for (uint16_t b1 = 1; b1 <= 7; ++b1)
                for (uint16_t b2 = 1; b2 <= 7; ++b2)
                    if (FindBlend(blendOverride, b1, b2) == kMissing)
                    {
                        blendOverride.push_back(b1);
                        blendOverride.push_back(b2);
                    }
        }

        /// Decodes a packed blend-bit shaderId and synthesises the coordinate entries it implies.
        void DecodeBlendBits(Batch& b, uint16_t shaderId, uint16_t textureCount,
                             const std::vector<int16_t>& coordLookup,
                             const std::vector<uint16_t>& blendOverride)
        {
            const uint16_t blend1 = (shaderId >> 4) & 0x7;
            const uint16_t blend2 = shaderId & 0x7;

            bool twoTexture = textureCount > 1 && (shaderId & 0x4000) && blend1 != 0 && blend2 != 0;

            if (twoTexture)
            {
                const uint16_t blend = FindBlend(blendOverride, blend1, blend2);
                if (blend == kMissing)
                {
                    // The pair is outside the built domain; drawing it as single-texture is better
                    // than naming a combiner entry that does not exist.
                    twoTexture = false;
                    b.shaderId = 0;
                }
                else
                {
                    // NOTE: combiner index 0 is a real entry, so "no blend" must never be inferred
                    // from shaderId being zero -- that is what twoTexture is for.
                    b.shaderId = blend;
                }
            }
            else
            {
                b.shaderId = 0;
            }

            if (!twoTexture) textureCount = 1;

            b.flags &= 0x10;

            if (!twoTexture)
            {
                const int16_t t0 = (shaderId & 0x80) ? -1 : 0;
                const uint16_t single = LookupSingle(coordLookup, t0);
                b.textureCoordComboIndex = single == kMissing ? 0 : single;
            }
            else
            {
                int16_t t0, t1;
                if (shaderId & 0x80) { t0 = -1; t1 = (shaderId & 0x8) ? -1 : 0; }
                else { t0 = 0; t1 = (shaderId & 0x8) ? -1 : ((shaderId & 0x4000) ? 1 : 0); }
                const uint16_t pair = LookupPair(coordLookup, t0, t1);
                b.textureCoordComboIndex = pair == kMissing ? 0 : pair;
            }

            b.textureCount = textureCount < 2 ? textureCount : 2;
        }

        /// Maps one modern effect index onto the nearest contract the client actually implements.
        void MapModernEffect(Batch& b, uint16_t low, const std::vector<int16_t>& coordLookup,
                             const std::vector<uint16_t>& blendOverride)
        {
            switch (low)
            {
                // Diffuse-only ids: no combiner needed, one texture, the plain T1 coordinate set.
                case 5: case 8: case 10: case 12: case 16: case 23:
                    b.shaderId = 0;
                    b.textureCount = 1;
                    b.textureCoordComboIndex = SingleOrDefault(coordLookup, 0);
                    break;

                // The additive-overlay ids. These are the ones that carry a second, env-mapped
                // texture over the diffuse, and dropping them to "no shader" is precisely what makes
                // a modern model render as nothing at all.
                case 21: case 33: case 35:
                {
                    const uint16_t blend = FindBlend(blendOverride, 1, 4);
                    const uint16_t pair = LookupPair(coordLookup, 0, -1);
                    b.shaderId = blend == kMissing ? 0 : blend;
                    b.textureCount = blend == kMissing ? 1 : 2;
                    b.textureCoordComboIndex = pair == kMissing ? 0 : pair;
                    b.flags &= 0x10;
                    break;
                }

                default:
                    b.shaderId = 0x0010;
                    b.textureCount = 1;
                    b.textureCoordComboIndex = SingleOrDefault(coordLookup, 0);
                    break;
            }
        }
    }

    SkinContractResult RebuildSkinContract(uint8_t* body, uint32_t bodySize, uint32_t capacity,
                                           uint32_t& outBodySize)
    {
        SkinContractResult result;
        outBodySize = bodySize;

        // The skin's batches are not in the model body; they live in the .skin file, which the client
        // attaches later. So the model side of the contract is the two lookup tables the batches will
        // index. Building them here means the later batch rewrite only has to pick indices into them.
        //
        // A model that already ships textureCoordCombos has a stock contract and needs none of this.
        const uint32_t coordCount = Rd32(body + kOffTexUnitLookup);
        if (coordCount) return result;

        const uint32_t batchHint = Rd32(body + kOffTexCombos);
        if (!batchHint) return result;

        std::vector<int16_t> coordLookup;
        std::vector<uint16_t> blendOverride;
        BuildFullTables(coordLookup, blendOverride);

        const size_t coordBytes = coordLookup.size() * sizeof(int16_t);
        const size_t blendBytes = blendOverride.size() * sizeof(uint16_t);
        if (static_cast<uint64_t>(bodySize) + coordBytes + blendBytes > capacity)
        {
            MODERN_ASSETS_LOG_WARN("m2: no room for the skin contract tables (%zu + %zu byte(s))",
                                   coordBytes, blendBytes);
            return result;
        }

        while (bodySize & 3u) body[bodySize++] = 0;
        const uint32_t coordAt = bodySize;
        std::memcpy(body + coordAt, coordLookup.data(), coordBytes);
        bodySize += static_cast<uint32_t>(coordBytes);

        while (bodySize & 3u) body[bodySize++] = 0;
        const uint32_t blendAt = bodySize;
        std::memcpy(body + blendAt, blendOverride.data(), blendBytes);
        bodySize += static_cast<uint32_t>(blendBytes);

        Wr32(body + kOffTexUnitLookup, static_cast<uint32_t>(coordLookup.size()));
        Wr32(body + kOffTexUnitLookup + 4, coordAt);

        Wr32(body + kOffTexCombinerCombos, static_cast<uint32_t>(blendOverride.size()));
        Wr32(body + kOffTexCombinerCombos + 4, blendAt);
        Wr32(body + kOffGlobalFlags, Rd32(body + kOffGlobalFlags) | kFlagUseTextureCombinerCombos);

        result.coordCombosBuilt = static_cast<uint32_t>(coordLookup.size());
        result.combinerCombosBuilt = static_cast<uint32_t>(blendOverride.size());
        outBodySize = bodySize;
        return result;
    }

    SkinContractResult RebuildSkinBatches(uint8_t* skin, uint32_t skinSize)
    {
        SkinContractResult result;
        if (!skin || skinSize < 0x2C) return result;

        // The skin's own header: magic, then five count+offset arrays, then boneCountMax.
        constexpr uint32_t kOffSkinVertices = 0x04;
        constexpr uint32_t kOffSkinIndices  = 0x0C;
        constexpr uint32_t kOffSkinBones    = 0x14;
        constexpr uint32_t kOffSkinSections = 0x1C;
        constexpr uint32_t kOffSkinBatches  = 0x24;

        const uint32_t batchCount = Rd32(skin + kOffSkinBatches);
        const uint32_t batchOffset = Rd32(skin + kOffSkinBatches + 4);
        if (!batchCount || !batchOffset) return result;
        if (static_cast<uint64_t>(batchOffset) + static_cast<uint64_t>(batchCount) * kBatchBytes > skinSize)
            return result;

        // Guard on the DATA, not only on the model's flag: a stock skin already carries the client's
        // own encoding, where the low bits of a shaderId are a shader variant and NOT a blend pair.
        // Re-deriving them turns ordinary armour into something reflective-looking. A modern pack is
        // recognisable by its packed effect indices, so a skin with none of those is left alone.
        bool looksModern = false;
        for (uint32_t i = 0; i < batchCount && !looksModern; ++i)
        {
            const uint8_t* at = skin + batchOffset + i * kBatchBytes;
            if (Rd16(at + 2) >= kSourceShaderMin) looksModern = true;
        }
        if (!looksModern) return result;

        // The model's synthesised tables are read through the header, which by this point still holds
        // body-relative offsets (the model was repacked before any skin was requested). Rebuilding the
        // lookups here keeps the two halves of the contract derived from one place.
        // The exact same tables the model pass wrote, built deterministically so the indices this
        // pass looks up are the ones the model actually carries.
        std::vector<int16_t> coordLookup;
        std::vector<uint16_t> blendOverride;
        BuildFullTables(coordLookup, blendOverride);

        for (uint32_t i = 0; i < batchCount; ++i)
        {
            uint8_t* at = skin + batchOffset + i * kBatchBytes;
            Batch batch = ReadBatch(at);
            const uint16_t shaderId = batch.shaderId;

            batch.flags &= 0x10;

            if (shaderId >= kSourceShaderMin)
            {
                const uint16_t sourceIndex = shaderId & 0x7FFF;

                // Source effect indices 0..2 are the Diffuse_T1_Env effects the client renders
                // natively, re-based so index 0 keeps meaning "no shader".
                if (sourceIndex <= 2)
                {
                    batch.shaderId = static_cast<uint16_t>(kSourceShaderMin | (sourceIndex + 1));
                    batch.textureCount = 2;
                    {
                        const uint16_t pair = LookupPair(coordLookup, 0, -1);
                        batch.textureCoordComboIndex = pair == kMissing ? 0 : pair;
                    }
                }
                else
                {
                    MapModernEffect(batch, shaderId & 0xFF, coordLookup, blendOverride);
                }
            }
            else
            {
                DecodeBlendBits(batch, shaderId, batch.textureCount, coordLookup, blendOverride);
            }

            // A batch on a submesh with no geometry cannot draw; it must not name a shader either, or
            // the client builds an effect for nothing.
            if (!batch.textureCount)
            {
                batch.shaderId = 0;
                ++result.batchesParked;
            }

            WriteBatch(at, batch);
            ++result.batchesRewritten;

            // Named in the log so the encoding can be checked rather than assumed: a batch whose
            // effect is unresolvable is exactly the "model loads but draws nothing" symptom.
            MODERN_ASSETS_LOG_INFO("m2:   batch %u: 0x%04x -> shader=0x%04x textures=%u coord=%u layer=%u",
                                   i, shaderId, batch.shaderId, batch.textureCount,
                                   batch.textureCoordComboIndex, batch.materialLayer);
        }

        // The lookup tables the batches now index were synthesised into the model body by
        // RebuildSkinContract; the counts must agree with what was actually emitted here.
        result.coordCombosBuilt = static_cast<uint32_t>(coordLookup.size());
        result.combinerCombosBuilt = static_cast<uint32_t>(blendOverride.size());
        return result;
    }
}
