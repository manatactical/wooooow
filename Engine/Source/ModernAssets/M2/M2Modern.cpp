#include "Engine/Source/ModernAssets/M2/M2Modern.h"

#include "Engine/Source/ModernAssets/M2/M2SkinContract.h"
#include "Engine/Source/ModernAssets/FdidResolver.h"
#include "Engine/Source/ModernAssets/ModernAssetsLog.h"

#include <atomic>
#include <cmath>
#include <cstring>

// Ported from WarcraftXL wxl-modern-m2 (GPL-3.0-or-later, Copyright (C) 2026 WarcraftXL): the MD21
// demux and the per-era normalisers (load/M2Demux.cpp, load/M2NormalizeParticles.cpp,
// load/M2NormalizeCameras.cpp, load/M2Fixups.cpp).
//
// The layouts were taken from the actual files, not assumed: the header prefix is byte-identical
// between v264 and v274 up to the combiner table, bones stay 0x58, attachments 0x28 and events 0x24,
// and only the camera (0x64 -> 0x74) and particle emitter (0x1DC -> 0x1EC) records changed width. Each
// was confirmed by checking that the candidate stride yields in-range bone/texture indices and finite
// pivots across real models.
//
// HOW THE REPACK WORKS
//
// Every widened record SHRINKS (0x1EC -> 0x1DC, 0x74 -> 0x64), so each affected array can be rewritten
// in place, front to back, with dst never overtaking src. The array keeps its offset and its count;
// only the used span gets shorter and dead bytes are left at its tail. That single property is what
// makes this safe: no array ever moves, so every body-relative offset nested inside a record -- the
// animation track slots that make up most of a model -- stays exactly valid and is never revisited.
//
// The only offsets that change are texture filenames, which are appended past the old end of the body
// (resolved FileDataIDs), which is why the caller hands in a buffer with slack.

namespace modern::m2
{
    namespace
    {
        constexpr uint32_t Tag(const char (&s)[5])
        {
            return uint32_t(uint8_t(s[0])) | (uint32_t(uint8_t(s[1])) << 8) |
                   (uint32_t(uint8_t(s[2])) << 16) | (uint32_t(uint8_t(s[3])) << 24);
        }

        constexpr uint32_t kMagicMD21 = 0x3132444D; // 'MD21'

        constexpr uint32_t kClientVersion = 264;

        // ---- record strides --------------------------------------------------------------------
        constexpr uint32_t kSequenceBytes       = 0x40;
        constexpr uint32_t kBoneBytes           = 0x58;
        constexpr uint32_t kMaterialBytes       = 0x04;
        constexpr uint32_t kTextureBytes        = 0x10;
        constexpr uint32_t kCameraBytes         = 0x64;
        constexpr uint32_t kParticleBytes       = 0x1DC;
        constexpr uint32_t kRibbonBytes         = 0xB0;
        constexpr uint32_t kCameraBytesModern   = 0x74;
        constexpr uint32_t kParticleBytesModern = 0x1EC;

        // ---- header offsets (identical in v264 and v274) ----------------------------------------
        constexpr uint32_t kOffMagic        = 0x00;
        constexpr uint32_t kOffVersion      = 0x04;
        constexpr uint32_t kOffGlobalFlags  = 0x10;
        constexpr uint32_t kOffSequences    = 0x1C;
        constexpr uint32_t kOffBones        = 0x2C;
        constexpr uint32_t kOffNumSkinProf  = 0x44;
        constexpr uint32_t kOffTextures     = 0x50;
        constexpr uint32_t kOffMaterials    = 0x70;
        constexpr uint32_t kOffCameras      = 0x110;
        constexpr uint32_t kOffCameraLookup = 0x118;
        constexpr uint32_t kOffRibbons      = 0x120;
        constexpr uint32_t kOffParticles    = 0x128;
        constexpr uint32_t kHeaderBytes     = 0x134;

        constexpr uint32_t kTrackGlobalSequence = 0x02;
        constexpr uint32_t kTrackValues     = 0x0C;
        constexpr int16_t  kNoGlobalSequence = -1;

        constexpr uint32_t kSeqFlagsOffset = 0x0C;
        constexpr uint32_t kSeqKeysInline  = 0x20;
        constexpr uint32_t kSeqBlendTime   = 0x1C;

        inline uint32_t Rd32(const void* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }
        inline uint16_t Rd16(const void* p) { uint16_t v; std::memcpy(&v, p, 2); return v; }
        inline void     Wr32(void* p, uint32_t v) { std::memcpy(p, &v, 4); }
        inline void     Wr16(void* p, uint16_t v) { std::memcpy(p, &v, 2); }

        inline bool Fits(uint64_t size, uint64_t offset, uint64_t count, uint64_t stride)
        {
            return offset <= size && count * stride <= size - offset;
        }

        std::atomic<uint32_t> g_converted{ 0 };
        std::atomic<uint32_t> g_failed{ 0 };
        std::atomic<uint32_t> g_texResolved{ 0 };
        std::atomic<uint32_t> g_texUnresolved{ 0 };
        std::atomic<uint32_t> g_particles{ 0 };
        std::atomic<uint32_t> g_cameras{ 0 };
        std::atomic<uint32_t> g_sequences{ 0 };
        std::atomic<uint32_t> g_external{ 0 };
        std::atomic<uint32_t> g_shadowGate{ 0 };

        struct Chunks
        {
            uint32_t bodyOffset = 0;
            uint32_t bodySize = 0;
            uint32_t txid[64]{};
            uint32_t txidCount = 0;
        };

        /// Walks the MD21 chunk container, harvesting the MD20 body and the FileDataID list.
        bool ScanContainer(const uint8_t* bytes, uint32_t size, Chunks& out)
        {
            out = Chunks{};
            uint32_t at = 0;
            while (at + 8 <= size)
            {
                const uint32_t tag = Rd32(bytes + at);
                const uint32_t length = Rd32(bytes + at + 4);
                if (length > size || at + 8 + length > size) return false;

                if (tag == kMagicMD21)
                {
                    out.bodyOffset = at + 8;
                    out.bodySize = length;
                }
                else if (tag == Tag("TXID"))
                {
                    const uint32_t count = length / 4;
                    for (uint32_t i = 0; i < count && i < 64; ++i)
                        out.txid[i] = Rd32(bytes + at + 8 + i * 4);
                    out.txidCount = count < 64 ? count : 64;
                }
                at += 8 + length;
            }
            return out.bodySize >= kHeaderBytes && out.bodyOffset + out.bodySize <= size;
        }

        // ---- normalization ---------------------------------------------------------------------

        constexpr uint32_t kOffParticleFlags   = 0x004;
        constexpr uint32_t kOffParticleTexture = 0x016;
        constexpr uint32_t kOffParticleBlend   = 0x028;
        constexpr uint32_t kOffParticleGravity = 0x084;

        constexpr uint16_t kPackedTextureIdMask = 0x1F;
        constexpr uint32_t kParticleFlagMultiTex = 0x10000000;
        constexpr uint32_t kParticleFlagCompressedGravity = 0x800000;
        constexpr float    kGravityMagnitudeUnit = 0.04238648f;
        constexpr float    kPackedDirectionUnit = 1.0f / 128.0f;

        struct Ctx
        {
            uint8_t* body;
            uint32_t size;
        };

        bool InlineSequence(uint8_t* body, uint32_t size, uint32_t index, bool& inlineKeys)
        {
            const uint32_t count = Rd32(body + kOffSequences);
            const uint32_t offset = Rd32(body + kOffSequences + 4);
            if (index >= count || !offset) return false;
            if (!Fits(size, offset, count, kSequenceBytes)) return false;
            inlineKeys = (Rd32(body + offset + index * kSequenceBytes + kSeqFlagsOffset) & kSeqKeysInline) != 0;
            return true;
        }

        /// Expands gravity keys packed as {int8 x, int8 y, int16 magnitude} into the downward scalar
        /// the client reads, and clears the flag that said they were packed.
        bool ExpandGravityKeys(const Ctx& ctx, uint8_t* record, uint32_t flags)
        {
            int16_t globalSequence;
            std::memcpy(&globalSequence, record + kOffParticleGravity + kTrackGlobalSequence, 2);

            const uint32_t slots = Rd32(record + kOffParticleGravity + kTrackValues);
            const uint32_t offset = Rd32(record + kOffParticleGravity + kTrackValues + 4);
            const bool perSequence = globalSequence == kNoGlobalSequence;

            if (slots && offset)
            {
                if (!Fits(ctx.size, offset, slots, 8)) return false;

                for (uint32_t s = 0; s < slots; ++s)
                {
                    // A slot whose keys stream in with a companion file does not address this body;
                    // leaving it packed is right, that file's arrival brings its own.
                    if (perSequence)
                    {
                        bool inlineKeys = false;
                        if (!InlineSequence(ctx.body, ctx.size, s, inlineKeys)) return false;
                        if (!inlineKeys) continue;
                    }

                    const uint8_t* slot = ctx.body + offset + s * 8;
                    const uint32_t keyCount = Rd32(slot);
                    const uint32_t keyOffset = Rd32(slot + 4);
                    if (!keyCount || !keyOffset) continue;
                    if (!Fits(ctx.size, keyOffset, keyCount, 4)) return false;

                    uint8_t* keys = ctx.body + keyOffset;
                    for (uint32_t k = 0; k < keyCount; ++k)
                    {
                        uint8_t* key = keys + k * 4;
                        const float dx = static_cast<int8_t>(key[0]) * kPackedDirectionUnit;
                        const float dy = static_cast<int8_t>(key[1]) * kPackedDirectionUnit;
                        int16_t packedMagnitude;
                        std::memcpy(&packedMagnitude, key + 2, 2);

                        const float planar = dx * dx + dy * dy;
                        float z = planar < 1.0f ? std::sqrt(1.0f - planar) : 0.0f;
                        float magnitude = packedMagnitude * kGravityMagnitudeUnit;
                        if (magnitude < 0.0f) { z = -z; magnitude = -magnitude; }
                        const float scalar = -(z * magnitude);
                        std::memcpy(key, &scalar, 4);
                    }
                }
            }

            Wr32(record + kOffParticleFlags, flags & ~kParticleFlagCompressedGravity);
            return true;
        }

        /// Rewrites one modern emitter into the client's width. dst is never ahead of src.
        bool NormalizeParticle(const Ctx& ctx, const uint8_t* src, uint8_t* dst)
        {
            // Everything the client's record covers copies straight down; the modern trailing
            // multi-texture scroll parameters are dropped, which is the whole width difference.
            std::memmove(dst, src, kParticleBytes);

            const uint32_t flags = Rd32(dst + kOffParticleFlags);

            // A multi-layer emitter packs three 5-bit layer ids into the texture slot. Only the first
            // has a home in a single-texture emitter, and an id past the table would read out of
            // bounds in the client, so it is masked and clamped.
            uint16_t textureId = Rd16(dst + kOffParticleTexture);
            if (flags & kParticleFlagMultiTex) textureId &= kPackedTextureIdMask;
            const uint32_t textureCount = Rd32(ctx.body + kOffTextures);
            if (textureCount && textureId >= textureCount) textureId = 0;
            Wr16(dst + kOffParticleTexture, textureId);

            // The client maps this byte through a switch with no default, so a blend past its
            // 7-entry table leaves the emitter Opaque: the billboard quad then draws its whole
            // alpha-masked texture opaque (a black square behind an additive glow). Modern alpha
            // modes are past that table; AddAlpha is the nearest state the client can express.
            if (dst[kOffParticleBlend] > 6) dst[kOffParticleBlend] = 3;

            if ((flags & kParticleFlagCompressedGravity) && !ExpandGravityKeys(ctx, dst, flags))
                return false;
            return true;
        }

        /// Reads the first key of a modern camera's field-of-view track.
        float FirstFovKey(const Ctx& ctx, const uint8_t* track)
        {
            constexpr float kDefaultFov = 0.7853982f; // 45 degrees: the conventional static framing

            int16_t globalSequence;
            std::memcpy(&globalSequence, track + kTrackGlobalSequence, 2);

            const uint32_t slots = Rd32(track + kTrackValues);
            const uint32_t offset = Rd32(track + kTrackValues + 4);
            if (!slots || !offset) return kDefaultFov;
            if (!Fits(ctx.size, offset, slots, 8)) return kDefaultFov;

            if (globalSequence == kNoGlobalSequence)
            {
                bool inlineKeys = false;
                if (!InlineSequence(ctx.body, ctx.size, 0, inlineKeys)) return kDefaultFov;
                if (!inlineKeys) return kDefaultFov;
            }

            const uint8_t* slot = ctx.body + offset;
            const uint32_t keyCount = Rd32(slot);
            const uint32_t keyOffset = Rd32(slot + 4);
            if (!keyCount || !keyOffset) return kDefaultFov;
            if (!Fits(ctx.size, keyOffset, keyCount, 0x0C)) return kDefaultFov;

            float fov;
            std::memcpy(&fov, ctx.body + keyOffset, 4);
            return fov;
        }

        /// Rewrites one modern camera into the client's width.
        ///
        /// The modern record moved the field of view from a plain float ahead of the tracks to an
        /// animated track after them, so this is a real reshape: the track body slides back by that
        /// float's width and the float is filled from the track's first key. Both forms agree on
        /// everything else, including the diagonal-radians unit, so the camera frames the same view.
        void NormalizeCamera(const Ctx& ctx, const uint8_t* src, uint8_t* dst)
        {
            constexpr uint32_t kSrcFarClip = 0x04;
            constexpr uint32_t kSrcNearClip = 0x08;
            constexpr uint32_t kSrcBody = 0x0C;
            constexpr uint32_t kSrcFovTrack = 0x60;
            constexpr uint32_t kBodyBytes = 0x54;

            // Read every scalar out first: for the first record dst and src are the same address.
            uint32_t type, far, near;
            std::memcpy(&type, src + 0x00, 4);
            std::memcpy(&far, src + kSrcFarClip, 4);
            std::memcpy(&near, src + kSrcNearClip, 4);
            const float fov = FirstFovKey(ctx, src + kSrcFovTrack);

            std::memmove(dst + 0x10, src + kSrcBody, kBodyBytes);
            std::memcpy(dst + 0x00, &type, 4);
            std::memcpy(dst + 0x04, &fov, 4);
            std::memcpy(dst + 0x08, &far, 4);
            std::memcpy(dst + 0x0C, &near, 4);
        }

        void FixSequences(uint8_t* body, const Ctx& ctx)
        {
            const uint32_t count = Rd32(body + kOffSequences);
            const uint32_t offset = Rd32(body + kOffSequences + 4);
            if (!count || !offset || !Fits(ctx.size, offset, count, kSequenceBytes)) return;

            for (uint32_t i = 0; i < count; ++i)
            {
                uint8_t* sequence = body + offset + i * kSequenceBytes;

                // A curated set of modern animation ids sits above the client's own id table. Each
                // one means the same motion as a client id, so it is remapped rather than dropped.
                const uint16_t id = Rd16(sequence);
                uint16_t mapped = id;
                switch (id)
                {
                    case 564: mapped = 37;  break;
                    case 548: mapped = 41;  break;
                    case 556: mapped = 42;  break;
                    case 552: mapped = 43;  break;
                    case 554: mapped = 44;  break;
                    case 562: mapped = 45;  break;
                    case 572: mapped = 39;  break;
                    case 574: mapped = 187; break;
                    default: break;
                }
                if (mapped != id)
                {
                    Wr16(sequence, mapped);
                    ++g_sequences;
                }

                // The modern blend time packs two u16s where the client reads one u32; read whole it
                // is a blend so long the transition never completes.
                Wr32(sequence + kSeqBlendTime, Rd32(sequence + kSeqBlendTime) & 0xFFFFu);

                if (!(Rd32(sequence + kSeqFlagsOffset) & kSeqKeysInline)) ++g_external;
            }
        }

        void FixMaterials(uint8_t* body, const Ctx& ctx)
        {
            const uint32_t count = Rd32(body + kOffMaterials);
            const uint32_t offset = Rd32(body + kOffMaterials + 4);
            if (!count || !offset || !Fits(ctx.size, offset, count, kMaterialBytes)) return;

            for (uint32_t i = 0; i < count; ++i)
            {
                uint8_t* material = body + offset + i * kMaterialBytes;
                uint16_t flags = Rd16(material);
                uint16_t blend = Rd16(material + 2);
                // Past the client's 7-entry blend table, Add is the closest state it can express.
                if (blend > 6) { blend = 4; flags |= 0x5; }
                Wr16(material, flags & 0x1F);
                Wr16(material + 2, blend);
            }
        }

        void ClampRibbonRefs(uint8_t* body, uint32_t size)
        {
            const uint32_t count = Rd32(body + kOffRibbons);
            const uint32_t offset = Rd32(body + kOffRibbons + 4);
            if (!count || !offset || !Fits(size, offset, count, kRibbonBytes)) return;

            const uint32_t textures = Rd32(body + kOffTextures);
            const uint32_t materials = Rd32(body + kOffMaterials);

            auto clampArray = [&](uint8_t* ribbon, uint32_t arrayAt, uint32_t limit) {
                if (!limit) return;
                const uint32_t n = Rd32(ribbon + arrayAt);
                const uint32_t o = Rd32(ribbon + arrayAt + 4);
                if (!n || !o || !Fits(size, o, n, 2)) return;
                uint16_t* values = reinterpret_cast<uint16_t*>(body + o);
                for (uint32_t k = 0; k < n; ++k)
                    if (values[k] >= limit) values[k] = static_cast<uint16_t>(limit - 1);
            };

            for (uint32_t i = 0; i < count; ++i)
            {
                uint8_t* ribbon = body + offset + i * kRibbonBytes;
                clampArray(ribbon, 0x14, textures);
                clampArray(ribbon, 0x1C, materials);
            }
        }

        void ClampCameraRefs(uint8_t* body, uint32_t size)
        {
            const uint32_t count = Rd32(body + kOffCameraLookup);
            const uint32_t offset = Rd32(body + kOffCameraLookup + 4);
            const uint32_t cameras = Rd32(body + kOffCameras);
            if (!count || !offset || !Fits(size, offset, count, 2)) return;

            int16_t* values = reinterpret_cast<int16_t*>(body + offset);
            for (uint32_t i = 0; i < count; ++i)
                if (values[i] >= static_cast<int32_t>(cameras)) values[i] = -1;
        }

        /// A modern model can ship every bone flag empty, which sends the client's shadow pass down a
        /// fast path that never rebuilds the bone palette -- the shadow then appears to rotate with
        /// the camera. Setting the flag on one bone puts the runtime on the full path stock content
        /// already takes.
        void FixShadowGate(uint8_t* body, uint32_t size)
        {
            const uint32_t count = Rd32(body + kOffBones);
            const uint32_t offset = Rd32(body + kOffBones + 4);
            if (!count || !offset || !Fits(size, offset, count, kBoneBytes)) return;

            for (uint32_t i = 0; i < count; ++i)
                if (Rd32(body + offset + i * kBoneBytes + 4) & 0x200u) return;

            Wr32(body + offset + 4, Rd32(body + offset + 4) | 0x200u);
            ++g_shadowGate;
        }

        /// Compacts one widened array in place, front to back. Returns false if a record is malformed.
        template <typename RecordFn>
        bool CompactArray(Ctx& ctx, uint32_t headerAt, uint32_t sourceStride, uint32_t targetStride,
                          RecordFn rewrite, uint32_t* rewritten)
        {
            const uint32_t count = Rd32(ctx.body + headerAt);
            const uint32_t offset = Rd32(ctx.body + headerAt + 4);
            if (!count || !offset) return true;
            if (!Fits(ctx.size, offset, count, sourceStride)) return false;

            const uint8_t* src = ctx.body + offset;
            uint8_t* dst = ctx.body + offset;
            for (uint32_t i = 0; i < count; ++i)
            {
                if (!rewrite(src, dst)) return false;
                src += sourceStride;
                dst += targetStride;
            }

            if (rewritten) *rewritten = count;
            return true;
        }

        /// Points each hardcoded texture with no inline name at the client path its TXID resolves to.
        /// Appends the string past the current end of the body and re-points the filename array.
        void InjectTxidNames(uint8_t* body, uint32_t& size, uint32_t capacity, const Chunks& chunks)
        {
            const uint32_t count = Rd32(body + kOffTextures);
            const uint32_t offset = Rd32(body + kOffTextures + 4);
            if (!count || !offset || !Fits(size, offset, count, kTextureBytes)) return;

            for (uint32_t i = 0; i < count; ++i)
            {
                uint8_t* texture = body + offset + i * kTextureBytes;
                if (Rd32(texture) != 0) continue; // only a hardcoded slot can carry a path

                const uint32_t nameCount = Rd32(texture + 8);
                const uint32_t nameOffset = Rd32(texture + 0x0C);
                if (nameCount >= 2 && nameOffset) continue; // the model already names it

                const uint32_t fileDataId = i < chunks.txidCount ? chunks.txid[i] : 0;
                if (!fileDataId) continue;

                const char* path = db2::fdid::ResolveTexture(fileDataId);
                if (!path)
                {
                    ++g_texUnresolved;
                    MODERN_ASSETS_LOG_WARN("m2: texture %u: FileDataID %u does not resolve, so the "
                                           "client will use its white placeholder", i, fileDataId);
                    continue;
                }

                // Copied into the body rather than pointed at the resolver's cache: the client's
                // parser turns this offset into a pointer, and the body must own the bytes.
                const size_t length = std::strlen(path) + 1;
                const size_t pad = (4u - (size & 3u)) & 3u;
                if (static_cast<uint64_t>(size) + pad + length > capacity)
                {
                    // The reserve is not unbounded. Running out means the remaining textures keep
                    // the client's white placeholder rather than the body growing past its
                    // allocation -- a visible miss, not a heap overrun.
                    MODERN_ASSETS_LOG_WARN("m2: texture paths exhausted the %u byte reserve at "
                                           "texture %u of %u", capacity, i, count);
                    ++g_texUnresolved;
                    continue;
                }

                while (size & 3u) body[size++] = 0;
                std::memcpy(body + size, path, length);
                Wr32(texture + 8, static_cast<uint32_t>(length));
                Wr32(texture + 0x0C, size);
                size += static_cast<uint32_t>(length);
                ++g_texResolved;
            }
        }
    }

    Stats GetStats()
    {
        Stats stats;
        stats.modelsConverted = g_converted.load(std::memory_order_relaxed);
        stats.modelsFailed = g_failed.load(std::memory_order_relaxed);
        stats.texturesResolved = g_texResolved.load(std::memory_order_relaxed);
        stats.texturesUnresolved = g_texUnresolved.load(std::memory_order_relaxed);
        stats.particlesNormalized = g_particles.load(std::memory_order_relaxed);
        stats.camerasNormalized = g_cameras.load(std::memory_order_relaxed);
        stats.sequencesRemapped = g_sequences.load(std::memory_order_relaxed);
        stats.externalSequences = g_external.load(std::memory_order_relaxed);
        stats.shadowGateForced = g_shadowGate.load(std::memory_order_relaxed);
        return stats;
    }

    bool IsModernContainer(const uint8_t* bytes, uint32_t size)
    {
        return bytes && size >= 8 && Rd32(bytes) == kMagicMD21;
    }

    bool Convert(uint8_t* file, uint32_t capacity, uint32_t& bodySize, const char** why)
    {
        *why = nullptr;

        Chunks chunks;
        if (!ScanContainer(file, bodySize, chunks))
        {
            *why = "no MD20 body in the container";
            return false;
        }

        uint8_t* body = file + chunks.bodyOffset;
        uint32_t size = chunks.bodySize;

        if (Rd32(body) != 0x3032444D)
        {
            *why = "body is not MD20";
            return false;
        }

        const uint32_t version = Rd32(body + kOffVersion);
        if (version < 264 || version > 274)
        {
            *why = "inner version outside the supported window";
            return false;
        }

        // A split-skeleton model keeps its bones and sequences in a companion file; there is no way to
        // stand it up here, so the load is refused rather than half done.
        if (Rd32(body + kOffBones) == 0 && Rd32(body + kOffSequences) == 0)
        {
            *why = "split skeleton model (.skel) is not supported";
            return false;
        }

        // The body has to end up at offset 0 of the resident buffer, because the client's parser takes
        // the buffer start as the body base. Slide it down; the allocation pointer the caller holds is
        // unchanged, only the offset of the bytes within it.
        if (chunks.bodyOffset != 0) std::memmove(file, body, size);
        body = file;

        Ctx ctx{ body, size };

        // --- record normalization: one record shape for every source era, in place ---
        if (version >= 272)
        {
            uint32_t particles = 0;
            if (!CompactArray(ctx, kOffParticles, kParticleBytesModern, kParticleBytes,
                              [&ctx](const uint8_t* src, uint8_t* dst) { return NormalizeParticle(ctx, src, dst); },
                              &particles))
            {
                *why = "particle record normalization rejected a record";
                ++g_failed;
                return false;
            }
            g_particles.fetch_add(particles, std::memory_order_relaxed);

            uint32_t cameras = 0;
            const uint32_t cameraCount = Rd32(body + kOffCameras);
            if (cameraCount)
            {
                // Cameras are the one reshape that is not a plain truncation, so it is driven directly
                // rather than through the generic compactor.
                const uint32_t offset = Rd32(body + kOffCameras + 4);
                if (!Fits(size, offset, cameraCount, kCameraBytesModern))
                {
                    *why = "camera array outside the body";
                    ++g_failed;
                    return false;
                }
                const uint8_t* src = body + offset;
                uint8_t* dst = body + offset;
                for (uint32_t i = 0; i < cameraCount; ++i)
                {
                    NormalizeCamera(ctx, src, dst);
                    src += kCameraBytesModern;
                    dst += kCameraBytes;
                }
                cameras = cameraCount;
            }
            g_cameras.fetch_add(cameras, std::memory_order_relaxed);
        }

        // --- in-place field deltas and post-fixup clamps ---
        FixSequences(body, ctx);
        FixMaterials(body, ctx);
        ClampRibbonRefs(body, size);
        ClampCameraRefs(body, size);
        FixShadowGate(body, size);

        // The client's skin chooser walks a 4-entry threshold table indexed by 4 - numSkinProfiles;
        // more profiles than it ever shipped would underflow it.
        if (Rd32(body + kOffNumSkinProf) > 4) Wr32(body + kOffNumSkinProf, 1);

        // The runtime owns the texture combo arrays only when these bits say so, and it would then try
        // to free interior pointers of this one buffer. Nothing here hands it any.
        Wr32(body + kOffGlobalFlags, Rd32(body + kOffGlobalFlags) & ~0x60u);

        // --- texture names, appended past the body ---
        uint32_t withNames = size;
        InjectTxidNames(body, withNames, capacity, chunks);
        size = withNames;

        // --- the texture-unit contract a modern skin omits ---
        // A modern skin packs each batch's blend pair and coordinate set into its shaderId and leaves
        // the model's textureCoordCombos empty. Without this the client reads entry 0 of an empty
        // table, the effect builder finds nothing to build, and the model loads while drawing nothing
        // -- which is what makes modern mounts invisible rather than broken-looking.
        uint32_t withContract = size;
        const SkinContractResult contract =
            RebuildSkinContract(body, withContract, capacity, withContract);
        size = withContract;
        if (contract.coordCombosBuilt)
        {
            MODERN_ASSETS_LOG_INFO("m2: skin contract built: coordLookup=%u combinerCombos=%u",
                                   contract.coordCombosBuilt, contract.combinerCombosBuilt);
        }

        Wr32(body + kOffVersion, kClientVersion);
        bodySize = size;

        g_converted.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
}
