// Not in the original 3.3.5a binary. Screen-space ambient occlusion over the finished world frame; see
// AmbientOcclusion.h for the pipeline and docs/AMBIENT-OCCLUSION.md for the design. This file is the
// implementation, and says where it had to differ from that document.
#include "Engine/Source/AmbientOcclusion/AmbientOcclusion.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include <d3dcompiler.h>

#include "Engine/Source/Base/Handle.h"
#include "Engine/Source/ConsoleBase/ConsoleVar.h"
#include "Engine/Source/ConsoleBase/Types.h"
#include "Engine/Source/FFX/FFX.h"
#include "Engine/Source/Event/PerfLog.h"
#include "Engine/Source/Gx/CGxDevice/CGxDevice.h"
#include "Engine/Source/Gx/CGxDevice/CGxShader.h"
#include "Engine/Source/Gx/CGxTex.h"
#include "Engine/Source/Gx/CGxTexFlags.h"
#include "Engine/Source/Gx/Gx.h"
#include "Engine/Source/Services/Texture.h"
#include "Engine/Source/Tempest/C2Vector.h"
#include "Engine/Source/Tempest/C2iVector.h"
#include "Engine/Source/Tempest/C3Vector.h"
#include "Engine/Source/Tempest/C44Matrix.h"
#include "Engine/Source/Tempest/CImVector.h"
#include "Engine/Source/UnitOutline/UnitOutline.h"

namespace {

// Tuned for the client's world scale, which is yards: a player is about 2 yards tall, a door about 3. This is the
// reach at the default ssaoSpread; the kernel then adapts it per pixel (see kMinReachPixels).
constexpr float kRadius = 0.65f;

// How dark a fully occluded pixel goes: the occlusion fraction is scaled by this before it is taken from 1. The
// default of the ssaoStrength cvar; ssaoSpread scales kRadius.
constexpr float kStrength = 0.5f;

// Depth offset, in yards, that stops a surface from occluding itself. Scaled with the per-pixel reach.
constexpr float kBias = 0.03f;

// The ssaoSpread slider runs 0.06 to 0.33 and means this many times less than it says: the old default of 1 is 0.25 here, a
// multiple of 1 on kRadius and kBias, and the slider top of 0.33 is the old top of 1.33.
constexpr float kSpreadScale = 4.0f;

// The term is faded out with distance and stops entirely at the ssaoDistance cvar (default kDistance, in yards):
// far geometry is a few pixels per object, where AO is noise rather than shading, and skipping it saves the work
// too. The fade begins at kFadeStartFraction of that distance.
constexpr float kDistance = 145.0f;
constexpr float kFadeStartFraction = 0.25f;

// The three scales. One kernel size cannot suit everything: sized for a crate it misses the wide creases of a
// building, and sized for a building it smothers small props. So the occupancy runs three scales, each with its own
// reach, and the three terms are multiplied at the end. They are computed in one draw (the setup they share is paid
// once) and each lives in its own colour channel of the one AO target, so the blur and the upsample that follow handle
// all three in a single pass.
//
// The automatic spread, per scale. The kernel's reach is chosen per pixel so that, on screen, it always covers
// between minPixels and maxPixels:
//   - a surface far away would otherwise get a reach of a pixel or two, which is noise rather than shading, so
//     the reach grows with distance until it covers minPixels, up to maxGrowth times the scale's base reach;
//   - a surface right under the camera would get a reach of hundreds of pixels, which makes every tap a cache
//     miss, so it shrinks to maxPixels. This is the main cost control: the wide passes are the ones that would
//     otherwise thrash the cache, and they are capped hardest.
// The result is that a crate, a wall and a mountainside each get a shadow proportioned to how big they look.
//
//   reachScale   the scale's base reach as a multiple of the spread-scaled kRadius (yards)
//   strength     how much of the ssaoStrength this scale gets; together they stack to roughly one full-strength term
//   distance     the fraction of ssaoDistance at which the scale has faded out and stops running. Fine detail is
//                invisible at a distance, so the small pass is the first to stop, and the big one runs to the end.
//   spin         the kernel's rotation offset, so the scales do not share a pattern
//   enabled      false skips the scale's draw altogether, and the composite ignores its channel
struct PassScale {
    float reachScale;
    float minPixels;
    float maxPixels;
    float maxGrowth;
    float strength;
    float distance;
    float spin;
    bool enabled;
};

constexpr int32_t kPassCount = 3;
constexpr PassScale kPasses[kPassCount] = {
    // small: props, contact shadows, the gap under a crate
    { 1.0f, 8.0f, 40.0f, 5.0f, 0.70f, 0.45f, 0.00f, true },
    // medium: furniture, boxes, the corner of a room
    { 3.0f, 14.0f, 70.0f, 4.0f, 0.60f, 0.75f, 0.33f, true },
    // big: buildings, walls meeting the ground, the underside of an eave. Switched off for now; set enabled to true
    // to bring it back, nothing else needs to change.
    { 9.0f, 30.0f, 105.0f, 3.0f, 0.50f, 1.00f, 0.66f, false },
};

// The close-up character pass (see the shaders for how it works). The exclusion mask's red channel is grass and water,
// its green the character; these are the engine's colour-write bits for them. (GxRs_ColorWrite's bits are not
// Direct3D's: the engine's are 1 red, 4 green, 2 blue, 8 alpha -- see Ds_ColorWriteEnable in the device.)
constexpr int32_t kMaskRed = 1;
constexpr int32_t kMaskGreen = 4;

// How far the character's occlusion reaches, in yards, and how many screen pixels that is held between: close up
// a 0.3 yard crease is a hundred pixels wide, which would make every tap a cache miss, and far off it is a few pixels
// of noise. Strength is the ssaoStrength cvar's, scaled; the bias keeps a surface from shading itself.
constexpr float kCharRadius = 0.30f;
constexpr float kCharMinPixels = 10.0f;
constexpr float kCharMaxPixels = 110.0f;
constexpr float kCharStrengthScale = 1.0f;
constexpr float kCharBias = 0.012f;
constexpr float kCharGain = 3.0f;
constexpr float kCharCurvePower = 1.5f;
constexpr float kCharSoftness = 0.35f;
constexpr float kCharDepthSigma = 0.03f;

// How much of the strength is lost at full rain: 0.5 is half.
constexpr float kRainStrengthLoss = 0.5f;

// How far the rain's tint is pushed past the fog colour's own hue. The storm's fog is nearly grey, so its hue taken
// as it is would not show; 1 is the fog's own, higher is more colour.
constexpr float kRainTintBoost = 2.5f;

// How far past ssaoCharDistance the character's pass takes to fade out, in yards. Inside ssaoCharDistance it is at full
// strength; over these yards it fades to nothing, handing the character back to the frame-wide pass, and past them it
// is off and costs nothing.
constexpr float kCharFadeYards = 4.0f;

// The cvars' defaults: whether the pass is on, and how close the camera has to be (yards from the player).
constexpr float kCharDistance = 6.0f;
constexpr int32_t kCharConstantRows = 11;

// The pass runs at half the frame's size in each direction -- a quarter of the pixels -- and is brought back to
// full size by a depth-aware upsample in the composite.
constexpr int32_t kResolutionDivisor = 2;

// A tight corner only has about half of its hemisphere covered, so the raw occlusion fraction barely reaches 0.5 and
// the term would never get dark. This scales it back up before the strength is applied.
constexpr float kOcclusionGain = 2.4f;

// The shape of the ramp. The occlusion fraction goes through a power before the strength is applied: above 1 the
// curve is convex, so a surface that is only lightly shadowed stays nearly clean and the darkening climbs faster
// the closer the occluder is, which is how contact shadows read. Each tap also counts for more the closer it is to
// the surface (see kNearTapWeight), and a tap's own on/off test is a short ramp rather than a step -- the step is
// what made the term noisy, because a tap flips from 0 to 1 on a pixel's worth of depth.
constexpr float kCurvePower = 1.5f;

// Each scale takes five taps (kKernel in the shader). Few, because each scale is a pass of its own and the rotation
// tile plus the two blurs turn five taps into dozens of samples by the time a pixel is shaded.
constexpr float kSoftness = 0.35f;
constexpr float kNearTapWeight = 0.7f;
constexpr float kFarTapWeight = 0.3f;

// Distance-dependent smoothing. Far surfaces cover few pixels, so single-pixel depth steps are large relative to the
// surface and a fixed bias and a one-pixel normal baseline both read as noise. Per yard of distance, the bias grows by
// kBiasPerYard, and the normal is taken over a baseline that widens from one AO texel to kFarNormalSpan texels by
// kNormalFarYards.
constexpr float kBiasPerYard = 0.0035f;
constexpr float kFarNormalSpan = 2.0f;
constexpr float kNormalFarYards = 45.0f;

// Foliage. A canopy is hundreds of leaf cards at every angle, so almost every pixel of it has something close enough
// in front of it to read as a corner, and it comes out far darker than a tree should. A wall or a corner of a building
// is the opposite: smooth, with its depth changing along one axis only. The shader tells them apart by how many of
// four directions across the pixel are not flat (see the clutter term), and the occlusion of a pixel where all four
// are is scaled down to this fraction.
constexpr float kFoliageKeep = 0.1f;

// The other side of the same measure: occlusion on a smooth surface (a wall, a floor, a corner of a building) is
// scaled up by this, so the creases of structures come out darker without touching foliage.
constexpr float kSmoothBoost = 1.45f;

// Blur tap spacing in AO texels, and how far apart two depths may be, as a fraction of the centre's own distance,
// before the tap stops contributing. Also the upsample's depth tolerance.
constexpr float kBlurRadius = 1.0f;
constexpr float kBlurDepthSigma = 0.04f;

// The world's depth mapping; see MapWorldDepth. These are the water pass's constants, for the same projection.
constexpr float kMinViewportDepthExtent = 0.01f;
constexpr float kDeepestWorldDepthInFullRangeViewport = 0.9999995f;
constexpr float kWorldDepthMargin = 2.0e-6f;
constexpr float kSkyMinDepth = 0.99903f;
// What a pixel with no geometry decodes to. Far past the fade, so it can never occlude or be occluded.
constexpr float kMaxViewDepth = 2000.0f;

// The rows shared by every pass, uploaded once per frame; the two after them are rewritten for each draw.
constexpr int32_t kConstantRows = 9;
constexpr int32_t kPassRow = 9;
constexpr int32_t kPassRows = 6;

// Registers shared by the occupancy, blur and composite shaders. These replace the engine's pixel shader
// outright, so the whole constant range is free.
//
// c0 = { 1/width, 1/height, viewport origin x, viewport origin y }        (frame size; origin normalised)
// c1 = { P00, P11, P08, P09 }                                             (the world projection's terms)
// c2 = { atInfinity, perInverseViewDepth, deepest depth, max view depth }
// c3 = { radius, strength, bias, viewport width }                         (width normalised to the frame)
// c4 = { fade start, fade end, viewport height, blur radius in AO texels }
// c5 = { depth sigma, exclusion mask stamped, 1/AO width, 1/AO height }
// c6 = { occlusion gain, curve power, tap softness, bias per yard }
// c7 = { near tap weight, far tap weight, far normal span, far normal yards }
// c8 = { foliage keep, smooth boost, character pass share (0..1), fog start }
//
// Per draw (kPassRow onward):
// occupancy, two rows per scale (small c9/c10, medium c11/c12, big c13/c14):
//   A = { reach scale, min reach px, max reach px, max reach growth }
//   B = { strength scale, distance fraction, spin offset, enabled }
// composite:
//   c9  = { show small, show medium, show big, fog end }                    (1 shows a scale, 0 hides it)
//   c10 = { rain tint r, g, b, how much }
const char* const kCommonHLSL = R"HLSL(
float4 c0 : register(c0);
float4 c1 : register(c1);
float4 c2 : register(c2);
float4 c3 : register(c3);
float4 c4 : register(c4);
float4 c5 : register(c5);
float4 c6 : register(c6);
float4 c7 : register(c7);
float4 c8 : register(c8);
float4 c9 : register(c9);
float4 c10 : register(c10);
float4 c11 : register(c11);
float4 c12 : register(c12);
float4 c13 : register(c13);
float4 c14 : register(c14);

// Stored depth back to yards in front of the eye. The denominator is negative for real geometry (the client's
// projection stores depth = m22 + m32 / z with m32 < 0), which is why there is no clamp on it.
float ViewZ(float depth)
{
    return depth >= c2.z ? c2.w : min(c2.y / (depth - c2.x), c2.w);
}

// The point on the surface the frame's uv sees at view distance z, camera at the origin looking down +z.
float3 ViewPos(float2 uv, float z)
{
    float2 ndc = float2((uv.x - c0.z) / c3.w * 2 - 1, 1 - (uv.y - c0.w) / c4.z * 2);
    return float3((ndc.x - c1.z) / c1.x * z, (ndc.y - c1.w) / c1.y * z, z);
}

// An AO texel covers a 2x2 block of frame pixels. Its depth is always read from the same one of them -- the
// lower right -- so the occupancy pass, the blur and the upsample all agree on which depth an AO texel stands
// for. The quarter-pixel nudge keeps the floor off the exact boundary the texel centre sits on.
float2 SnapToFramePixel(float2 uv)
{
    float2 pixel = floor(uv / c0.xy + 0.25);
    return (pixel + 0.5) * c0.xy;
}

float DepthAt(sampler2D s, float2 uv)
{
    return tex2Dlod(s, float4(uv, 0, 0)).r;
}

// The view distance, as a 16-bit fraction of the fade end (c4.y) held in two 8-bit channels. Everything after the
// first pass reads this instead of the scene depth: it is a quarter the size, already in yards, and carries the
// exclusion mask as "past the fade", so a tap is one fetch from a small texture rather than a decode of a full-size
// one plus a second fetch for the mask. Resolution is c4.y / 65535, finer than the bias at any distance.
float2 PackZ(float z)
{
    float s = saturate(z / c4.y) * 65535;
    float hi = floor(s / 256);
    float lo = min(floor(s - hi * 256 + 0.5), 255);
    return float2(hi, lo) / 255;
}

float UnpackZ(float2 p)
{
    return (p.x * 255 * 256 + p.y * 255) * (c4.y / 65535);
}
)HLSL";

// Half-size view distance of the scene: the depth snapped to the lower right pixel of each 2x2 block, decoded to
// yards and packed. Excluded surfaces (grass, water) and sky are written as "past the fade", which is what makes
// them neither occlude nor receive anything in the passes that follow.
const char* const kZHLSL = R"HLSL(
sampler2D sDepth : register(s0);
sampler2D sExclude : register(s1);

float4 main(float2 uv : TEXCOORD0) : COLOR0
{
    float2 pixel = floor(uv / c0.xy + 0.25);
    float2 uvC = (pixel + 0.5) * c0.xy;
    float z = ViewZ(DepthAt(sDepth, uvC));

    [branch] if (c5.y > 0.5)
    {
        // Grass is a few pixels wide per blade, so the one pixel this texel stands for would usually miss it and
        // leave the blade to shade (and be shaded by) its neighbours. The mask is read over the 4x4 pixels around
        // the block instead, as four bilinear fetches of 2x2 each: anything in it marks the whole texel.
        float2 corner = pixel * c0.xy;
        float2 o = c0.xy;
        float seen = tex2Dlod(sExclude, float4(corner + float2(-o.x, -o.y), 0, 0)).r
                   + tex2Dlod(sExclude, float4(corner + float2( o.x, -o.y), 0, 0)).r
                   + tex2Dlod(sExclude, float4(corner + float2(-o.x,  o.y), 0, 0)).r
                   + tex2Dlod(sExclude, float4(corner + float2( o.x,  o.y), 0, 0)).r;
        if (seen > 0.01) { z = c4.y; }
    }

    return float4(PackZ(z), 0, 1);
}
)HLSL";

// Writes a stamped batch into the exclusion mask. The cutoff is the batch's own alpha reference, so the mask
// follows the real blade shape rather than the whole quad. c0.x = cutoff.
const char* const kExcludeHLSL = R"HLSL(
sampler2D s0 : register(s0);
float4 c0 : register(c0);

float4 main(float2 uv : TEXCOORD0) : COLOR0
{
    clip(tex2D(s0, uv).a - c0.x);
    return float4(1, 1, 1, 1);
}
)HLSL";

const char* const kAoHLSL = R"HLSL(
sampler2D sZ : register(s0);

float ZAt(float2 uv)
{
    return UnpackZ(tex2Dlod(sZ, float4(uv, 0, 0)).rg);
}

float3 PosAt(float2 uv)
{
    return ViewPos(uv, ZAt(uv));
}

// The kernel, as (x, y, z, scale): a golden-angle spiral over the unit hemisphere with taps clustered near the
// surface, where contact shadows live. Precomputed, so a pixel needs one sin/cos for its rotation rather than five.
// The same five taps serve all three scales; what differs between them is the reach they are multiplied by.
static const float4 kKernel[5] = {
    float4(0.31623, 0.00000, 0.94868, 0.30000),
    float4(-0.40390, 0.37000, 0.83666, 0.50000),
    float4(0.06190, -0.70440, 0.70711, 0.70000),
    float4(0.50950, 0.66380, 0.54772, 0.85000),
    float4(-0.93420, -0.16530, 0.31623, 1.00000)
};

// All three scales in one draw. What is the same for every scale -- the pixel's position, its normal and tangent frame,
// the foliage measure, the rotation tile -- is worked out once, and each enabled scale then only runs its own taps.
// Three separate draws used to repeat all of that (twelve of the eighteen fetches a pixel made) once per scale.
float4 main(float2 uv : TEXCOORD0) : COLOR0
{
    // The scales' settings: A = { reach scale, min reach px, max reach px, max reach growth },
    // B = { strength scale, distance fraction, spin offset, enabled }. Indexed only by the unrolled loops below.
    float4 scaleA[3] = { c9, c11, c13 };
    float4 scaleB[3] = { c10, c12, c14 };

    float2 pixel = floor(uv / c0.xy + 0.25);
    float2 uvC = (pixel + 0.5) * c0.xy;
    float2 zc = tex2Dlod(sZ, float4(uv, 0, 0)).rg;
    float z = UnpackZ(zc);

    // The farthest any enabled scale goes. Past it, like sky, anything past the far plane and anything marked as not to
    // be occluded (the grass, the water, written as "past the fade"), the term is exactly 1 and there is nothing to
    // compute -- not even the normal.
    float reachEnd = 0;
    [unroll] for (int e = 0; e < 3; ++e)
    {
        reachEnd = max(reachEnd, scaleB[e].w > 0.5 ? c4.y * scaleB[e].y : 0);
    }

    [branch] if (z >= c4.y * 0.999 || z >= reachEnd) { return float4(1, 1, 1, 1); }

    float3 p = ViewPos(uvC, z);

    // Normal from the nearer side on each axis, so a silhouette does not tilt the normal of the pixel beside it. One
    // AO texel is the smallest step that lands on a different sample of the half-size distance.
    float2 t = c5.zw * lerp(1, c7.z, saturate(z / c7.w));
    float3 pr = PosAt(uvC + float2(t.x, 0));
    float3 pl = PosAt(uvC - float2(t.x, 0));
    float3 pd = PosAt(uvC + float2(0, t.y));
    float3 pu = PosAt(uvC - float2(0, t.y));
    float3 dx = abs(pr.z - p.z) < abs(p.z - pl.z) ? pr - p : p - pl;
    float3 dy = abs(pd.z - p.z) < abs(p.z - pu.z) ? pd - p : p - pu;
    float3 n = normalize(cross(dx, dy));
    // The eye is at the origin, so a surface that faces it has a normal pointing back along -p.
    n = dot(n, p) > 0 ? -n : n;

    float3 up = abs(n.y) < 0.99 ? float3(0, 1, 0) : float3(1, 0, 0);
    float3 tangent = normalize(cross(up, n));
    float3 bitangent = cross(n, tangent);

    // How many screen pixels a yard covers here; each scale turns this into its own reach in yards, so that the kernel
    // lands between its minimum and maximum size on screen: nearby surfaces get a tighter kernel (cheaper, and
    // proportioned to what is in front of the eye), distant ones a wider one (so they still read).
    float pixelsPerYard = c1.y * 0.5 * (c4.z / c0.y) / z;

    // Each AO texel rotates the kernel by an angle taken from a 3x3 tile of nine evenly spaced angles; without a
    // rotation the taps read as rings. The blur is a 3x3 box over the same texels, so every blur window holds each of
    // the nine rotations exactly once. That is what stops it flickering: a random or hashed rotation leaves residual
    // grain that changes as the scene slides under the fixed screen pattern, while a complete tile averages out the
    // same way wherever the window sits.
    float2 texel = floor(uv / c5.zw);
    float2 tile = texel - 3 * floor(texel / 3);
    float slot = tile.x + 3 * tile.y;

    // How broken-up the surface around this pixel is. Inverse depth is linear across a plane whatever its slope, so
    // the second difference of it across the pixel is ~0 on a wall, a floor or a roof, and large where the depth
    // jumps. Taken in four directions and reduced with min: a crease or an edge breaks the depth across it but not
    // along it, so it scores 0 and keeps all its occlusion, while a leaf canopy breaks it every way. Neighbours past
    // the fade carry no depth and count as flat.
    float clutter = 1;
    [unroll] for (int d = 0; d < 4; ++d)
    {
        float2 dir = d == 0 ? float2(1, 0) : (d == 1 ? float2(0, 1) : (d == 2 ? float2(1, 1) : float2(1, -1)));
        float2 off = dir * c5.zw * 3;
        float zf = ZAt(uvC + off);
        float zb = ZAt(uvC - off);
        float jump = abs(1 / zf + 1 / zb - 2 / z) * z;
        bool known = zf < c4.y * 0.999 && zb < c4.y * 0.999;
        clutter = min(clutter, known ? saturate((jump - 0.002) / 0.008) : 0);
    }

    float shape = lerp(c8.y, c8.x, clutter);
    float3 result = float3(1, 1, 1);

    [unroll] for (int s = 0; s < 3; ++s)
    {
        float4 A = scaleA[s];
        float4 B = scaleB[s];

        // A scale that is off, or past its own distance (fine detail is invisible far away, so the small one stops
        // first), costs nothing.
        [branch] if (B.w > 0.5 && z < c4.y * B.y)
        {
            float baseReach = c3.x * A.x;
            float reach = clamp(baseReach * pixelsPerYard, A.y, A.z) / pixelsPerYard;
            reach = min(reach, baseReach * A.w);
            float bias = c3.z * reach / c3.x + z * c6.w;

            float spin = frac(slot * (4.0 / 9.0) + B.z) * 6.2831853;
            float spinSin, spinCos;
            sincos(spin, spinSin, spinCos);

            float occlusion = 0;
            float weightSum = 0;

            [unroll] for (int i = 0; i < 5; ++i)
            {
                float4 k = kKernel[i];
                float2 disk = float2(k.x * spinCos - k.y * spinSin, k.x * spinSin + k.y * spinCos);
                float3 dir = tangent * disk.x + bitangent * disk.y + n * k.z;
                float3 tap = p + dir * (reach * k.w);

                float2 ndc = float2(tap.x / tap.z * c1.x + c1.z, tap.y / tap.z * c1.y + c1.w);
                float2 tapUv = float2(c0.z + (ndc.x * 0.5 + 0.5) * c3.w, c0.w + (0.5 - ndc.y * 0.5) * c4.z);

                bool inside = tap.z > 0.05
                           && tapUv.x >= c0.z && tapUv.x <= c0.z + c3.w
                           && tapUv.y >= c0.w && tapUv.y <= c0.w + c4.z;
                float sceneZ = ZAt(tapUv);

                // Occluded when the neighbour is in front of the tap. The range weight fades an occluder that is much
                // nearer than the kernel reaches, which is what removes the halo a silhouette would cast on the wall
                // behind. A tap that lands on an excluded surface does not occlude: blades are not geometry that
                // shades anything.
                float diff = tap.z - sceneZ;
                float range = smoothstep(0, 1, reach / max(abs(z - sceneZ), 1e-4));

                // How far in front of the tap the neighbour is, as a ramp: the whole step from clear to occluded takes
                // kSoftness of the tap's own distance from the surface, so a tap does not flip on a pixel's worth of
                // depth.
                float blocked = saturate((diff - bias) / (c6.z * reach * k.w));

                // A tap near the surface says more about a contact than one at the rim of the kernel.
                float tapWeight = lerp(c7.x, c7.y, k.w);

                occlusion += inside ? blocked * range * tapWeight : 0;
                weightSum += tapWeight;
            }

            float ao = 1 - c3.y * B.x * pow(saturate(occlusion * c6.x / weightSum * shape), c6.y);
            result[s] = lerp(ao, 1, saturate((z - c4.x * B.y) / ((c4.y - c4.x) * B.y)));
        }
    }

    return float4(result, 1);
}
)HLSL";

const char* const kBlurHLSL = R"HLSL(
sampler2D sAo : register(s0);
sampler2D sZ : register(s1);

float ZAt(float2 uv)
{
    return UnpackZ(tex2Dlod(sZ, float4(uv, 0, 0)).rg);
}

// The other eight texels of the 3x3 box around this one, exactly one texel apart (kBlurRadius 1).
static const float2 kOffsets[8] = {
    float2( 1,  0), float2(-1,  0), float2( 0,  1), float2( 0, -1),
    float2( 1,  1), float2(-1,  1), float2( 1, -1), float2(-1, -1)
};

// The one blur, and the last thing that touches the term before it is put on the frame. Alpha is not an AO scale: it
// marks the texels that sit on a depth edge (1) and the ones in the open (0), which is what lets the composite filter
// the term with a single hardware fetch almost everywhere and look at depths only along edges. The blur already knows:
// it is comparing the depth of nine texels anyway.
float4 main(float2 uv : TEXCOORD0) : COLOR0
{
    float3 centre = tex2Dlod(sAo, float4(uv, 0, 0)).rgb;
    float centreZ = ZAt(uv);

    // Sky, far terrain and excluded surfaces hold exactly 1 and every neighbour that could reach them is either 1 as
    // well or weighted out by the depth test, so the blur would return the texel unchanged. Skipping it is exact, and
    // those texels are most of a typical frame. They count as edge: the composite's filter has to look at the depths
    // wherever a pixel's footprint touches one of them.
    [branch] if (centreZ >= c4.y * 0.999)
    {
        return float4(1, 1, 1, 1);
    }

    // The box doubles its spacing with distance. Offsets of two texels still land on all three residues of the 3x3
    // rotation tile, so the grain keeps cancelling, while each tap reaches twice as far.
    float spacing = lerp(1, 2, saturate((centreZ - c4.x * 0.5) / max(c4.x, 1)));
    // All three scales are blurred together: they sit in the three colour channels, so the depth test and the fetch
    // are paid once, not three times.
    float3 sum = centre;
    float wsum = 1;
    float edge = 0;

    [unroll] for (int i = 0; i < 8; ++i)
    {
        float2 tapUv = uv + kOffsets[i] * c5.zw * c4.w * spacing;
        float3 tap = tex2Dlod(sAo, float4(tapUv, 0, 0)).rgb;
        float tapZ = ZAt(tapUv);

        // Bilateral weight: taps on the far side of a silhouette do not contribute, so blurring cannot paint a
        // dark fringe onto the wall behind a fence post.
        float apart = abs(tapZ - centreZ) / (c5.x * centreZ + 0.05);
        float w = saturate(1 - apart);
        sum += tap * w;
        wsum += w;
        edge = max(edge, apart);
    }

    return float4(sum / wsum, edge > 0.5 ? 1 : 0);
}
)HLSL";

// Brings the half-size term back to the frame's size. Each frame pixel takes the four AO texels around it, weighted
// by the usual bilinear share times how close that texel's depth is to the pixel's own, so the term does not leak
// across a silhouette. Also where the exclusion mask is applied at full resolution.
const char* const kCompositeHLSL = R"HLSL(
sampler2D sAo : register(s0);
sampler2D sDepth : register(s1);
sampler2D sExclude : register(s2);
sampler2D sZ : register(s3);

float4 main(float2 uv : TEXCOORD0) : COLOR0
{
    float2 pixel = floor(uv / c0.xy);

    // How much of this pixel's frame-wide term the character's own pass has taken over: c8.z (1 at full, falling to 0 as
    // the camera moves off) on the character's pixels, 0 anywhere else.
    float taken = 0;

    [branch] if (c5.y > 0.5)
    {
        // Red: grass and water, which take no part. Green: the character, when its own close-up pass is running --
        // the pixels it shades are not shaded twice.
        float2 mask = tex2Dlod(sExclude, float4(uv, 0, 0)).rg;
        if (mask.r > 0.5) { return float4(1, 1, 1, 1); }
        taken = mask.g > 0.5 ? c8.z : 0;
    }

    float centreZ = min(ViewZ(DepthAt(sDepth, uv)), c4.y);

    // Past the fade the term is exactly 1; the four taps below could only add the 0.001 leak of a nearer texel.
    [branch] if (centreZ >= c4.y * 0.999)
    {
        return float4(1, 1, 1, 1);
    }

    // AO texel j stands for frame pixel 2j + 1, so this pixel sits (pixel - 1) / 2 texels along.
    float2 t = (pixel - 1) * 0.5;

    // The common case, one fetch: the hardware's own bilinear filter over the four texels around the pixel, at exactly
    // the weights the careful path below uses. Its alpha is the blur's edge flag, filtered the same way, so anything
    // above zero means one of those four texels sits on a depth edge, where a plain filter would smear the term across
    // a silhouette. Open surfaces, which are nearly all of a frame, never leave this path.
    float4 filtered = tex2Dlod(sAo, float4((t + 0.5) * c5.zw, 0, 0));
    float3 term = filtered.rgb;

    [branch] if (filtered.a > 0.001)
    {
        // Along an edge: each of the four texels is weighted by the usual bilinear share times how close its depth is to
        // this pixel's own, so the term does not leak across a silhouette.
        float2 base = floor(t);
        float2 f = t - base;

        float3 sum = 0;
        float wsum = 0;

        [unroll] for (int i = 0; i < 4; ++i)
        {
            float2 corner = float2((i == 1 || i == 3) ? 1 : 0, i >= 2 ? 1 : 0);
            float2 texel = base + corner;
            float2 aoUv = (texel + 0.5) * c5.zw;
            float3 tap = tex2Dlod(sAo, float4(aoUv, 0, 0)).rgb;
            float tapZ = UnpackZ(tex2Dlod(sZ, float4(aoUv, 0, 0)).rg);

            float2 bil = lerp(1 - f, f, corner);
            float w = bil.x * bil.y * (saturate(1 - abs(tapZ - centreZ) / (c5.x * centreZ + 0.05)) + 0.001);
            sum += tap * w;
            wsum += w;
        }

        term = sum / max(wsum, 1e-6);
    }

    // The three scales stack by multiplication, each as an independent dimming of the light. c9 picks which of
    // them take part, for the debug views; a hidden scale is 1.
    float3 scales = lerp(float3(1, 1, 1), term, c9.xyz);
    float ao = scales.x * scales.y * scales.z;

    // The engine's own fog is already in the frame. A pixel that is mostly fog has no surface left to occlude, so the
    // term goes to 1 by the same linear factor the scene was fogged with; without it the distant haze, and the storm's
    // grey, is darkened below its own colour and the contrast climbs. c8.w / c9.w = fog start / end; none when the end is
    // not past the start.
    float fog = c9.w > c8.w + 1 ? saturate((centreZ - c8.w) / (c9.w - c8.w)) : 0;
    ao = lerp(ao, 1, fog);

    // The character's own pass has this much of the pixel; what it has not taken stays with this one, so the hand-over
    // as the camera moves away is a cross-fade and not a step.
    ao = lerp(ao, 1, taken);

    // In the rain the shading takes the fog's hue: c10.xyz is that colour as a multiplier of the same brightness (so
    // the luminance of the term is still ao) and c10.w how much of it, which is the rain. Where there is no occlusion
    // there is no tint, and it builds up with the occlusion. No rain, c10.w is 0 and this is the plain grey it was.
    float3 shade = ao * lerp(float3(1, 1, 1), c10.xyz, saturate((1 - ao) * 2) * c10.w);
    return float4(min(shade, 1), 1);
}
)HLSL";

// The close-up character pass. Everything here runs at the frame's own size on the pixels the character's batches
// stamped into the green channel of the exclusion mask, and nowhere else (the first line of each shader is the
// early-out), so its cost follows the character's size on screen rather than the frame's.
//
// It shares the common block and its constant layout with the frame-wide passes, plus
// c9  = { reach in yards, strength, bias in yards, max reach px }
// c10 = { min reach px, occlusion gain, curve power, tap softness }
// and, for the blur, c8 = { step x, step y, depth sigma, unused }.
//
// A twelve-tap kernel, rotated per pixel through a 4x4 tile of sixteen angles. The blur is two 4-tap passes over
// the same 4x4 window, so every window holds each of the sixteen rotations exactly once and the rotation cancels
// out, whatever the surface underneath is doing. That exactness is the point: a random rotation leaves grain that
// swims as the character moves.
const char* const kCharAoHLSL = R"HLSL(
sampler2D sDepth : register(s0);
sampler2D sMask : register(s1);

static const float4 kKernel[12] = {
    float4(0.20412, 0.00000, 0.97895, 0.26294),
    float4(-0.26070, 0.23882, 0.93541, 0.35157),
    float4(0.03990, -0.45469, 0.88976, 0.42809),
    float4(0.32859, 0.42859, 0.84163, 0.49854),
    float4(-0.60301, -0.10666, 0.79057, 0.56502),
    float4(0.57123, -0.36337, 0.73598, 0.62858),
    float4(-0.19106, 0.71075, 0.67700, 0.68986),
    float4(-0.36438, -0.70159, 0.61237, 0.74928),
    float4(0.79056, 0.28871, 0.54006, 0.80713),
    float4(-0.82244, 0.33949, 0.45644, 0.86363),
    float4(0.39647, -0.84724, 0.35355, 0.91895),
    float4(0.29298, 0.93407, 0.20412, 0.97322)
};

float ZAtUv(float2 uv)
{
    return ViewZ(DepthAt(sDepth, uv));
}

float3 PosAtUv(float2 uv)
{
    return ViewPos(uv, ZAtUv(uv));
}

float4 main(float2 uv : TEXCOORD0) : COLOR0
{
    // Alpha says whether the pixel is the character's; the blur and the composite read it.
    [branch] if (tex2Dlod(sMask, float4(uv, 0, 0)).g < 0.5) { return float4(1, 1, 1, 0); }

    float z = ZAtUv(uv);
    float3 p = ViewPos(uv, z);

    // The normal from the full-size depth: across the pixel where the surface is continuous, from the nearer side
    // where there is an edge.
    float2 o = c0.xy;
    float3 pr = PosAtUv(uv + float2(o.x, 0));
    float3 pl = PosAtUv(uv - float2(o.x, 0));
    float3 pd = PosAtUv(uv + float2(0, o.y));
    float3 pu = PosAtUv(uv - float2(0, o.y));
    float continuous = z * 0.004 + 0.002;
    float3 dx = abs(pr.z + pl.z - 2 * p.z) < continuous ? (pr - pl) * 0.5 : (abs(pr.z - p.z) < abs(p.z - pl.z) ? pr - p : p - pl);
    float3 dy = abs(pd.z + pu.z - 2 * p.z) < continuous ? (pd - pu) * 0.5 : (abs(pd.z - p.z) < abs(p.z - pu.z) ? pd - p : p - pu);
    float3 n = normalize(cross(dx, dy));
    n = dot(n, p) > 0 ? -n : n;

    float3 up = abs(n.y) < 0.99 ? float3(0, 1, 0) : float3(1, 0, 0);
    float3 tangent = normalize(cross(up, n));
    float3 bitangent = cross(n, tangent);

    // The reach in yards, held between a minimum and a maximum number of screen pixels, as in the frame-wide pass.
    float pixelsPerYard = c1.y * 0.5 * (c4.z / c0.y) / z;
    float reach = clamp(c9.x * pixelsPerYard, c10.x, c9.w) / pixelsPerYard;
    reach = min(reach, c9.x * 3);
    float bias = c9.z + z * 0.002;

    float2 pixel = floor(uv / c0.xy);
    float2 tile = pixel - 4 * floor(pixel / 4);
    float spin = frac((tile.x + 4 * tile.y) * (7.0 / 16.0)) * 6.2831853;
    float spinSin, spinCos;
    sincos(spin, spinSin, spinCos);

    float occlusion = 0;
    float weightSum = 0;

    [unroll] for (int i = 0; i < 12; ++i)
    {
        float4 k = kKernel[i];
        float2 disk = float2(k.x * spinCos - k.y * spinSin, k.x * spinSin + k.y * spinCos);
        float3 dir = tangent * disk.x + bitangent * disk.y + n * k.z;
        float3 tap = p + dir * (reach * k.w);

        float2 ndc = float2(tap.x / tap.z * c1.x + c1.z, tap.y / tap.z * c1.y + c1.w);
        float2 tapUv = float2(c0.z + (ndc.x * 0.5 + 0.5) * c3.w, c0.w + (0.5 - ndc.y * 0.5) * c4.z);
        bool inside = tap.z > 0.05
                   && tapUv.x >= c0.z && tapUv.x <= c0.z + c3.w
                   && tapUv.y >= c0.w && tapUv.y <= c0.w + c4.z;
        float sceneZ = ZAtUv(tapUv);

        float diff = tap.z - sceneZ;
        float range = smoothstep(0, 1, reach / max(abs(z - sceneZ), 1e-4));
        float blocked = saturate((diff - bias) / (c10.w * reach * k.w));
        float tapWeight = lerp(0.7, 0.3, k.w);

        occlusion += inside ? blocked * range * tapWeight : 0;
        weightSum += tapWeight;
    }

    float ao = 1 - c9.y * pow(saturate(occlusion * c10.y / weightSum), c10.z);

    // Faded out by the engine's fog, as in the frame-wide composite. c5.x / c5.y = fog start / end.
    float fog = c5.y > c5.x + 1 ? saturate((z - c5.x) / (c5.y - c5.x)) : 0;
    ao = lerp(ao, 1, fog);
    return float4(ao, ao, ao, 1);
}
)HLSL";

// One axis of the 4-pixel window {-2, -1, 0, +1}, bilateral on the full-size depth and on the character mask (a
// neighbour that is not the character has alpha 0 and no weight). Run along x, then along y.
const char* const kCharBlurHLSL = R"HLSL(
sampler2D sAo : register(s0);
sampler2D sDepth : register(s1);

float4 main(float2 uv : TEXCOORD0) : COLOR0
{
    float4 centre = tex2Dlod(sAo, float4(uv, 0, 0));
    [branch] if (centre.a < 0.5) { return centre; }

    float centreZ = ViewZ(DepthAt(sDepth, uv));
    float sum = centre.r;
    float wsum = 1;

    [unroll] for (int i = 0; i < 3; ++i)
    {
        float offset = i == 0 ? -2 : (i == 1 ? -1 : 1);
        float2 tapUv = uv + c8.xy * offset;
        float4 tap = tex2Dlod(sAo, float4(tapUv, 0, 0));
        float tapZ = ViewZ(DepthAt(sDepth, tapUv));
        float w = tap.a * saturate(1 - abs(tapZ - centreZ) / (c8.z * centreZ + 0.02));
        sum += tap.r * w;
        wsum += w;
    }

    float v = sum / wsum;
    return float4(v, v, v, 1);
}
)HLSL";

// Multiplied onto the frame by the same blend the frame-wide composite uses. Pixels that are not the character's
// are clipped, so this never touches what the frame-wide pass shaded (and the debug blit leaves them alone).
const char* const kCharCompositeHLSL = R"HLSL(
sampler2D sAo : register(s0);

float4 main(float2 uv : TEXCOORD0) : COLOR0
{
    float4 t = tex2Dlod(sAo, float4(uv, 0, 0));
    clip(t.a - 0.5);

    // The rain's tint, exactly as in the frame-wide composite (c10.xyz the hue, c10.w how much).
    float3 shade = t.r * lerp(float3(1, 1, 1), c10.xyz, saturate((1 - t.r) * 2) * c10.w);
    return float4(min(shade, 1), 1);
}
)HLSL";

// The decode terms for the stored depth. shortcut: duplicated from water_renderer.cpp's MapWorldDepth, move to a
// shared header at the third consumer. See there for why the viewport's z range is folded in as it is.
struct DepthMapping {
    float atInfinity;
    float perInverseViewDepth;
    float deepest;
};

DepthMapping MapWorldDepth(const float* m, float viewportMinZ, float viewportMaxZ) {
    const bool usableRange = viewportMaxZ - viewportMinZ > kMinViewportDepthExtent && viewportMinZ >= 0.0f && viewportMaxZ <= 1.0f;
    const float minZ = usableRange ? viewportMinZ : 0.0f;
    const float extent = usableRange ? viewportMaxZ - viewportMinZ : 1.0f;
    const float maxZ = minZ + extent;
    const float deepest = maxZ >= kDeepestWorldDepthInFullRangeViewport ? kDeepestWorldDepthInFullRangeViewport : maxZ + kWorldDepthMargin;

    return { minZ + extent * m[10], extent * m[14], std::max(deepest, kSkyMinDepth) };
}

// Compiles one of the shaders above. The same pattern as the unit outline's: optimised, because a failed compile
// leaves the shader null and the whole module silently draws nothing, and no CGxDevice::Log on the failure path,
// which is exactly when the process can least afford a varargs logger.
CGxShader* CompilePixelShader(const char* prefix, const char* body) {
    const size_t prefixLength = strlen(prefix);
    const size_t length = prefixLength + strlen(body);
    char* source = new char[length + 1];
    memcpy(source, prefix, prefixLength);
    memcpy(source + prefixLength, body, strlen(body) + 1);

    ID3DBlob* code = nullptr;
    ID3DBlob* error = nullptr;
    HRESULT result = D3DCompile(source, length, "AmbientOcclusion", nullptr, nullptr, "main", "ps_3_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &error);
    delete[] source;

    if (FAILED(result)) {
        FILE* file = DiagnosticLogsEnabled() ? fopen("Logs\\ambientocclusion.log", "a") : nullptr;

        if (file) {
            fputs("AmbientOcclusion: a pixel shader failed to compile\n", file);

            if (error) {
                fwrite(error->GetBufferPointer(), 1, error->GetBufferSize(), file);
                fputc('\n', file);
            }

            fclose(file);
        }

        if (error) {
            error->Release();
        }

        if (code) {
            code->Release();
        }

        return nullptr;
    }

    if (error) {
        error->Release();
    }

    CGxShader* shader = new (__FILE__, __LINE__) CGxShader();
    shader->m_target = GxSh_Pixel;
    shader->m_refCount = 1;
    shader->m_code.SetCount(static_cast<uint32_t>(code->GetBufferSize()));
    memcpy(shader->m_code.m_data, code->GetBufferPointer(), code->GetBufferSize());
    code->Release();

    return shader;
}

void RefreshGxTexture(FFX::Tex& tex, CGxTex*& gxTexture) {
    gxTexture = tex.m_texture ? TextureGetGxTex(tex.m_texture, 1, nullptr) : nullptr;
}

// A frame-sized render target, exactly the frame's size: the occupancy shader samples the scene depth with the
// same uv it writes with, so a rounded-up texture would read the depth at the wrong scale. Copied from the unit
// outline's CreateFrameTex; FFX::CreateTex is not used because it rounds up to a power of two.
bool CreateFrameTex(FFX::Tex& tex, const C2iVector& size, bool nearest = false) {
    EGxTexFormat format;

    if (g_theGxDevicePtr->Caps().m_rttFormat[GxTex_Argb8888]) {
        format = GxTex_Argb8888;
    } else if (g_theGxDevicePtr->Caps().m_rttFormat[GxTex_Abgr8888]) {
        format = GxTex_Abgr8888;
    } else {
        return false;
    }

    // GxTexCreate refuses a non-power-of-two size on GxTex_2d, so the non-pow2 target is the only one that can
    // hold the frame's exact size. The depth sharing needs it too, so a device without it never gets here.
    if (g_theGxDevicePtr->Caps().m_texTarget[GxTex_NonPow2] != 1 || size.x < 8 || size.y < 8) {
        return false;
    }

    int32_t maxSize = static_cast<int32_t>(g_theGxDevicePtr->Caps().m_texMaxSize[GxTex_NonPow2]);

    if (size.x > maxSize || size.y > maxSize) {
        return false;
    }

    if (!tex.m_texture || tex.m_texSize.x != size.x || tex.m_texSize.y != size.y) {
        if (tex.m_texture) {
            HandleClose(reinterpret_cast<HOBJECT>(tex.m_texture));
            tex.m_texture = nullptr;
        }

        CGxTexFlags flags(nearest ? GxTex_Nearest : GxTex_Linear, 0, 0, 0, 0, 1, 1, 0, 0, 0);
        tex.m_texture = TextureCreate(GxTex_NonPow2, size.x, size.y, 0, format, format, flags, nullptr, GxTexNullUserFunc, "AmbientOcclusion::CreateFrameTex", 0);

        if (!tex.m_texture) {
            return false;
        }

        tex.m_texSize = size;
    }

    tex.m_size = size;
    tex.m_texelSize.x = 1.0f / size.x;
    tex.m_texelSize.y = 1.0f / size.y;

    // The handle is non-null even when the device refused the texture, so the gx texture is what says whether
    // this actually worked.
    return TextureGetGxTex(tex.m_texture, 1, nullptr) != nullptr;
}

// The quad every pass draws through, built as the engine's own full-screen passes build theirs.
void DrawFullscreenQuad(const FFX::Tex& target) {
    C3Vector positions[4];
    C2Vector texcoords[4];
    C2Vector offsets[4] = { { 0.0f, 0.0f }, { 0.0f, 0.0f }, { 0.0f, 0.0f }, { 0.0f, 0.0f } };
    C2Vector scale = { 0.0f, 0.0f };

    FFX::SetupGeo_Quad(target.m_size, target.m_size, target.m_texSize, positions, texcoords, false);

    CImVector white(0xFF, 0xFF, 0xFF, 0xFF);
    FFX::SetupVB_PCT4(positions, white, texcoords, offsets, scale);

    g_theGxDevicePtr->PrimVertexMask(0x3D1);
    FFX::SetupIB();
    g_theGxDevicePtr->Draw(&FFX::s_quadBatch, 1);
}

// How much of the character's pass is in force at this camera distance: 1 up to fullDistance, then an eased fall to 0
// over kCharFadeYards.
float CharFade(float cameraDistance, float fullDistance) {
    const float t = std::min(std::max((cameraDistance - fullDistance) / kCharFadeYards, 0.0f), 1.0f);
    return 1.0f - t * t * (3.0f - 2.0f * t);
}

// Set when a shader failed to compile. Compiling is expensive, so a failure is not retried every frame -- that
// turned a broken shader into a large frame-rate loss on top of the missing effect. Cleared by a device restore.
bool s_compileFailed;

// The same for the character pass's own shaders, kept apart so a failure there costs only the character pass and not
// the frame-wide one.
bool s_charCompileFailed;

}  // namespace

CVar* CAmbientOcclusion::s_cvarSsao;
CVar* CAmbientOcclusion::s_cvarSpread;
CVar* CAmbientOcclusion::s_cvarStrength;
CVar* CAmbientOcclusion::s_cvarDistance;
CVar* CAmbientOcclusion::s_cvarCharacter;
CVar* CAmbientOcclusion::s_cvarCharDistance;
CGxShader* CAmbientOcclusion::s_charAoPS;
CGxShader* CAmbientOcclusion::s_charBlurPS;
CGxShader* CAmbientOcclusion::s_charCompositePS;
FFX::Tex CAmbientOcclusion::s_charAoTex;
FFX::Tex CAmbientOcclusion::s_charBlurTex;
CGxTex* CAmbientOcclusion::s_charAo;
CGxTex* CAmbientOcclusion::s_charBlur;
bool CAmbientOcclusion::s_charStamped;
bool CAmbientOcclusion::s_submerged;
float CAmbientOcclusion::s_rain;
float CAmbientOcclusion::s_fogStart;
float CAmbientOcclusion::s_fogEnd;
float CAmbientOcclusion::s_fogColor[3] = { 1.0f, 1.0f, 1.0f };
float CAmbientOcclusion::s_cameraDistance = 1.0e9f;
CGxShader* CAmbientOcclusion::s_zPS;
CGxShader* CAmbientOcclusion::s_aoPS;
CGxShader* CAmbientOcclusion::s_blurPS;
CGxShader* CAmbientOcclusion::s_compositePS;
CGxShader* CAmbientOcclusion::s_excludePS;
FFX::Tex CAmbientOcclusion::s_excludeTex;
CGxTex* CAmbientOcclusion::s_exclude;
bool CAmbientOcclusion::s_excludeStamped;
bool CAmbientOcclusion::s_stamping;

// Pixel shader constant c0 as the caller had it, put back by EndExcludeStamps. RsPush/RsPop do not cover shader
// constants, and the stamp's alpha cutoff would otherwise be left in c0 for whatever draws next (the stock water and
// the effects after it read constants they set earlier and do not set again).
static float s_savedPixelC0[4];
FFX::Tex CAmbientOcclusion::s_zTex;
CGxTex* CAmbientOcclusion::s_z;
FFX::Tex CAmbientOcclusion::s_aoTex;
FFX::Tex CAmbientOcclusion::s_aoBlurTex;
CGxTex* CAmbientOcclusion::s_ao;
CGxTex* CAmbientOcclusion::s_aoBlur;

// Not in the original binary. Called when a world frame is created, which happens again after a UI reload, so
// the callback registration is guarded: the device's list is append-only and OnDeviceRestored is not safe to run
// twice.
void CAmbientOcclusion::Init() {
    static bool s_registered;

    // Registered by CWorldParam::Initialize so the options panel can read its default at the login screen. This
    // call is the safety net, and Register reuses an existing var. Unlike antiAlias this defaults to on.
    s_cvarSsao = CVar::Register("ssao", "screen-space ambient occlusion: 0 off, 1 on, 2 show the occlusion term, 3/4/5 show only the small/medium/big scale",
                                1, "1", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);

    // The two knobs of the look, registered here as well as by CWorldParam::Initialize for the same reason.
    s_cvarSpread = CVar::Register("ssaoSpread", "ambient occlusion spread, as a multiple of the default reach",
                                  1, "0.06", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
    s_cvarStrength = CVar::Register("ssaoStrength", "ambient occlusion strength: 0 none, 1 full",
                                    1, "0.5", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);

    s_cvarDistance = CVar::Register("ssaoDistance", "ambient occlusion fades out and stops at this distance, in yards",
                                    1, "145", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);

    s_cvarCharacter = CVar::Register("ssaoCharacter", "dedicated high-quality ambient occlusion for the player character when the camera is close",
                                     1, "1", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
    s_cvarCharDistance = CVar::Register("ssaoCharDistance", "the character's own ambient occlusion is at full strength while the camera is closer than this, in yards, then fades out over the next few",
                                        1, "6", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);

    if (s_registered) {
        return;
    }

    s_registered = true;
    g_theGxDevicePtr->AddDeviceRestoredCallback(CAmbientOcclusion::OnDeviceRestored);
}

bool CAmbientOcclusion::Enabled() {
    return s_cvarSsao && s_cvarSsao->m_intValue > 0 && !s_submerged && g_theGxDevicePtr && g_theGxDevicePtr->m_context;
}

// Not in the original binary. See the declaration.
void CAmbientOcclusion::SetSubmerged(bool submerged) {
    s_submerged = submerged;
}

// Not in the original binary. See the declaration.
void CAmbientOcclusion::SetRain(float amount) {
    s_rain = std::min(std::max(amount, 0.0f), 1.0f);
}

// Not in the original binary. See the declaration.
void CAmbientOcclusion::SetFogColor(float r, float g, float b) {
    s_fogColor[0] = r;
    s_fogColor[1] = g;
    s_fogColor[2] = b;
}

// Not in the original binary. See the declaration. The engine parks its fog at a million yards when the depth-fog pass
// replaces it; anything that far out is no fog at all as far as this pass is concerned.
void CAmbientOcclusion::SetFog(float start, float end) {
    const bool usable = end > start + 1.0f && end < 100000.0f && start >= 0.0f;
    s_fogStart = usable ? start : 0.0f;
    s_fogEnd = usable ? end : 0.0f;
}

// Not in the original binary. See the declaration.
bool CAmbientOcclusion::Active() {
    return Enabled() && CUnitOutline::IsDepthPassBound();
}

// Not in the original binary. A device reset frees the shaders; the AO targets are default-pool render targets
// the device re-created itself, so only the cached pointers are stale. ShaderDestroy re-nulls what it is given.
void CAmbientOcclusion::OnDeviceRestored() {
    if (s_zPS) { g_theGxDevicePtr->ShaderDestroy(s_zPS); }
    if (s_aoPS) { g_theGxDevicePtr->ShaderDestroy(s_aoPS); }
    if (s_blurPS) { g_theGxDevicePtr->ShaderDestroy(s_blurPS); }
    if (s_compositePS) { g_theGxDevicePtr->ShaderDestroy(s_compositePS); }
    if (s_excludePS) { g_theGxDevicePtr->ShaderDestroy(s_excludePS); }
    if (s_charAoPS) { g_theGxDevicePtr->ShaderDestroy(s_charAoPS); }
    if (s_charBlurPS) { g_theGxDevicePtr->ShaderDestroy(s_charBlurPS); }
    if (s_charCompositePS) { g_theGxDevicePtr->ShaderDestroy(s_charCompositePS); }

    s_charCompileFailed = false;
    s_charStamped = false;
    RefreshGxTexture(s_charAoTex, s_charAo);
    RefreshGxTexture(s_charBlurTex, s_charBlur);
    s_compileFailed = false;
    RefreshGxTexture(s_excludeTex, s_exclude);
    s_excludeStamped = false;
    RefreshGxTexture(s_zTex, s_z);
    RefreshGxTexture(s_aoTex, s_ao);
    RefreshGxTexture(s_aoBlurTex, s_aoBlur);
}

bool CAmbientOcclusion::EnsureResources() {
    if (!Enabled()) {
        return false;
    }

    if (g_theGxDevicePtr->DeviceApi() != GxApi_D3d9 && g_theGxDevicePtr->DeviceApi() != GxApi_D3d9Ex) {
        // The shaders are D3D9 bytecode, and the depth sharing is a D3D9 concept.
        return false;
    }

    // The scene depth can only be sampled when it is INTZ; the D24X8 fallback is unreadable.
    if (!g_theGxDevicePtr->Caps().m_rttFormat[GxTex_INTZ]) {
        return false;
    }

    if (s_compileFailed) {
        return false;
    }

    if (!s_zPS) { s_zPS = CompilePixelShader(kCommonHLSL, kZHLSL); }
    if (!s_aoPS) { s_aoPS = CompilePixelShader(kCommonHLSL, kAoHLSL); }
    if (!s_blurPS) { s_blurPS = CompilePixelShader(kCommonHLSL, kBlurHLSL); }
    if (!s_compositePS) { s_compositePS = CompilePixelShader(kCommonHLSL, kCompositeHLSL); }
    if (!s_excludePS) { s_excludePS = CompilePixelShader("", kExcludeHLSL); }

    if (!s_zPS || !s_aoPS || !s_blurPS || !s_compositePS || !s_excludePS) {
        s_compileFailed = true;
        return false;
    }

    return true;
}

// Not in the original binary. See the declaration.
bool CAmbientOcclusion::BeginExcludeStamps(bool ignoreAlpha) {
    return BeginStamps(ignoreAlpha, kMaskRed);
}

// Not in the original binary. Modelled on CUnitOutline::StampMask, except that the render target is switched once
// for a whole run of stamps instead of once per stamp. The mask is one texture shared by what is excluded (red) and
// by the character (green); channelMask is the engine's colour-write bits for the one this run writes.
bool CAmbientOcclusion::BeginStamps(bool ignoreAlpha, int32_t channelMask) {
    if (!Active() || !EnsureResources()) {
        return false;
    }

    // The mask is in the default back buffer's pixel space, like the depth it is tested against.
    CGxTex* oldTarget = nullptr;
    g_theGxDevicePtr->DeviceGetRenderTarget(GxBuffers_Color, oldTarget);

    if (oldTarget) {
        return false;
    }

    const CRect& window = g_theGxDevicePtr->DeviceCurWindow();
    C2iVector size = { static_cast<int32_t>(window.maxX), static_cast<int32_t>(window.maxY) };

    if (!CreateFrameTex(s_excludeTex, size)) {
        return false;
    }

    RefreshGxTexture(s_excludeTex, s_exclude);

    if (!s_exclude) {
        return false;
    }

    const int32_t alphaRef = g_theGxDevicePtr->m_appRenderStates[GxRs_AlphaRef].m_value.m_data.i[0];
    // Below zero nothing is ever clipped: the whole surface is stamped, whatever its texture's alpha says.
    const float cutoff[4] = { ignoreAlpha ? -1.0f : static_cast<float>(alphaRef) * (1.0f / 255.0f), 0.0f, 0.0f, 0.0f };

    // Render targets live outside RsPush/RsPop, so the caller's target is put back by hand in EndExcludeStamps.
    // The world's viewport is left alone: the mask is drawn through the same projection and viewport as the
    // frame, so its pixels land where the frame's do.
    g_theGxDevicePtr->RsPush();

    GxRenderTargetSet(GxBuffers_Color, s_exclude, 0);

    if (!s_excludeStamped) {
        g_theGxDevicePtr->SceneClear(GxBufferBit_Color, CImVector(0, 0, 0, 0));
        s_excludeStamped = true;
    }

    // One flat value per pixel, so no blending. The depth test stays as the caller has it, and nothing is written
    // to the depth: the caller's real draw follows and is what puts the blades in it.
    GxRsSet(GxRs_BlendingMode, GxBlend_Opaque);
    GxRsSet(GxRs_Lighting, 0);
    GxRsSet(GxRs_Fog, 0);
    GxRsSet(GxRs_DepthWrite, 0);
    GxRsSet(GxRs_ColorWrite, channelMask);
    GxRsSet(GxRs_PixelShader, s_excludePS);
    memcpy(s_savedPixelC0, g_theGxDevicePtr->ShaderConstantsLock(GxSh_Pixel), sizeof(s_savedPixelC0));
    GxShaderConstantsSet(GxSh_Pixel, 0, cutoff, 1);

    s_stamping = true;
    return true;
}

void CAmbientOcclusion::StampExcluded(const CGxBatch& batch) {
    if (!s_stamping) {
        return;
    }

    g_theGxDevicePtr->Draw(const_cast<CGxBatch*>(&batch), 1);
}

void CAmbientOcclusion::EndExcludeStamps() {
    if (!s_stamping) {
        return;
    }

    s_stamping = false;
    g_theGxDevicePtr->RsPop();
    GxRenderTargetSet(GxBuffers_Color, nullptr, 0);
    GxShaderConstantsSet(GxSh_Pixel, 0, s_savedPixelC0, 1);
}

// Not in the original binary. See the declaration. One surface at a time, so the target is switched in and out
// around each; a view has a handful of water surfaces at most.
void CAmbientOcclusion::StampLiquidBatch(const CGxBatch& batch) {
    if (!Enabled()) {
        return;
    }

    if (BeginExcludeStamps(true)) {
        StampExcluded(batch);
        EndExcludeStamps();
    }
}

// Not in the original binary. See the declaration.
void CAmbientOcclusion::SetCameraDistance(float yards) {
    s_cameraDistance = yards;
}

// Not in the original binary. See the declaration.
bool CAmbientOcclusion::CharacterWanted() {
    if (!Enabled() || !s_cvarCharacter || s_cvarCharacter->m_intValue <= 0) {
        return false;
    }

    // Wanted until it has faded out completely; past that it is off, and nothing is stamped or drawn for it.
    const float distance = s_cvarCharDistance ? s_cvarCharDistance->m_floatValue : kCharDistance;
    return CharFade(s_cameraDistance, std::min(std::max(distance, 1.0f), 60.0f)) > 0.0f;
}

// Not in the original binary. See the declaration. A batch at a time, like the liquid stamps: the character is a few
// dozen batches and only gets here while the camera is close.
void CAmbientOcclusion::StampCharacter(const CGxBatch& batch, CM2Model* model) {
    if (!CharacterWanted() || !CUnitOutline::IsPlayerModel(model)) {
        return;
    }

    // What the batch is, as the unit outline reads it: anything but a body, a cutout or a blended surface is an effect
    // or an attachment glow, whose quads are not the character's shape.
    const int32_t blend = g_theGxDevicePtr->m_appRenderStates[GxRs_BlendingMode].m_value.m_data.i[0];

    if (blend != GxBlend_Opaque && blend != GxBlend_AlphaKey && blend != GxBlend_Alpha) {
        return;
    }

    if (BeginStamps(false, kMaskGreen)) {
        StampExcluded(batch);
        EndExcludeStamps();
        s_charStamped = true;
    }
}

// Not in the original binary. Compiled the first time a close-up frame needs them, apart from the frame-wide shaders.
bool CAmbientOcclusion::EnsureCharacterResources() {
    if (s_charCompileFailed) {
        return false;
    }

    if (!s_charAoPS) { s_charAoPS = CompilePixelShader(kCommonHLSL, kCharAoHLSL); }
    if (!s_charBlurPS) { s_charBlurPS = CompilePixelShader(kCommonHLSL, kCharBlurHLSL); }
    if (!s_charCompositePS) { s_charCompositePS = CompilePixelShader(kCommonHLSL, kCharCompositeHLSL); }

    if (!s_charAoPS || !s_charBlurPS || !s_charCompositePS) {
        s_charCompileFailed = true;
        return false;
    }

    return true;
}

// Not in the original binary. The world -> interface boundary; see the header for the order and why.
void CAmbientOcclusion::Apply() {
    // Whether the mask was written this frame is consumed here, whatever the pass then decides: a stale mask from
    // a frame that stood down must not be read by the next one.
    const bool excludeStamped = s_excludeStamped;
    s_excludeStamped = false;
    const bool charStamped = s_charStamped;
    s_charStamped = false;

    if (!EnsureResources()) {
        return;
    }

    // The depth is only this frame's world depth when the shared-depth bind succeeded. A multisampled frame never
    // gets one (see CUnitOutline::OcclusionSupported), so this is also what stands the pass down under MSAA.
    if (!CUnitOutline::HasFrameDepth() || g_theGxDevicePtr->m_format.m_multisampleCount > 1) {
        return;
    }

    // The shared depth belongs to the default back buffer. If the world drew somewhere else -- the glow buffer --
    // the depth does not describe it.
    CGxTex* boundTarget = nullptr;
    g_theGxDevicePtr->DeviceGetRenderTarget(GxBuffers_Color, boundTarget);

    if (boundTarget) {
        return;
    }

    CGxTex* depth = CUnitOutline::DepthPassTexture();
    const CRect& window = g_theGxDevicePtr->DeviceCurWindow();
    C2iVector size = { static_cast<int32_t>(window.maxX), static_cast<int32_t>(window.maxY) };

    // Never sample a depth of another size.
    if (!depth || static_cast<int32_t>(depth->m_width) != size.x || static_cast<int32_t>(depth->m_height) != size.y) {
        return;
    }

    // The world's own projection and viewport, read before FFX::SetupGx replaces them with its pixel-space ones.
    float minX = 0.0f, maxX = 0.0f, minY = 0.0f, maxY = 0.0f, minZ = 0.0f, maxZ = 0.0f;
    GxXformViewport(minX, maxX, minY, maxY, minZ, maxZ);

    if (maxX - minX <= 0.001f || maxY - minY <= 0.001f) {
        return;
    }

    const C44Matrix projection = g_theGxDevicePtr->m_projNative;
    const float* m = reinterpret_cast<const float*>(&projection);

    if (m[0] == 0.0f || m[5] == 0.0f) {
        return;
    }

    // The term lives at a fraction of the frame's size. Rounded up, so an odd frame still has a texel for its last
    // column; the shaders address it in the frame's own uv, so the exact size only has to be consistent.
    const C2iVector aoSize = { (size.x + kResolutionDivisor - 1) / kResolutionDivisor, (size.y + kResolutionDivisor - 1) / kResolutionDivisor };

    if (!CreateFrameTex(s_zTex, aoSize, true) || !CreateFrameTex(s_aoTex, aoSize) || !CreateFrameTex(s_aoBlurTex, aoSize)) {
        return;
    }

    RefreshGxTexture(s_zTex, s_z);
    RefreshGxTexture(s_aoTex, s_ao);
    RefreshGxTexture(s_aoBlurTex, s_aoBlur);

    const bool useExclude = excludeStamped && s_exclude;

    if (!s_z || !s_ao || !s_aoBlur) {
        return;
    }

    const int32_t mode = s_cvarSsao->m_intValue;
    const DepthMapping mapping = MapWorldDepth(m, minZ, maxZ);

    // The player's two knobs scale the compiled-in defaults. Clamped here, so a hand-edited Config.wtf cannot hand
    // the shader a radius of zero (no occlusion at all) or one wide enough to shade the whole screen.
    float spread = s_cvarSpread ? s_cvarSpread->m_floatValue : 0.06f;
    float strength = s_cvarStrength ? s_cvarStrength->m_floatValue : kStrength;
    spread = std::min(std::max(spread, 0.025f), 0.4f) * kSpreadScale;
    strength = std::min(std::max(strength, 0.0f), 1.0f);
    // In the rain, the same strength with half the bite. Applied to the frame-wide passes and the character's alike.
    strength *= 1.0f - kRainStrengthLoss * s_rain;

    float distance = s_cvarDistance ? s_cvarDistance->m_floatValue : kDistance;
    distance = std::min(std::max(distance, 10.0f), 500.0f);

    // The rain's tint: the fog's hue as a multiplier of the same brightness (its colour over its luminance, pushed out by
    // kRainTintBoost), and the rain as how much of it.
    const float fogLuma = std::max(0.299f * s_fogColor[0] + 0.587f * s_fogColor[1] + 0.114f * s_fogColor[2], 0.05f);
    float tintRow[4];

    for (int32_t channel = 0; channel < 3; ++channel) {
        tintRow[channel] = std::min(std::max(1.0f + (s_fogColor[channel] / fogLuma - 1.0f) * kRainTintBoost, 0.35f), 1.7f);
    }

    tintRow[3] = s_rain;

    // The close-up character pass: wanted, something was stamped, and its own targets and shaders are there. The
    // debug views of single scales (3 to 5) leave the character white, so they stay about the frame-wide pass.
    bool useChar = charStamped && useExclude && mode <= 2 && CharacterWanted() && EnsureCharacterResources()
                && CreateFrameTex(s_charAoTex, size, true) && CreateFrameTex(s_charBlurTex, size, true);

    if (useChar) {
        RefreshGxTexture(s_charAoTex, s_charAo);
        RefreshGxTexture(s_charBlurTex, s_charBlur);
        useChar = s_charAo && s_charBlur;
    }

    // How much of the character is its own pass's: full inside ssaoCharDistance, fading to none over kCharFadeYards.
    const float charFullDistance = s_cvarCharDistance ? s_cvarCharDistance->m_floatValue : kCharDistance;
    const float charFade = useChar ? CharFade(s_cameraDistance, std::min(std::max(charFullDistance, 1.0f), 60.0f)) : 0.0f;

    const float constants[kConstantRows * 4] = {
        1.0f / size.x, 1.0f / size.y, minX, minY,
        m[0], m[5], m[8], m[9],
        mapping.atInfinity, mapping.perInverseViewDepth, mapping.deepest, kMaxViewDepth,
        kRadius * spread, strength, kBias * spread, maxX - minX,
        distance * kFadeStartFraction, distance, maxY - minY, kBlurRadius,
        kBlurDepthSigma, useExclude ? 1.0f : 0.0f, 1.0f / aoSize.x, 1.0f / aoSize.y,
        kOcclusionGain, kCurvePower, kSoftness, kBiasPerYard,
        kNearTapWeight, kFarTapWeight, kFarNormalSpan, kNormalFarYards,
        kFoliageKeep, kSmoothBoost, charFade, s_fogStart,
    };

    // The engine's own full-screen setup: pixel-space ortho projection, identity view and world, the AO target as
    // the colour target, and the render-state push that covers everything set from here on.
    FFX::SetupGx(&s_zTex);
    GxXformSetViewport(0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 1.0f);

    // The quad is transformed by the fixed-function pipeline, so a vertex shader left over from the world's last
    // batch must not transform it first.
    GxRsSet(GxRs_VertexShader, nullptr);
    GxRsSet(GxRs_BlendingMode, GxBlend_Opaque);
    GxRsSet(GxRs_ColorWrite, 15);
    GxRsSet(GxRs_AlphaRef, 0);

    GxShaderConstantsSet(GxSh_Pixel, 0, constants, kConstantRows);

    // View distance at half size, with the exclusion mask folded in.
    CGxTex* mask = useExclude ? s_exclude : depth;
    GxRsSet(GxRs_PixelShader, s_zPS);
    GxRsSet(GxRs_Texture0, depth);
    // Stage 1 is always a real texture, even when nothing was stamped: the shader's flag says whether to read it.
    GxRsSet(GxRs_Texture1, mask);
    DrawFullscreenQuad(s_zTex);

    // Occupancy: one draw for all three scales. Their settings go in as two rows each, and each lands in its own colour
    // channel of the AO target (small red, medium green, big blue); a scale that is off leaves its channel at 1. The
    // reads are all from the half-size distance, never from the target being written.
    float scaleConstants[kPassRows * 4];

    for (int32_t pass = 0; pass < kPassCount; ++pass) {
        const PassScale& scale = kPasses[pass];
        float* row = &scaleConstants[pass * 8];

        row[0] = scale.reachScale;
        row[1] = scale.minPixels;
        row[2] = scale.maxPixels;
        row[3] = scale.maxGrowth;
        row[4] = scale.strength;
        row[5] = scale.distance;
        row[6] = scale.spin;
        row[7] = scale.enabled ? 1.0f : 0.0f;
    }

    GxRenderTargetSet(GxBuffers_Color, s_ao, 0);
    GxRsSet(GxRs_PixelShader, s_aoPS);
    GxRsSet(GxRs_Texture0, s_z);
    GxShaderConstantsSet(GxSh_Pixel, kPassRow, scaleConstants, kPassRows);
    DrawFullscreenQuad(s_aoTex);

    // Blur, all three scales at once, and the one blur: a single 3x3 box over the nine-angle rotation tile cancels the
    // rotation exactly. The distance is the second input, since the AO target does not carry it; the alpha it writes
    // marks the texels on a depth edge, for the composite.
    GxRenderTargetSet(GxBuffers_Color, s_aoBlur, 0);
    GxRsSet(GxRs_PixelShader, s_blurPS);
    GxRsSet(GxRs_Texture0, s_ao);
    GxRsSet(GxRs_Texture1, s_z);
    DrawFullscreenQuad(s_aoBlurTex);

    // Composite onto the frame: upsample, then multiply by the term -- or, for the debug view, replace the frame
    // with it. Back to the default back buffer, clipped to the 3D rectangle the world drew into. The quad is the
    // AO target's size, which the ortho projection from SetupGx stretches over the whole frame.
    GxRenderTargetSet(GxBuffers_Color, nullptr, 0);
    GxXformSetViewport(minX, maxX, minY, maxY, 0.0f, 1.0f);
    GxRsSet(GxRs_PixelShader, s_compositePS);
    GxRsSet(GxRs_BlendingMode, mode > 1 ? GxBlend_Opaque : GxBlend_Mod);
    GxRsSet(GxRs_Texture0, s_aoBlur);
    GxRsSet(GxRs_Texture1, depth);
    GxRsSet(GxRs_Texture2, mask);
    GxRsSet(GxRs_Texture3, s_z);

    // ssao 3, 4 and 5 show one scale on its own (small, medium, big); anything else shows all three.
    const bool single = mode >= 3 && mode <= 2 + kPassCount;
    // A scale that was not drawn has nothing in its channel, so it is never shown.
    const float shown[4] = {
        kPasses[0].enabled && (!single || mode == 3) ? 1.0f : 0.0f,
        kPasses[1].enabled && (!single || mode == 4) ? 1.0f : 0.0f,
        kPasses[2].enabled && (!single || mode == 5) ? 1.0f : 0.0f,
        s_fogEnd,
    };
    GxShaderConstantsSet(GxSh_Pixel, kPassRow, shown, 1);
    GxShaderConstantsSet(GxSh_Pixel, kPassRow + 1, tintRow, 1);
    DrawFullscreenQuad(s_aoBlurTex);

    GxRsSet(GxRs_Texture0, nullptr);
    GxRsSet(GxRs_Texture1, nullptr);
    GxRsSet(GxRs_Texture2, nullptr);
    GxRsSet(GxRs_Texture3, nullptr);
    FFX::SetupReset();

    if (!useChar) {
        return;
    }

    // The close-up character pass, at the frame's full size over the pixels the character stamped. The frame-wide
    // composite above left exactly those pixels alone. The constants are the same layout (see kCharAoHLSL), written
    // again because the frame-wide ones are about a half-size target.
    const float charStrength = strength * kCharStrengthScale * charFade;
    const float charConstants[kCharConstantRows * 4] = {
        1.0f / size.x, 1.0f / size.y, minX, minY,
        m[0], m[5], m[8], m[9],
        mapping.atInfinity, mapping.perInverseViewDepth, mapping.deepest, kMaxViewDepth,
        0.0f, 0.0f, 0.0f, maxX - minX,
        0.0f, 0.0f, maxY - minY, 0.0f,
        s_fogStart, s_fogEnd, 0.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 0.0f, kCharDepthSigma, 0.0f,
        kCharRadius, charStrength, kCharBias, kCharMaxPixels,
        kCharMinPixels, kCharGain, kCharCurvePower, kCharSoftness,
    };

    FFX::SetupGx(&s_charAoTex);
    GxXformSetViewport(0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 1.0f);
    GxRsSet(GxRs_VertexShader, nullptr);
    GxRsSet(GxRs_BlendingMode, GxBlend_Opaque);
    GxRsSet(GxRs_ColorWrite, 15);
    GxRsSet(GxRs_AlphaRef, 0);
    GxShaderConstantsSet(GxSh_Pixel, 0, charConstants, kCharConstantRows);

    // Occupancy: the depth, and the mask whose green channel is the character.
    GxRenderTargetSet(GxBuffers_Color, s_charAo, 0);
    GxRsSet(GxRs_PixelShader, s_charAoPS);
    GxRsSet(GxRs_Texture0, depth);
    GxRsSet(GxRs_Texture1, mask);
    DrawFullscreenQuad(s_charAoTex);

    // The blur, along x and then along y: together the 4x4 window that holds each of the sixteen rotations once.
    const float stepX[4] = { 1.0f / size.x, 0.0f, kCharDepthSigma, 0.0f };
    const float stepY[4] = { 0.0f, 1.0f / size.y, kCharDepthSigma, 0.0f };

    GxRenderTargetSet(GxBuffers_Color, s_charBlur, 0);
    GxRsSet(GxRs_PixelShader, s_charBlurPS);
    GxRsSet(GxRs_Texture0, s_charAo);
    GxRsSet(GxRs_Texture1, depth);
    GxShaderConstantsSet(GxSh_Pixel, 8, stepX, 1);
    DrawFullscreenQuad(s_charBlurTex);

    GxRenderTargetSet(GxBuffers_Color, s_charAo, 0);
    GxRsSet(GxRs_Texture0, s_charBlur);
    GxShaderConstantsSet(GxSh_Pixel, 8, stepY, 1);
    DrawFullscreenQuad(s_charAoTex);

    // Onto the frame, the same way the frame-wide term goes on.
    GxRenderTargetSet(GxBuffers_Color, nullptr, 0);
    GxXformSetViewport(minX, maxX, minY, maxY, 0.0f, 1.0f);
    GxRsSet(GxRs_PixelShader, s_charCompositePS);
    GxRsSet(GxRs_BlendingMode, mode > 1 ? GxBlend_Opaque : GxBlend_Mod);
    GxRsSet(GxRs_Texture0, s_charAo);
    // Row 10 held the occupancy's own terms until now; the composite reads the rain's tint from it.
    GxShaderConstantsSet(GxSh_Pixel, kPassRow + 1, tintRow, 1);
    DrawFullscreenQuad(s_charAoTex);

    GxRsSet(GxRs_Texture0, nullptr);
    GxRsSet(GxRs_Texture1, nullptr);
    FFX::SetupReset();
}
