// Not in the original 3.3.5a binary. The loot beam: a pillar of light over every NPC corpse that can still
// be looted, ported from the WXL extension wxl-loot-beam (WarcraftXL).
//
// Copyright (C) 2026 WarcraftXL
//
// This program is free software: you can redistribute it and/or modify it under the terms of the
// GNU General Public License as published by the Free Software Foundation, either version 3 of the
// License, or (at your option) any later version.

#include "LootBeam.h"

#include <cmath>
#include <cstring>
#include <vector>

#include "Engine/Source/ConsoleBase/ConsoleVar.h"
#include "Engine/Source/ConsoleBase/Types.h"
#include "Engine/Source/Gx/CGxDevice/CGxDevice.h"
#include "Engine/Source/Gx/CGxVertexPC.h"
#include "Engine/Source/Gx/Gx.h"
#include "Engine/Source/ShaderEffect/Types.h"
#include "Engine/Source/Tempest/C3Vector.h"
#include "Engine/Source/Tempest/C44Matrix.h"
#include "Engine/Source/Tempest/CRect.h"
#include "Engine/Source/UnitOutline/UnitOutline.h"
#include "WoW/Source/Object/ObjectClient/Unit_C.h"
#include "WoW/Source/ObjectMgrClient/ObjectMgrClient.h"
#include "WoW/Source/Ui/LootFrame.h"
#include "WoW/Source/WorldClient/World.h"

namespace
{
    // --- the look -------------------------------------------------------------------------------
    //
    // Compile-time on purpose: the extension read these from an INI beside its DLL and offered a
    // settings panel, but this port ships the defaults and nothing else. Change a value here and
    // rebuild.

    enum GearTier
    {
        kTierCurrency = 0, // money-only loot (server hint 9)
        kTierPoor,         // quality 0  grey
        kTierCommon,       // quality 1  white
        kTierUncommon,     // quality 2  green
        kTierRare,         // quality 3  blue
        kTierEpic,         // quality 4  purple
        kTierLegendary,    // quality 5  orange
        kTierArtifact,     // quality 6  gold
        kTierHeirloom,     // quality 7  cyan
        kTierCount
    };

    struct TierStyle
    {
        bool  enabled;
        float color[3];
    };

    struct BeamStyle
    {
        float height       = 20.0f;  // how far the beam rises, yards
        float baseOffset   = 1.0f;   // where the floating base sits above the body, yards
        float beamWidth    = 0.50f;  // half-width at the base
        float widthPerYard = 0.010f; // minimum half-width per yard of camera distance (0 = off)
        float color[3]     = { 1.00f, 0.82f, 0.42f }; // warm gold, used while a body's loot is unknown

        // Per-tier on/off and tint. Currency, poor and common ship pure black; a tier switched off draws
        // nothing at all for corpses whose best loot falls into it.
        TierStyle tiers[kTierCount] = {
            { true, { 0.00f, 0.00f, 0.00f } },       // kTierCurrency
            { true, { 0.00f, 0.00f, 0.00f } },       // kTierPoor
            { true, { 0.00f, 0.00f, 0.00f } },       // kTierCommon
            { true, { 0.12f, 1.00f, 0.00f } },       // kTierUncommon  #1EFF00
            { true, { 0.00f, 0.19607843f, 1.00f } }, // kTierRare      #0032FF
            { true, { 0.58823529f, 0.00f, 1.00f } }, // kTierEpic      #9600FF
            { true, { 1.00f, 0.50f, 0.00f } },       // kTierLegendary
            { true, { 0.90f, 0.80f, 0.50f } },       // kTierArtifact
            { true, { 0.00f, 0.80f, 1.00f } },       // kTierHeirloom
        };

        float beamAlpha  = 0.75f;
        float pulse      = 0.20f;
        float pulseSpeed = 1.60f;
        float fadeIn     = 0.35f;
        float fadeOut    = 0.70f;

        bool  showBeam       = true;
        bool  showSparkles   = true;
        int   sparkleCount   = 24;
        float sparkleSize    = 0.04f;
        float sparkleAlpha   = 0.90f;
        float sparkleRise    = 1.00f;
        float sparkleDrift   = 0.05f;
        float sparkleLife    = 2.40f;
        float sparkleTwinkle = 6.00f;

        bool  throughWalls = false; // let the world hide the beacon
        float maxDistance  = 0.0f;  // yards; 0 = unlimited
    };

    const BeamStyle kStyle;

    constexpr int    kMaxBeacons   = 64;   // corpses beamed at once; the rest wait for a slot
    constexpr int    kMaxSparkles  = 32;
    constexpr int    kMaxLootSlots = 19;   // 18 item slots plus the money entry
    constexpr int    kBeamMaxRows  = 64;   // the grid is sized per beam from its pixel height
    constexpr int    kBeamMaxCols  = 4;
    constexpr float  kTwoPi        = 6.28318530717959f;


    // --- the descriptor block -------------------------------------------------------------------
    //
    // A unit's update fields are the raw OBJECT_FIELD_* / UNIT_FIELD_* array, reachable from the object
    // at 0x08 (CGObjectData, which is what CGUnit_C::m_unit points into). The client models the same
    // block, so these reads go through CGUnitData and the offsets are pinned below.

    static_assert(offsetof(CGUnitData, m_unitFieldHealth) == 0x48, "UNIT_FIELD_HEALTH byte offset");
    static_assert(offsetof(CGUnitData, m_unitDynamicFlags) == 0x124, "UNIT_DYNAMIC_FLAGS byte offset");
    static_assert(offsetof(CGUnitData, m_unitFieldPadding) == 0x234, "UNIT_FIELD_PADDING byte offset");

    // UNIT_DYNFLAG_LOOTABLE clears once the body has been looted, which is what takes a beacon away.
    constexpr uint32_t kDynamicFlagLootable = 0x1;

    // UNIT_FIELD_PADDING carries the companion mod-loot-beam's hint: quality + 1 for 1..8, 9 for money
    // without gear, 0 for "no hint". Anything else is ignored rather than indexed out of bounds.
    constexpr uint32_t kQualityMax   = 7;
    constexpr uint32_t kCurrencyHint = kQualityMax + 2;

    CGUnitData* UnitData(void* unit)
    {
        return unit ? static_cast<CGUnit_C*>(unit)->m_unit : nullptr;
    }

    // Reads the server's tier hint off a corpse. False when the field holds no usable hint.
    bool ServerTier(void* unit, int& tier)
    {
        CGUnitData* data = UnitData(unit);
        if (!data) return false;

        const uint32_t value = data->m_unitFieldPadding;
        if (value == kCurrencyHint) { tier = kTierCurrency; return true; }
        if (value < 1 || value > kQualityMax + 1) return false;
        tier = int(value);
        return true;
    }

    // A dead NPC the server still flags lootable. Player corpses are left alone: this marks bodies.
    bool IsLootableCorpse(void* unit)
    {
        if (!unit) return false;

        CGObject_C* object = static_cast<CGObject_C*>(unit);
        if (!(object->m_obj->m_type & TYPE_UNIT)) return false;
        if (object->m_obj->m_type & TYPE_PLAYER)  return false;

        CGUnitData* data = UnitData(unit);
        if (!data || data->m_unitFieldHealth > 0) return false;

        // The flags field is only trusted when it can be the field: a wrong read lands on unrelated heap
        // whose high bits betray it, and the strict test then falls back to the health-only verdict
        // rather than turning into noise -- the same guard the extension used.
        if ((data->m_unitDynamicFlags & ~0x0000FFFFu) == 0 &&
            !(data->m_unitDynamicFlags & kDynamicFlagLootable))
            return false;

        return true;
    }

    // --- helpers --------------------------------------------------------------------------------

    float Clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

    uint32_t Pack(float alpha, const float rgb[3])
    {
        const uint32_t a = uint32_t(Clamp01(alpha) * 255.0f + 0.5f);
        const uint32_t r = uint32_t(Clamp01(rgb[0]) * 255.0f + 0.5f);
        const uint32_t g = uint32_t(Clamp01(rgb[1]) * 255.0f + 0.5f);
        const uint32_t b = uint32_t(Clamp01(rgb[2]) * 255.0f + 0.5f);
        return (a << 24) | (r << 16) | (g << 8) | b;
    }

    // The tint pulled toward white by `whiten`: the middle of a light shaft is hotter than its edges.
    uint32_t PackTint(float alpha, const float rgb[3], float whiten)
    {
        const float c[3] = {
            rgb[0] + (1.0f - rgb[0]) * whiten,
            rgb[1] + (1.0f - rgb[1]) * whiten,
            rgb[2] + (1.0f - rgb[2]) * whiten,
        };
        return Pack(alpha, c);
    }

    // A tiny xorshift so a mote's drift and flicker vary without <random>'s weight. Seeded from the
    // corpse's GUID, so two bodies never sparkle in lockstep.
    uint32_t NextRandom(uint32_t& state)
    {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return state;
    }

    float Random01(uint32_t& state) { return float(NextRandom(state) >> 8) * (1.0f / 16777216.0f); }

    struct Sparkle
    {
        float pos[3];
        float vel[3];
        float age;
        float life;
        float twinkle;
        float twinkleSpeed;
    };

    struct Beacon
    {
        uint64_t guid;
        float    pos[3];
        float    fade;
        int      tier; // rarest GearTier known for the corpse, -1 until known
        bool     seen;

        Sparkle  sparkles[kMaxSparkles];
        uint32_t rngSeed;
    };

    Beacon    s_beacons[kMaxBeacons];
    int       s_trackedCount;
    int       s_beaconCount;
    float     s_phase;

    uint64_t s_lootGuid; // GUID of the loot the cached tier belongs to
    int      s_lootTier; // its GearTier, recomputed when the GUID changes

    CVar* s_cvarLootBeam;

    // --- the triangle queue ---------------------------------------------------------------------
    //
    // Not gfx::Flush: that one blends source-over, which makes a light shaft read as a coloured pane.
    // The beacon is additive with a colour on every vertex, so the GPU interpolates the falloffs and a
    // handful of quads read as a soft volume.

    struct Vertex { float x, y, z; uint32_t color; };
    static_assert(sizeof(Vertex) == 16, "Vertex must match the declared vertex format stride");

    std::vector<Vertex> s_vertices;

    void Triangle(const float a[3], const float b[3], const float c[3],
                  uint32_t ca, uint32_t cb, uint32_t cc)
    {
        s_vertices.push_back(Vertex{ a[0], a[1], a[2], ca });
        s_vertices.push_back(Vertex{ b[0], b[1], b[2], cb });
        s_vertices.push_back(Vertex{ c[0], c[1], c[2], cc });
    }

    // Soft falloff from 1 at x = 0 to 0 at x = 1, with a flat top and a gentle shoulder -- the
    // difference between a lit volume and a hard-edged quad.
    float SoftEdge(float x)
    {
        const float a = x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x);
        return 1.0f - a * a * (3.0f - 2.0f * a);
    }

    // Transparent where the shaft leaves the body, peaking a short way up, then easing to nothing at the
    // top: light gathering in the air rather than a column with a cut-off base.
    float BeamVertical(float t)
    {
        constexpr float kRise = 0.22f;
        if (t < kRise) return 1.0f - SoftEdge(t / kRise);
        return SoftEdge((t - kRise) / (1.0f - kRise));
    }

    // --- visibility -----------------------------------------------------------------------------
    //
    // The occluder is the frame's own depth buffer, shared with the unit outline (CUnitOutline's depth
    // pass). Terrain, WMOs and every M2 -- trees, rocks, banners, foliage, other bodies -- write into it
    // before the beam is drawn, so a plain depth test hides the beacon behind exactly what hides the
    // outlined units, at the same pixels, with no per-shot ray casting. The test is only armed when that
    // depth is known to be the bound surface (see CUnitOutline::IsDepthPassBound); otherwise it would run
    // against whatever the device happened to hold and reject every pixel silently.
    bool DepthOcclusionAvailable()
    {
        return !kStyle.throughWalls && CUnitOutline::IsDepthPassBound();
    }




    // --- drawing --------------------------------------------------------------------------------

    float s_viewportW = 0.0f, s_viewportH = 0.0f;

    // The shaft: one camera-facing billboard gridded so a colour can sit on every vertex. The grid
    // follows the shaft's own screen size, so a far thread is a few quads and a near one is finely
    // divided, which is where the vertex work can be seen.
    void QueueBeamColumn(const float pos[3], const BeamStyle& style, float alphaScale, const float rgb[3])
    {
        const C3Vector& camera = CWorldScene::s_activeWorldView;
        const float dx = camera.x - pos[0];
        const float dy = camera.y - pos[1];
        const float dz = camera.z - pos[2];
        const float len  = sqrtf(dx * dx + dy * dy);
        const float dist = sqrtf(dx * dx + dy * dy + dz * dz);

        // Billboard across the view direction: the only vertical plane the camera sees face-on.
        float fx = 1.0f, fy = 0.0f;
        if (len > 1e-3f) { fx = dx / len; fy = dy / len; }
        const float sx = -fy, sy = fx;

        const float minHalfWidth = style.widthPerYard > 0.0f ? style.widthPerYard * dist : 0.0f;

        const float baseZ = pos[2] + style.baseOffset;
        const float topZ  = pos[2] + fmaxf(style.height, style.baseOffset + 0.1f);
        if (baseZ >= topZ - 0.05f) return;
        const float spanZ = topZ - baseZ;

        int rows = kBeamMaxRows;
        int cols = kBeamMaxCols;
        const C44Matrix& proj = g_theGxDevicePtr->m_projection;

        if (s_viewportW > 0.0f && s_viewportH > 0.0f && dist > 0.5f)
        {
            const float invDist = 1.0f / dist;
            const float pxH = fabsf(proj.b1) * spanZ * invDist * 0.5f * s_viewportH;
            const float pxW = fabsf(proj.a0) * (2.0f * style.beamWidth) * invDist * 0.5f * s_viewportW;
            rows = int(pxH / 5.0f) + 1; // 5 px per cell is below what the eye resolves on a soft edge
            cols = int(pxW / 5.0f) + 1;
            if (rows < 4)            rows = 4;
            if (rows > kBeamMaxRows) rows = kBeamMaxRows;
            if (cols < 2)            cols = 2;
            if (cols > kBeamMaxCols) cols = kBeamMaxCols;
        }
        else if (dist > 150.0f) { rows = 5;  cols = 2; }
        else if (dist > 80.0f)  { rows = 12; cols = 4; }

        float    xs[kBeamMaxRows + 1][kBeamMaxCols + 1];
        float    ys[kBeamMaxRows + 1][kBeamMaxCols + 1];
        float    zs[kBeamMaxRows + 1];
        uint32_t cs[kBeamMaxRows + 1][kBeamMaxCols + 1];

        // The shaft is drawn whole; the frame depth the unit outline shares decides what the eye actually
        // sees of it (see Draw). There is no per-column ray work left to do here.

        for (int r = 0; r <= rows; ++r)
        {
            const float t = float(r) / float(rows);
            const float w = fmaxf(style.beamWidth * (1.0f - 0.55f * t), minHalfWidth);
            const float v = BeamVertical(t);
            zs[r] = baseZ + spanZ * t;

            for (int c = 0; c <= cols; ++c)
            {
                const float u = -1.0f + 2.0f * float(c) / float(cols);
                const float h = SoftEdge(fabsf(u));
                xs[r][c] = pos[0] + sx * (w * u);
                ys[r][c] = pos[1] + sy * (w * u);

                cs[r][c] = PackTint(style.beamAlpha * v * h * alphaScale, rgb,
                                    0.45f * h * (1.0f - 0.3f * t));
            }
        }

        for (int r = 0; r < rows; ++r)
        {
            for (int c = 0; c < cols; ++c)
            {
                const float p00[3] = { xs[r][c],         ys[r][c],         zs[r] };
                const float p10[3] = { xs[r][c + 1],     ys[r][c + 1],     zs[r] };
                const float p11[3] = { xs[r + 1][c + 1], ys[r + 1][c + 1], zs[r + 1] };
                const float p01[3] = { xs[r + 1][c],     ys[r + 1][c],     zs[r + 1] };
                Triangle(p00, p10, p11, cs[r][c], cs[r][c + 1], cs[r + 1][c + 1]);
                Triangle(p00, p11, p01, cs[r][c], cs[r + 1][c + 1], cs[r + 1][c]);
            }
        }
    }

    // Drifting, twinkling motes: each is a small camera-facing disc of light -- a hot core easing out to
    // a transparent rim -- so additive blending turns it into a soft ball rather than a flat square.
    void QueueSparkles(const float pos[3], const Sparkle* sparkles, int count,
                       const BeamStyle& style, float alphaScale, const float rgb[3])
    {
        if (count <= 0) return;

        const C3Vector& camera = CWorldScene::s_activeWorldView;
        constexpr int   kSegments = 8;
        constexpr float kRing1 = 0.45f, kRing2 = 1.0f; // radii, as fractions of the mote size
        constexpr float kShoulder = 0.58f;             // brightness of the middle ring

        for (int i = 0; i < count; ++i)
        {
            const Sparkle& sp = sparkles[i];
            const float lifeT = sp.life > 0.001f ? sp.age / sp.life : 1.0f;

            // Ease in off the spawn, hold, then ease out, so a mote never pops in or cuts off.
            constexpr float kIn = 0.15f, kOut = 0.45f;
            float env = 1.0f;
            if (lifeT < kIn)              env = lifeT / kIn;
            else if (lifeT > 1.0f - kOut) env = (1.0f - lifeT) / kOut;
            if (env < 0.0f) env = 0.0f;
            if (env > 1.0f) env = 1.0f;

            const float twinkle = 0.5f + 0.5f * sinf(sp.twinkle);
            const float alpha   = style.sparkleAlpha * env * twinkle * alphaScale;
            if (alpha <= 0.003f) continue;

            const float wx = pos[0] + sp.pos[0];
            const float wy = pos[1] + sp.pos[1];
            const float wz = pos[2] + sp.pos[2];

            // No per-mote visibility test any more: the depth test in Draw hides a mote behind the same
            // geometry it hides the shaft behind.

            // The disc lies in the plane perpendicular to the eye->mote ray, so it reads as a round ball
            // from any angle. cross(worldUp, f) collapses on a straight-up ray, hence the axis fallback.
            float f[3] = { wx - camera.x, wy - camera.y, wz - camera.z };
            float flen = sqrtf(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
            if (flen < 1e-4f) { f[0] = 0.0f; f[1] = 1.0f; f[2] = 0.0f; flen = 1.0f; }
            f[0] /= flen; f[1] /= flen; f[2] /= flen;

            float r[3] = { -f[1], f[0], 0.0f };
            float rlen = sqrtf(r[0] * r[0] + r[1] * r[1]);
            if (rlen < 1e-4f) { r[0] = 1.0f; r[1] = 0.0f; r[2] = 0.0f; rlen = 1.0f; }
            r[0] /= rlen; r[1] /= rlen; r[2] /= rlen;

            float u[3] = { f[1] * r[2] - f[2] * r[1],
                           f[2] * r[0] - f[0] * r[2],
                           f[0] * r[1] - f[1] * r[0] };

            // A mote swells a little as it brightens, so a flicker reads as a glint.
            const float size = style.sparkleSize * (0.85f + 0.15f * twinkle);
            for (int k = 0; k < 3; ++k) { r[k] *= size; u[k] *= size; }

            const uint32_t core     = PackTint(alpha, rgb, 0.85f);
            const uint32_t shoulder = PackTint(alpha * kShoulder, rgb, 0.55f);
            const uint32_t rim      = Pack(0.0f, rgb);

            float mid[kSegments][3];
            float edge[kSegments][3];
            for (int s = 0; s < kSegments; ++s)
            {
                const float a  = kTwoPi * float(s) / float(kSegments);
                const float ca = cosf(a), sa = sinf(a);
                const float rx = r[0] * ca + u[0] * sa;
                const float ry = r[1] * ca + u[1] * sa;
                const float rz = r[2] * ca + u[2] * sa;

                mid[s][0]  = wx + rx * kRing1; mid[s][1]  = wy + ry * kRing1; mid[s][2]  = wz + rz * kRing1;
                edge[s][0] = wx + rx * kRing2; edge[s][1] = wy + ry * kRing2; edge[s][2] = wz + rz * kRing2;
            }

            const float centre[3] = { wx, wy, wz };
            for (int s = 0; s < kSegments; ++s)
            {
                const int n = (s + 1) % kSegments;
                Triangle(centre, mid[s], mid[n], core, shoulder, shoulder);
                Triangle(mid[s], edge[s], edge[n], shoulder, rim, rim);
                Triangle(mid[s], edge[n], mid[n], shoulder, rim, shoulder);
            }
        }
    }

    // --- sparkle simulation ---------------------------------------------------------------------

    void SpawnSparkle(Sparkle& sp, uint32_t& rng, const BeamStyle& style)
    {
        const float top  = fmaxf(style.height, style.baseOffset + 0.1f);
        const float span = top - style.baseOffset;
        const float ang  = Random01(rng) * kTwoPi;
        const float rad  = sqrtf(Random01(rng)) * style.beamWidth * 1.5f;
        const float dir  = Random01(rng) * kTwoPi;
        const float drift = style.sparkleDrift * (0.4f + 1.2f * Random01(rng));

        sp.vel[0] = cosf(dir) * drift;
        sp.vel[1] = sinf(dir) * drift;
        sp.vel[2] = style.sparkleRise * (0.5f + Random01(rng));
        sp.life   = style.sparkleLife * (0.6f + 0.8f * Random01(rng));
        sp.age    = Random01(rng) * sp.life * 0.5f;
        sp.twinkle      = Random01(rng) * kTwoPi;
        sp.twinkleSpeed = style.sparkleTwinkle * (0.6f + 0.8f * Random01(rng));

        // Motes are born progressively less often the higher they are -- the min of three uniforms is
        // quadratic -- so the field is denser at the foot of the shaft than at its crest.
        const float h = fminf(fminf(Random01(rng), Random01(rng)), Random01(rng));
        sp.pos[0] = cosf(ang) * rad + sp.vel[0] * sp.age;
        sp.pos[1] = sinf(ang) * rad + sp.vel[1] * sp.age;
        sp.pos[2] = style.baseOffset + span * 0.85f * h + sp.vel[2] * sp.age;
    }

    void AdvanceSparkle(Sparkle& sp, uint32_t& rng, const BeamStyle& style, float dt)
    {
        sp.age += dt;
        sp.pos[0] += sp.vel[0] * dt;
        sp.pos[1] += sp.vel[1] * dt;
        sp.pos[2] += sp.vel[2] * dt;
        sp.twinkle += sp.twinkleSpeed * dt;

        const float top = fmaxf(style.height, style.baseOffset + 0.1f);
        if (sp.age >= sp.life || sp.pos[2] > top)
            SpawnSparkle(sp, rng, style);
    }

    void SeedSparkles(Beacon& b, const BeamStyle& style)
    {
        uint32_t seed = uint32_t(b.guid) ^ uint32_t(b.guid >> 32);
        seed ^= 0x9E3779B9u;
        if (seed == 0) seed = 0xA341316Cu;
        b.rngSeed = seed;
        for (int i = 0; i < kMaxSparkles; ++i)
            SpawnSparkle(b.sparkles[i], b.rngSeed, style);
    }

    void AdvanceSparkles(Beacon& b, const BeamStyle& style, float dt)
    {
        if (style.sparkleCount <= 0) return;
        for (int i = 0; i < kMaxSparkles; ++i)
            AdvanceSparkle(b.sparkles[i], b.rngSeed, style, dt);
    }

    // --- loot -----------------------------------------------------------------------------------

    // The GearTier of the rarest item in the open loot. The client holds one loot at a time and keeps its
    // slots in CGLootInfo; GetLootItemRarity returns -1 for an item it has not cached. + kTierPoor maps
    // quality 0..7 onto the tier table.
    int ReadLootTier()
    {
        if (CGLootInfo::m_lootObj == WOWGUID()) return -1;

        const int32_t count = CGLootWindow::GetNumLootItems();
        int best = -1;
        for (int slot = 0; slot < count && slot < kMaxLootSlots; ++slot)
        {
            const int32_t quality = CGLootWindow::GetLootItemRarity(uint32_t(slot));
            if (quality >= 0 && quality > best) best = quality;
        }
        return best < 0 ? -1 : best + kTierPoor;
    }

    // Adopts the open loot's tier onto the tracked corpse it belongs to, and only ever raises it: the
    // server's hint and what the client discovers are merged, whichever holds the rarer item winning.
    void ScanLoot()
    {
        const uint64_t guid = CGLootInfo::m_lootObj.m_guid;
        if (guid != s_lootGuid)
        {
            s_lootGuid = guid;
            s_lootTier = guid != 0 ? ReadLootTier() : -1;
        }

        if (s_lootTier < 0) return;
        for (int i = 0; i < s_trackedCount; ++i)
        {
            if (s_beacons[i].guid == s_lootGuid && s_lootTier > s_beacons[i].tier)
                s_beacons[i].tier = s_lootTier;
        }
    }

    // --- per-frame steps ------------------------------------------------------------------------

    void ScanUnits()
    {
        // Everything tracked starts unseen; a corpse that qualifies this frame marks itself seen, so
        // what is left unseen afterwards is a body that was looted or despawned and must fade out.
        for (int i = 0; i < s_trackedCount; ++i)
            s_beacons[i].seen = false;

        s_beaconCount = 0;

        ObjectMgr* mgr = ClntObjMgrGetCurrent();
        if (!mgr || ClntObjMgrGetActivePlayer() == WOWGUID()) return;

        const C3Vector& camera = CWorldScene::s_activeWorldView;
        const float maxD2 = kStyle.maxDistance > 0.0f ? kStyle.maxDistance * kStyle.maxDistance : 0.0f;

        // The walk reads the resident-object list; it is main-thread only, which this is.
        for (CGObject_C* object = mgr->m_visibleObjects.Head(); object;
             object = mgr->m_visibleObjects.Next(object))
        {
            void* unit = object;
            if (!IsLootableCorpse(unit)) continue;

            const C3Vector p = object->GetPosition();
            if (maxD2 > 0.0f && (p - camera).SquaredMag() > maxD2) continue;

            const uint64_t guid = object->m_obj->m_guid.m_guid;

            // Match on GUID so a corpse keeps its fade level while tracked; a newly seen one enters at
            // zero and eases up.
            Beacon* b = nullptr;
            for (int i = 0; i < s_trackedCount; ++i)
            {
                if (s_beacons[i].guid == guid) { b = &s_beacons[i]; break; }
            }
            if (!b)
            {
                if (s_trackedCount >= kMaxBeacons) continue; // table full; let it go this pass
                b = &s_beacons[s_trackedCount++];
                b->guid = guid;
                b->fade = 0.0f;
                b->tier = -1;
                SeedSparkles(*b, kStyle);
            }
            b->pos[0] = p.x;
            b->pos[1] = p.y;
            b->pos[2] = p.z;
            b->seen   = true;

            // A corpse the server tagged carries its tier from the moment it dies, so it colours before
            // the loot window is ever opened. It only ever raises the tier.
            int serverTier = -1;
            if (ServerTier(unit, serverTier) && serverTier > b->tier)
                b->tier = serverTier;
        }
    }

    void UpdateFade(float dt)
    {
        const float inRate  = kStyle.fadeIn  > 0.001f ? 1.0f / kStyle.fadeIn  : 1.0e9f;
        const float outRate = kStyle.fadeOut > 0.001f ? 1.0f / kStyle.fadeOut : 1.0e9f;

        int kept = 0;
        for (int i = 0; i < s_trackedCount; ++i)
        {
            Beacon b = s_beacons[i];
            if (b.seen)
            {
                b.fade += inRate * dt;
                if (b.fade > 1.0f) b.fade = 1.0f;
            }
            else
            {
                b.fade -= outRate * dt;
                if (b.fade <= 0.0f) continue; // finished fading; forget it
            }
            if (b.fade > 0.001f) AdvanceSparkles(b, kStyle, dt);
            s_beacons[kept++] = b;
        }
        s_trackedCount = kept;

        s_beaconCount = 0;
        for (int i = 0; i < s_trackedCount; ++i)
        {
            if (s_beacons[i].fade > 0.001f) ++s_beaconCount;
        }
    }

    // Draws the queue additively straight onto the finished scene and puts every piece of device state
    // back. Emptied on every path, including the ones that draw nothing.
    //
    // Through the engine's own gx calls rather than the raw D3D9 device: this client's decompiled
    // CGxDevice does not expose the device pointer on the base class, and going through gx is what keeps
    // the cached render state the rest of the frame relies on in step with what was actually set.
    void Draw()
    {
        struct Emptied { ~Emptied() { s_vertices.clear(); } } emptied;

        if (!g_theGxDevicePtr || !g_theGxDevicePtr->m_context || s_vertices.empty()) return;

        CGxDevice* gx = g_theGxDevicePtr;

        gx->RsPush();

        // Untextured, unlit, vertex-coloured geometry through the fixed-function pipeline. Every one of
        // these is inherited from whatever drew last; any one left wrong rejects the draw outright or
        // repaints it in a colour that was not asked for.
        gx->RsSet(GxRs_VertexShader, static_cast<void*>(nullptr));
        gx->RsSet(GxRs_PixelShader, static_cast<void*>(nullptr));
        gx->RsSet(GxRs_Texture0, static_cast<void*>(nullptr));
        gx->RsSet(GxRs_Texture1, static_cast<void*>(nullptr));

        // No texture is bound, so stage 0 multiplies its (white) texture by the current colour -- which
        // with a Color0 stream and lighting off is the interpolated vertex colour. Stage 1 passes that
        // through untouched (SELECTARG2 = current).
        GxRsSet(GxRs_ColorOp0, GxTexOp_Mod);
        GxRsSet(GxRs_AlphaOp0, GxTexOp_Mod);
        GxRsSet(GxRs_ColorOp1, GxTexOp_PassThru);

        GxRsSet(GxRs_Lighting, 0);
        GxRsSet(GxRs_Fog, 0);          // world fog would tint the beacon with distance
        GxRsSet(GxRs_Culling, 0);
        GxRsSet(GxRs_ColorWrite, 15);
        GxRsSet(GxRs_ScissorTest, 0);

        // The world hides the beacon exactly where it hides an outlined unit: terrain, WMOs and every M2
        // the frame has drawn are already in the shared depth by now, so LEQUAL against it costs the
        // image its occluded pixels and no ray work at all. Only armed when that depth is the bound
        // surface; otherwise the test would read whatever the device holds and drop everything.
        if (DepthOcclusionAvailable())
        {
            GxRsSet(GxRs_DepthTest, 1);
            GxRsSet(GxRs_DepthFunc, GxCmp_LEqual);
        }
        else
        {
            // Through walls, or no shared depth on this device: the beacon is a marker to be found.
            GxRsSet(GxRs_DepthTest, 0);
        }

        GxRsSet(GxRs_DepthWrite, 0);   // a marker is never part of the world

        // Additive: source colour times its own alpha, added straight onto what is behind. That keeps the
        // shaft from looking like a pane of coloured glass and lets overlapping soft geometry sum into a
        // glow. It is depth-tested, so the glow only ever lands on visible geometry.
        GxRsSet(GxRs_BlendingMode, GxBlend_Add);
        GxUpdateAlphaRef(GxRs_AlphaRef);

        // The scene's view matrix carries no translation: the world is drawn about the camera, so a
        // world-space vertex has to be moved to that origin or it projects thousands of units away.
        const C3Vector& eye = CWorldScene::s_activeWorldView;
        C44Matrix world;
        world.Translate(C3Vector(-eye.x, -eye.y, -eye.z));
        gx->XformPushAndSet(GxXform_World, world);

        CGxBuf* vertexBuf = gx->BufStream(GxPoolTarget_Vertex, sizeof(CGxVertexPC),
                                          static_cast<uint32_t>(s_vertices.size()));
        CGxVertexPC* out = reinterpret_cast<CGxVertexPC*>(gx->BufLock(vertexBuf));

        for (size_t i = 0; i < s_vertices.size(); ++i)
        {
            const Vertex& v = s_vertices[i];
            out[i].p = C3Vector(v.x, v.y, v.z);
            // The gx colour is BGRA in memory: byte order b, g, r, a.
            out[i].c.b = static_cast<uint8_t>(v.color & 0xFF);
            out[i].c.g = static_cast<uint8_t>((v.color >> 8) & 0xFF);
            out[i].c.r = static_cast<uint8_t>((v.color >> 16) & 0xFF);
            out[i].c.a = static_cast<uint8_t>((v.color >> 24) & 0xFF);
        }

        gx->BufUnlock(vertexBuf, 0);
        vertexBuf->m_hasData = 1;
        GxPrimVertexPtr(vertexBuf, GxVBF_PC);

        CGxBatch batch;
        batch.m_primType   = GxPrim_Triangles;
        batch.m_start      = 0;
        batch.m_count      = static_cast<uint32_t>(s_vertices.size());
        batch.m_minIndex   = 0;
        batch.m_maxIndex   = static_cast<uint32_t>(s_vertices.size());
        gx->Draw(&batch, 0); // unindexed: count 0 makes the device draw the vertex stream as-is

        GxXformPop(GxXform_World);
        gx->RsPop();
    }
}

namespace LootBeam
{
    void Init()
    {
        // Registered by CWorldParam::Initialize so the Advanced panel can read its default at the login
        // screen. This call is a safety net for a client that never ran that; Register reuses an
        // existing var.
        s_cvarLootBeam = CVar::Register("lootBeam", "Draw a beam of light over lootable corpses", 1, "1",
                                        nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
    }

    void Update(float elapsedSec)
    {
        if (!s_cvarLootBeam || !s_cvarLootBeam->m_intValue || !g_theGxDevicePtr ||
            !g_theGxDevicePtr->m_context || !World::s_m2Scene)
        {
            s_trackedCount = 0;
            s_beaconCount  = 0;
            s_vertices.clear();
            return;
        }

        // Shapes are per-frame; a frame that never reached the draw must not leave its queue to pile up
        // under the next one.
        s_vertices.clear();

        // The 3D rectangle in pixels, with the viewport narrowed to it. The grid sizes itself from this,
        // so only what is actually being drawn onto counts.
        CRect window = { 0.0f, 0.0f, 0.0f, 0.0f };
        g_theGxDevicePtr->CapsWindowSize(window);
        s_viewportW = (window.maxX - window.minX) * (CWorldScene::viewPort.m_x.m_h - CWorldScene::viewPort.m_x.m_l);
        s_viewportH = (window.maxY - window.minY) * (CWorldScene::viewPort.m_y.m_h - CWorldScene::viewPort.m_y.m_l);

        s_phase += elapsedSec * kStyle.pulseSpeed * kTwoPi;
        s_phase = fmodf(s_phase, kTwoPi); // keep the phase bounded over a long session

        ScanUnits();
        ScanLoot();
        UpdateFade(elapsedSec);

        if (s_beaconCount == 0) return;

        // -1..+1 mapped into [1 - pulse, 1], so a beacon never gets brighter than the configured alpha
        // and a pulse of 0 is perfectly steady. Each beacon's own fade folds in on top.
        const float pulseScale = 1.0f - 0.5f * kStyle.pulse * (1.0f - sinf(s_phase));

        for (int i = 0; i < s_trackedCount; ++i)
        {
            if (s_beacons[i].fade <= 0.001f) continue;

            const float* rgb = kStyle.color;
            const int    t   = s_beacons[i].tier;
            if (t >= 0 && t < kTierCount)
            {
                // A tier switched off is filtered out entirely: the corpse gets no beacon at all, so
                // unwanted drops (bare currency, greys, ...) can be hidden without hiding everything.
                if (!kStyle.tiers[t].enabled) continue;
                rgb = kStyle.tiers[t].color;
            }

            const float alpha = pulseScale * s_beacons[i].fade;
            if (kStyle.showBeam && kStyle.height > 0.01f)
                QueueBeamColumn(s_beacons[i].pos, kStyle, alpha, rgb);

            if (kStyle.showSparkles && kStyle.sparkleCount > 0)
            {
                int count = kStyle.sparkleCount;
                if (count > kMaxSparkles) count = kMaxSparkles;
                QueueSparkles(s_beacons[i].pos, s_beacons[i].sparkles, count, kStyle, alpha, rgb);
            }
        }

        Draw();
    }
}
