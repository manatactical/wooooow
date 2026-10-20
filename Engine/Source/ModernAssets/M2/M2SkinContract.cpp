#include "Engine/Source/ModernAssets/M2/M2SkinContract.h"

#include "Engine/Source/ModernAssets/ModernAssetsLog.h"

#include <algorithm>
#include <cstring>
#include <utility>
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
        constexpr uint32_t kSectionBytes = 0x30;
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

        /// The tables are a fixed function of the decoder above: no input varies between calls, so the
        /// build is done once and handed out. Rebuilding them per skin was measurable work on a path that
        /// runs inside a postload callback on the frame thread.
        const std::vector<int16_t>& FullCoordLookup()
        {
            static const std::vector<int16_t> table = [] {
                std::vector<int16_t> coordLookup;
                std::vector<uint16_t> unusedBlend;
                BuildFullTables(coordLookup, unusedBlend);
                return coordLookup;
            }();

            return table;
        }

        const std::vector<uint16_t>& FullBlendOverride()
        {
            static const std::vector<uint16_t> table = [] {
                std::vector<int16_t> unusedCoord;
                std::vector<uint16_t> blendOverride;
                BuildFullTables(unusedCoord, blendOverride);
                return blendOverride;
            }();

            return table;
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

        /**
         * @brief Clamps each submesh's per-draw bone count to the client's palette ceiling.
         *
         * The client's vertex shader holds the bone palette from register 31 at three registers per matrix,
         * so (256 - 31) / 3 = 75 bones per draw is a hard limit of the engine contract, not of the source
         * format. Modern content routinely declares far more: the Sporeglider skins declare boneCount=144 on
         * every submesh. Handing that to the client overruns the palette on every draw of that section --
         * the symptom is a body that tears and parts (the wings) that never appear, which is exactly what was
         * reported, and it is why splitting the batches alone did not help.
         *
         * The reference host solves the same problem more thoroughly (BoneBudget.hpp repartitions the
         * submesh into sub-sections, each with its own compact boneCombos slice). That needs the model's
         * boneCombos array, which this pass does not have, so the count is clamped here instead: the geometry
         * stays put and the draw stops overrunning. Clamping rather than splitting can under-pose vertices
         * whose influence index lands past the clamped window, so this is the smaller fix, not a complete one.
         *
         * @param skin      skin bytes, sections adjusted in place.
         * @param sectionAt offset of the first section record.
         * @param count     number of section records.
         */
        void ClampSectionBones(uint8_t* skin, uint32_t sectionAt, uint32_t count, SkinContractResult& result)
        {
            // (256 - 31) / 3: the client's vertex-shader bone palette, in matrices.
            constexpr uint16_t kMaxBonesPerDraw = 75;

            for (uint32_t i = 0; i < count; ++i)
            {
                uint8_t* at = skin + sectionAt + i * kSectionBytes;

                const uint16_t boneCount     = Rd16(at + 0x0C);
                const uint16_t boneInfluences = Rd16(at + 0x10);

                // A section with no geometry cannot draw, and the native finalize divides the instance
                // budget by every section's bone count, so it must never be zero.
                const uint16_t clamped = boneCount > kMaxBonesPerDraw ? kMaxBonesPerDraw
                                       : (boneCount ? boneCount : 1);

                if (clamped != boneCount)
                {
                    Wr16(at + 0x0C, clamped);
                    ++result.sectionsBoneClamped;
                }

                // A drawn section whose influence count is zero still needs one influence to build indices.
                if (!boneInfluences && Rd16(at + 0x0A))
                {
                    Wr16(at + 0x10, 1);
                }
            }
        }

        /**
         * @brief Splits one source env batch into a primary plus a follower 2nd render pass over the same
         *        geometry.
         *
         * This is the case the client cannot express any other way. A source batch on one of these effect
         * indices declares THREE textures -- body, glow and overlay -- while the client's effect builder only
         * ever considers one or two. Clamping (what this port used to do) silently drops the third layer,
         * which is exactly the reported symptom: a mount that loads, is solid, and is missing its wings.
         *
         * The reference host solves it by emitting the primary batch plus a follower that re-draws the same
         * skin section as a layered second pass, carrying material layer 1 and material index +1. That needs
         * no third texture unit: the client already draws a material's layers as separate passes.
         *
         * @param low           Low byte of the source shaderId selecting the split case.
         * @param shaderId      Full source shaderId.
         * @param primary       Batch to split (passed by value).
         * @param coordLookup   textureUnitLookup table to look indices up in.
         * @param blendOverride textureCombinerCombos table to look pair indices up in.
         * @param out           Receives the primary and follower batches.
         * @return true when this low code is an env split; false lets the caller fall through.
         */
        bool EnvSplit(uint16_t low, uint16_t shaderId, Batch primary,
                      const std::vector<int16_t>& coordLookup,
                      const std::vector<uint16_t>& blendOverride,
                      std::vector<Batch>& out)
        {
            Batch follower = primary;
            follower.materialIndex = static_cast<uint16_t>(primary.materialIndex + 1);
            follower.materialLayer = 1;
            follower.textureCount = 1;

            switch (low)
            {
                case 0: case 3: case 9: case 17: case 24:
                {
                    primary.textureCount = 2;
                    const uint16_t blend = FindBlend(blendOverride, 1, 4);
                    const uint16_t pair = LookupPair(coordLookup, 0, -1);
                    primary.shaderId = blend == kMissing ? 0 : blend;
                    primary.textureCoordComboIndex = pair == kMissing ? 0 : pair;
                    follower.shaderId = primary.shaderId;
                    follower.textureCoordComboIndex = primary.textureCoordComboIndex;
                    out.push_back(primary);
                    out.push_back(follower);
                    return true;
                }

                case 1: case 15:
                {
                    primary.textureCount = 1;
                    primary.shaderId = 0;
                    follower.shaderId = 0;
                    follower.textureComboIndex = static_cast<uint16_t>(primary.textureComboIndex + 1);

                    // The follower's own coordinate set: the reference walks one entry past the primary's
                    // pair. The table is built to a fixed domain, so the entry is known to exist; clamp to
                    // the last slot rather than reading past the end if the domain ever shrinks.
                    const int16_t t1 = (shaderId == 0x8001) ? -1 : 1;
                    const uint16_t pair = LookupPair(coordLookup, 0, t1);
                    primary.textureCoordComboIndex = pair == kMissing ? 0 : pair;
                    const uint16_t next = static_cast<uint16_t>(pair + 1);
                    follower.textureCoordComboIndex =
                        coordLookup.empty() ? primary.textureCoordComboIndex
                                            : (next < coordLookup.size() ? next : primary.textureCoordComboIndex);

                    out.push_back(primary);
                    out.push_back(follower);
                    return true;
                }

                case 2:
                {
                    const uint16_t blend = FindBlend(blendOverride, 1, 3);
                    const uint16_t pair = LookupPair(coordLookup, 0, -1);
                    primary.shaderId = blend == kMissing ? 0 : blend;
                    primary.textureCoordComboIndex = pair == kMissing ? 0 : pair;
                    follower.shaderId = primary.shaderId;
                    follower.textureCoordComboIndex = primary.textureCoordComboIndex;
                    out.push_back(primary);
                    out.push_back(follower);
                    return true;
                }

                default:
                    return false;
            }
        }

        /**
         * @brief Reshapes one source batch into one batch, or into a primary plus follower for an env split.
         *
         * Mirrors the reference host's DownConvertBatch: source effect indices 0..2 are the Diffuse_T1_Env
         * effects the client renders natively and must NOT be routed through the split heuristic; everything
         * else either splits, maps to a named client effect, or falls back to a plain diffuse batch.
         *
         * @param b             Source batch (passed by value).
         * @param piece         Receives the reshaped batch(es).
         * @param coordLookup   textureUnitLookup table to look indices up in.
         * @param blendOverride textureCombinerCombos table to look pair indices up in.
         * @param outIsAddAlpha Set true for the additive-overlay ids a patched combiner substitutes.
         */
        void DownConvertBatch(Batch b, std::vector<Batch>& piece,
                              const std::vector<int16_t>& coordLookup,
                              const std::vector<uint16_t>& blendOverride,
                              bool& outIsAddAlpha)
        {
            uint16_t shaderId = b.shaderId;
            uint16_t textureCount = b.textureCount;
            b.flags &= 0x10;

            if (shaderId >= kSourceShaderMin)
            {
                const uint16_t sourceIndex = shaderId & 0x7FFFu;

                // Source effect indices 0..2 are Diffuse_T1_Env effects the client renders natively,
                // re-based so index 0 keeps meaning "no shader". One two-texture batch, no split.
                if (sourceIndex <= 2)
                {
                    b.shaderId = static_cast<uint16_t>(kSourceShaderMin | (sourceIndex + 1));
                    b.textureCount = 2;
                    const uint16_t pair = LookupPair(coordLookup, 0, -1);
                    b.textureCoordComboIndex = pair == kMissing ? 0 : pair;
                    b.flags &= 0x10;
                    piece.push_back(b);
                    return;
                }

                const uint16_t low = shaderId & 0xFF;

                if (EnvSplit(low, shaderId, b, coordLookup, blendOverride, piece))
                    return;

                switch (low)
                {
                    case 5: case 8: case 10: case 12: case 16: case 23:
                        shaderId = 0;
                        textureCount = 1;
                        break;

                    case 21: case 33: case 35:
                        shaderId = 0x4011;
                        textureCount = 2;
                        outIsAddAlpha = true;
                        break;

                    default:
                        shaderId = 0x0010;
                        textureCount = 1;
                        break;
                }
            }

            if (shaderId < kSourceShaderMin)
                DecodeBlendBits(b, shaderId, textureCount, coordLookup, blendOverride);
            else
                b.textureCount = textureCount < 2 ? textureCount : 2;

            piece.push_back(b);
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

    namespace
    {
        /// The client's per-draw bone ceiling: the vertex shader holds the palette from register 31 at
        /// three registers per matrix, so (256 - 31) / 3 = 75.
        constexpr uint16_t kMaxBonesPerDraw = 75;

        /// The client's u16 ceiling for the skin arrays.
        constexpr uint32_t kSkinU16Max = 0xFFFF;

        /// One rebuilt section, kept as raw bytes so it can be written straight back.
        struct SplitSection
        {
            uint8_t m_record[kSectionBytes];
            uint16_t m_original;
        };

        /// The fully rebuilt skin geometry plus the section array that indexes it.
        struct SplitResult
        {
            std::vector<uint16_t> m_lookup;
            std::vector<uint16_t> m_indices;
            std::vector<uint8_t>  m_bones;
            std::vector<SplitSection> m_sections;
            std::vector<uint16_t> m_boneCombos;
        };

        /**
         * @brief Repartitions every submesh whose per-draw bone palette exceeds the client ceiling.
         *
         * This is the reference host's BoneBudget: a greedy triangle bin-packer. An over-budget submesh is cut
         * into runs of triangles whose union of used bones fits the ceiling; each run becomes its own
         * sub-section with a compact boneCombos slice, a deduplicated vertex block whose influence indices are
         * remapped into that slice, and a global-indexed triangle block.
         *
         * Clamping boneCount instead stops the palette overrun but leaves the influence indices pointing at
         * entries the clamped palette no longer loads, so vertices pose against the wrong bones and parts of
         * the mesh collapse or vanish. That is why clamping alone left the mount's wings missing.
         */
        bool SplitSubmeshes(const uint8_t* skin, uint32_t skinSize,
                            const uint16_t* boneCombos, uint32_t comboCount,
                            uint32_t sectionAt, uint32_t sectionCount, SplitResult& out)
        {
            // The caller hands the resolved array. It used to be read through the header as a body-relative
            // offset, but after M2Init an M2Array's m_offset is a delta from its OWN member address, not a
            // body offset -- so the bounds check below rejected every model and the split never ran.
            if (!boneCombos || !comboCount) return false;

            bool needed = false;
            for (uint32_t i = 0; i < sectionCount && !needed; ++i)
                if (Rd16(skin + sectionAt + i * kSectionBytes + 0x0C) > kMaxBonesPerDraw) needed = true;
            if (!needed) return false;

            const uint32_t lookupCount  = Rd32(skin + 0x04);
            const uint32_t lookupOffset = Rd32(skin + 0x08);
            const uint32_t indexCount   = Rd32(skin + 0x0C);
            const uint32_t indexOffset  = Rd32(skin + 0x10);
            const uint32_t boneCount    = Rd32(skin + 0x14);
            const uint32_t boneOffset   = Rd32(skin + 0x18);

            if (!lookupOffset || !indexOffset || !boneOffset) return false;
            if (static_cast<uint64_t>(lookupOffset) + static_cast<uint64_t>(lookupCount) * 2 > skinSize) return false;
            if (static_cast<uint64_t>(indexOffset) + static_cast<uint64_t>(indexCount) * 2 > skinSize) return false;
            if (static_cast<uint64_t>(boneOffset) + static_cast<uint64_t>(boneCount) * 4 > skinSize) return false;

            const uint16_t* lookup  = reinterpret_cast<const uint16_t*>(skin + lookupOffset);
            const uint16_t* indices = reinterpret_cast<const uint16_t*>(skin + indexOffset);
            const uint8_t*  bones   = skin + boneOffset;

            out.m_lookup.clear();
            out.m_indices.clear();
            out.m_bones.clear();
            out.m_sections.clear();
            out.m_boneCombos.clear();

            // The rebuilt boneCombos array starts as a copy of the source; each split slice appends.
            out.m_boneCombos.assign(boneCombos, boneCombos + comboCount);

            // Maps a vertex's influence slot onto the global bone id it names.
            auto globalBone = [&](uint16_t localVertex, uint32_t slot, uint16_t comboBase) -> uint16_t {
                const uint32_t ci = static_cast<uint32_t>(comboBase) + bones[localVertex * 4 + slot];
                return ci < comboCount ? boneCombos[ci] : uint16_t(0);
            };

            for (uint32_t si = 0; si < sectionCount; ++si)
            {
                const uint8_t* src = skin + sectionAt + si * kSectionBytes;

                uint8_t record[kSectionBytes];
                std::memcpy(record, src, kSectionBytes);

                const uint16_t vertexStart  = Rd16(record + 0x04);
                const uint16_t vertexCount  = Rd16(record + 0x06);
                const uint16_t indexStart   = Rd16(record + 0x08);
                const uint16_t indexCountS  = Rd16(record + 0x0A);
                const uint16_t comboBase    = Rd16(record + 0x0E);

                if (static_cast<uint32_t>(vertexStart) + vertexCount > lookupCount) return false;
                if (static_cast<uint32_t>(indexStart) + indexCountS > indexCount) return false;

                // Every section -- kept or split -- is re-emitted into the rebuilt arrays, because those
                // arrays start empty and a record's offsets are relative to them. Copying a kept section
                // verbatim is what left its indices pointing into whatever a previous section had emitted,
                // which is the triangles stretched across the screen.

                // Does this section fit the ceiling as one draw?
                std::vector<uint16_t> sectionBoneSet;
                for (uint32_t v = 0; v < vertexCount && sectionBoneSet.size() <= kMaxBonesPerDraw; ++v)
                {
                    const uint16_t local = static_cast<uint16_t>(vertexStart + v);
                    for (uint32_t j = 0; j < 4; ++j)
                    {
                        const uint16_t g = globalBone(local, j, comboBase);
                        if (std::find(sectionBoneSet.begin(), sectionBoneSet.end(), g) == sectionBoneSet.end())
                            sectionBoneSet.push_back(g);
                    }
                }

                auto emitSection = [&](uint32_t firstVertex, uint32_t endVertex,
                                       uint32_t firstIndex, uint32_t endIndex,
                                       const std::vector<uint16_t>& globals) -> bool
                {
                    if (out.m_lookup.size() > kSkinU16Max) return false;
                    const uint32_t vertStart = static_cast<uint32_t>(out.m_lookup.size());
                    if (out.m_indices.size() > kSkinU16Max) return false;
                    const uint32_t idxStart = static_cast<uint32_t>(out.m_indices.size());
                    if (out.m_boneCombos.size() > kSkinU16Max) return false;
                    const uint32_t comboAt = static_cast<uint32_t>(out.m_boneCombos.size());

                    for (uint16_t g : globals) out.m_boneCombos.push_back(g);

                    std::vector<uint16_t> remapFrom;
                    std::vector<uint16_t> remapTo;

                    // firstIndex/endIndex are ABSOLUTE offsets into the skin's index array; indexStart is the
                    // section's base and must not be added again.
                    for (uint32_t k = firstIndex; k < endIndex; ++k)
                    {
                        const uint16_t local = indices[k];
                        if (local < firstVertex || local >= endVertex) return false;

                        uint16_t mapped = 0xFFFF;
                        for (size_t e = 0; e < remapFrom.size(); ++e)
                            if (remapFrom[e] == local) { mapped = remapTo[e]; break; }

                        if (mapped == 0xFFFF)
                        {
                            if (out.m_lookup.size() > kSkinU16Max) return false;
                            mapped = static_cast<uint16_t>(out.m_lookup.size());
                            remapFrom.push_back(local);
                            remapTo.push_back(mapped);

                            out.m_lookup.push_back(lookup[local]);

                            for (uint32_t j = 0; j < 4; ++j)
                            {
                                const uint16_t g = globalBone(local, j, comboBase);
                                const size_t at = std::lower_bound(globals.begin(), globals.end(), g) - globals.begin();
                                const bool found = at < globals.size() && globals[at] == g;
                                out.m_bones.push_back(static_cast<uint8_t>(found ? at : 0));
                            }
                        }

                        out.m_indices.push_back(mapped);
                    }

                    const uint32_t vertCountOut = static_cast<uint32_t>(out.m_lookup.size()) - vertStart;
                    const uint32_t idxCountOut  = static_cast<uint32_t>(out.m_indices.size()) - idxStart;
                    if (vertCountOut > kSkinU16Max || idxCountOut > kSkinU16Max) return false;

                    uint8_t made[kSectionBytes];
                    std::memcpy(made, record, kSectionBytes);

                    Wr16(made + 0x04, static_cast<uint16_t>(vertStart));
                    Wr16(made + 0x06, static_cast<uint16_t>(vertCountOut));
                    Wr16(made + 0x08, static_cast<uint16_t>(idxStart));
                    Wr16(made + 0x0A, static_cast<uint16_t>(idxCountOut));
                    Wr16(made + 0x0C, static_cast<uint16_t>(globals.size()));
                    Wr16(made + 0x0E, static_cast<uint16_t>(comboAt));
                    Wr16(made + 0x10, 4);

                    SplitSection e;
                    std::memcpy(e.m_record, made, kSectionBytes);
                    e.m_original = static_cast<uint16_t>(si);
                    out.m_sections.push_back(e);

                    return true;
                };

                // --- under budget: one draw over the section's whole triangle range ---
                if (sectionBoneSet.size() <= kMaxBonesPerDraw)
                {
                    std::vector<uint16_t> globals = sectionBoneSet;
                    std::sort(globals.begin(), globals.end());

                    if (!emitSection(vertexStart, vertexStart + vertexCount,
                                     indexStart, indexStart + indexCountS, globals))
                        return false;

                    continue;
                }

                // --- over budget: bin-pack triangles into runs whose bone union fits ---
                const uint32_t triCount = indexCountS / 3;
                if (!triCount) return false;

                struct Run
                {
                    uint32_t m_from;
                    uint32_t m_to;
                    std::vector<uint16_t> m_bones;
                };

                std::vector<Run> runs;
                runs.push_back(Run{ 0, 0, {} });

                std::vector<uint16_t> triBones(12);

                for (uint32_t t = 0; t < triCount; ++t)
                {
                    uint32_t n = 0;

                    for (uint32_t k = 0; k < 3; ++k)
                    {
                        const uint16_t local = indices[indexStart + t * 3 + k];
                        if (local < vertexStart || local >= vertexStart + vertexCount) return false;

                        for (uint32_t j = 0; j < 4; ++j)
                        {
                            const uint16_t g = globalBone(local, j, comboBase);
                            bool seen = false;
                            for (uint32_t e = 0; e < n; ++e) if (triBones[e] == g) { seen = true; break; }
                            if (!seen) triBones[n++] = g;
                        }
                    }

                    Run& cur = runs.back();

                    std::vector<uint16_t> trial = cur.m_bones;
                    for (uint32_t e = 0; e < n; ++e)
                        if (std::find(trial.begin(), trial.end(), triBones[e]) == trial.end())
                            trial.push_back(triBones[e]);

                    if (trial.size() > kMaxBonesPerDraw && cur.m_to > cur.m_from)
                    {
                        cur.m_to = t;
                        Run next;
                        next.m_from = t;
                        next.m_to = t + 1;
                        next.m_bones.assign(triBones.begin(), triBones.begin() + n);
                        runs.push_back(next);
                        continue;
                    }

                    cur.m_bones.swap(trial);
                    cur.m_to = t + 1;
                }

                for (const Run& run : runs)
                {
                    if (run.m_to <= run.m_from) continue;

                    // Which vertices this run actually touches dictates its emitted vertex range.
                    uint32_t lo = 0xFFFFFFFFu;
                    uint32_t hi = 0;
                    for (uint32_t t = run.m_from; t < run.m_to; ++t)
                    {
                        for (uint32_t k = 0; k < 3; ++k)
                        {
                            const uint16_t local = indices[indexStart + t * 3 + k];
                            if (local < lo) lo = local;
                            if (local + 1 > hi) hi = local + 1;
                        }
                    }
                    if (lo >= hi) continue;

                    std::vector<uint16_t> globals = run.m_bones;
                    std::sort(globals.begin(), globals.end());

                    if (!emitSection(lo, hi, indexStart + run.m_from * 3, indexStart + run.m_to * 3, globals))
                        return false;
                }
            }

            return !out.m_sections.empty();
        }
    }

    SkinContractResult RebuildSkinBatches(uint8_t* skin, uint32_t skinSize,
                                          const uint16_t* boneCombos, uint32_t boneComboCount,
                                          uint32_t skinFileSize,
                                          std::vector<uint16_t>* boneCombosOut, uint32_t* boneComboCountOut)
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

        // The bone budget is a property of every drawn section, not of the shader encoding, so it is fixed
        // before anything looks at the batches: an over-ceiling section tears on every draw regardless of
        // which effect the batch names.
        //
        // An over-budget section is REPARTITIONED, not clamped. Clamping the count stops the palette overrun
        // but leaves the mesh's influence indices pointing at entries the clamped palette no longer loads, so
        // vertices pose against the wrong bones -- which is why clamping alone left parts of the mount missing.
        const uint32_t sectionCount = Rd32(skin + kOffSkinSections);
        const uint32_t sectionOffset = Rd32(skin + kOffSkinSections + 4);

        // Maps an original section index onto the run of new sections it became (first, count).
        std::vector<std::pair<uint16_t, uint16_t> > sectionRun;

        if (sectionCount && sectionOffset &&
            static_cast<uint64_t>(sectionOffset) + static_cast<uint64_t>(sectionCount) * kSectionBytes <= skinSize)
        {
            SplitResult split;

            if (SplitSubmeshes(skin, skinSize, boneCombos, boneComboCount, sectionOffset, sectionCount, split))
            {
                // Rebuilt geometry is appended past the data the file itself occupies. It must start at the
                // FILE end, not the capacity end: `skinSize` is the writable capacity (the allocation carries
                // slack), and appending at the capacity end leaves no room at all, so the fit check below
                // could never pass and the split never committed.
                const uint32_t appendAt = skinFileSize ? skinFileSize : skinSize;
                const uint32_t lookupAt = (appendAt + 3u) & ~3u;
                const uint32_t lookupBytes = static_cast<uint32_t>(split.m_lookup.size()) * 2;
                const uint32_t indexAt = lookupAt + lookupBytes;
                const uint32_t indexBytes = static_cast<uint32_t>(split.m_indices.size()) * 2;
                const uint32_t boneAt = indexAt + indexBytes;
                const uint32_t boneBytes = static_cast<uint32_t>(split.m_bones.size());
                const uint32_t sectAt = (boneAt + boneBytes + 3u) & ~3u;
                const uint32_t sectBytes = static_cast<uint32_t>(split.m_sections.size()) * kSectionBytes;

                // The skin allocation carries slack for the grown batch array only, so there is usually no
                // room to append rebuilt geometry. Refuse rather than write past the allocation.
                if (static_cast<uint64_t>(sectAt) + sectBytes <= skinSize)
                {
                    std::memcpy(skin + lookupAt, split.m_lookup.data(), lookupBytes);
                    std::memcpy(skin + indexAt, split.m_indices.data(), indexBytes);
                    std::memcpy(skin + boneAt, split.m_bones.data(), boneBytes);

                    for (uint32_t i = 0; i < split.m_sections.size(); ++i)
                        std::memcpy(skin + sectAt + i * kSectionBytes, split.m_sections[i].m_record, kSectionBytes);

                    Wr32(skin + 0x04, static_cast<uint32_t>(split.m_lookup.size()));
                    Wr32(skin + 0x08, lookupAt);
                    Wr32(skin + 0x0C, static_cast<uint32_t>(split.m_indices.size()));
                    Wr32(skin + 0x10, indexAt);
                    Wr32(skin + 0x14, static_cast<uint32_t>(split.m_bones.size() / 4));
                    Wr32(skin + 0x18, boneAt);
                    Wr32(skin + kOffSkinSections, static_cast<uint32_t>(split.m_sections.size()));
                    Wr32(skin + kOffSkinSections + 4, sectAt);

                    sectionRun.assign(sectionCount, std::make_pair(uint16_t(0), uint16_t(0)));
                    for (uint32_t i = 0; i < split.m_sections.size(); ++i)
                    {
                        const uint16_t original = split.m_sections[i].m_original;
                        if (original < sectionRun.size())
                        {
                            if (!sectionRun[original].second) sectionRun[original].first = static_cast<uint16_t>(i);
                            sectionRun[original].second++;
                        }
                    }

                    result.sectionsBoneClamped = static_cast<uint32_t>(split.m_sections.size()) - sectionCount;

                    // The rebuilt sections index boneCombos slices that do not exist in the model's own
                    // array -- every split run appended its own compact slice. Committing the sections
                    // without committing that array left them pointing past the end of the real data, so
                    // each draw loaded the wrong bone palette. That is the artifact.
                    if (boneCombosOut)
                    {
                        *boneCombosOut = split.m_boneCombos;
                        *boneComboCountOut = static_cast<uint32_t>(split.m_boneCombos.size());
                    }
                }
            }
        }

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
        // pass looks up are the ones the model actually carries. The tables are constant for the process,
        // so they are taken from the shared cache rather than rebuilt for every skin that loads.
        const std::vector<int16_t>& coordLookup = FullCoordLookup();
        const std::vector<uint16_t>& blendOverride = FullBlendOverride();

        // The rewritten batch array is built here and written back over the original only once it is
        // complete. A source batch can expand into two (an env split), so the array grows; the caller
        // allocated slack past the file for exactly that, and `skinSize` is that writable capacity.
        std::vector<Batch> rebuilt;
        rebuilt.reserve(batchCount + 8);

        for (uint32_t i = 0; i < batchCount; ++i)
        {
            const Batch source = ReadBatch(skin + batchOffset + i * kBatchBytes);

            std::vector<Batch> piece;
            bool isAddAlpha = false;
            DownConvertBatch(source, piece, coordLookup, blendOverride, isAddAlpha);

            for (Batch& batch : piece)
            {
                // A batch on a submesh with no geometry cannot draw; it must not name a shader either, or
                // the client builds an effect for nothing.
                if (!batch.textureCount)
                {
                    batch.shaderId = 0;
                    ++result.batchesParked;
                }

                // Re-point across the run of sections its original section became. A batched section that
                // the bone split cut into several needs one batch per resulting section, or only the first
                // slice of the mesh is ever drawn -- which is what leaves parts of a model missing.
                const uint16_t original = batch.skinSectionIndex;
                if (original < sectionRun.size() && sectionRun[original].second)
                {
                    for (uint16_t s = 0; s < sectionRun[original].second; ++s)
                    {
                        Batch copy = batch;
                        copy.skinSectionIndex = static_cast<uint16_t>(sectionRun[original].first + s);
                        rebuilt.push_back(copy);
                        ++result.batchesRewritten;
                    }
                    continue;
                }

                rebuilt.push_back(batch);
                ++result.batchesRewritten;
            }

            // NOT IN BINARY, build-only. One line per batch was costing most of a second: this whole
            // function runs inside a postload callback on the frame thread, and LogWrite does an
            // fopen/fwrite/fflush/fclose per line, so a few hundred batches meant a few hundred file
            // round trips. A measured attach was 989 ms for a 78 KB skin. Summarised below instead, which
            // keeps the "model loads but draws nothing" signal without paying for it per batch.
        }

        // Commit over the original array in place. The new array starts at the same offset and may be
        // longer; it can never be shorter than the source, so nothing already read moves.
        const uint64_t writtenBytes = static_cast<uint64_t>(batchOffset) + static_cast<uint64_t>(rebuilt.size()) * kBatchBytes;
        if (writtenBytes > skinSize)
        {
            MODERN_ASSETS_LOG_WARN("m2: skin batch rewrite needs %llu byte(s) but only %u are writable; "
                                   "left unchanged",
                                   static_cast<unsigned long long>(writtenBytes), skinSize);
            return result;
        }

        for (uint32_t i = 0; i < rebuilt.size(); ++i)
            WriteBatch(skin + batchOffset + i * kBatchBytes, rebuilt[i]);

        Wr32(skin + kOffSkinBatches, static_cast<uint32_t>(rebuilt.size()));

        // The one line worth keeping: how many batches were rewritten or parked. A non-zero parked count is
        // exactly the "loads and draws nothing" symptom, and it costs a single write per skin. The source and
        // result counts differ when a batch split into a primary plus follower, which is worth seeing: it is
        // the only trace that a three-texture source batch was expressed as two passes.
        MODERN_ASSETS_LOG_INFO("m2: skin batches in=%u out=%u parked=%u bonesClamped=%u sections %u->%u coordLookup=%u combinerCombos=%u",
                               batchCount, result.batchesRewritten, result.batchesParked,
                               result.sectionsBoneClamped, sectionCount,
                               Rd32(skin + kOffSkinSections),
                               static_cast<uint32_t>(coordLookup.size()),
                               static_cast<uint32_t>(blendOverride.size()));

        // The lookup tables the batches now index were synthesised into the model body by
        // RebuildSkinContract; the counts must agree with what was actually emitted here.
        result.coordCombosBuilt = static_cast<uint32_t>(coordLookup.size());
        result.combinerCombosBuilt = static_cast<uint32_t>(blendOverride.size());
        return result;
    }
}
