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

// The automatic spread. The kernel's reach is chosen per pixel so that, on screen, it always covers between these
// two sizes:
//   - a surface far away would otherwise get a reach of a pixel or two, which is noise rather than shading, so
//     the reach grows with distance until it covers kMinReachPixels, up to kMaxReachGrowth times the base reach;
//   - a surface right under the camera would get a reach of hundreds of pixels, which makes every tap a cache
//     miss, so it shrinks to kMaxReachPixels.
// The result is that a crate, a wall and a mountainside each get a shadow proportioned to how big they look.
constexpr float kMinReachPixels = 9.0f;
constexpr float kMaxReachPixels = 52.0f;
constexpr float kMaxReachGrowth = 5.0f;

// The pass runs at half the frame's size in each direction -- a quarter of the pixels -- and is brought back to
// full size by a depth-aware upsample in the composite.
constexpr int32_t kResolutionDivisor = 2;

// A tight corner only has about half of its hemisphere covered, so the raw occlusion fraction barely reaches 0.5 and
// the term would never get dark. This scales it back up before the strength is applied.
constexpr float kOcclusionGain = 2.1f;

// The shape of the ramp. The occlusion fraction goes through a power before the strength is applied: above 1 the
// curve is convex, so a surface that is only lightly shadowed stays nearly clean and the darkening climbs faster
// the closer the occluder is, which is how contact shadows read. Each tap also counts for more the closer it is to
// the surface (see kNearTapWeight), and a tap's own on/off test is a short ramp rather than a step -- the step is
// what made the term noisy, because a tap flips from 0 to 1 on a pixel's worth of depth.
constexpr float kCurvePower = 1.5f;

// Two scales. A single kernel size cannot suit everything: sized for a crate it misses the wide creases of a building
// (under an eave, where a wall meets the ground), and sized for a building it smothers small props. So the last
// kCoarseTaps of the kernel reach kCoarseScale times as far, at kCoarseWeight of the weight of a fine tap. Small
// objects are dominated by the fine taps and stay tight; a large structure picks up a broad, gentle gradient
// from the coarse ones.
constexpr int32_t kCoarseTaps = 5;
constexpr float kCoarseScale = 3.5f;
constexpr float kCoarseWeight = 1.9f;
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

constexpr int32_t kConstantRows = 10;

// Registers shared by the occupancy, blur and composite shaders. These replace the engine's pixel shader
// outright, so the whole constant range is free.
//
// c0 = { 1/width, 1/height, viewport origin x, viewport origin y }        (frame size; origin normalised)
// c1 = { P00, P11, P08, P09 }                                             (the world projection's terms)
// c2 = { atInfinity, perInverseViewDepth, deepest depth, max view depth }
// c3 = { radius, strength, bias, viewport width }                         (width normalised to the frame)
// c4 = { fade start, fade end, viewport height, blur radius in AO texels }
// c5 = { depth sigma, exclusion mask stamped, 1/AO width, 1/AO height }
// c6 = { min reach px, max reach px, max reach growth, occlusion gain }
// c7 = { curve power, tap softness, near tap weight, far tap weight }
// c8 = { coarse reach scale, coarse tap weight, first coarse tap, bias per yard }
// c9 = { far normal span, far normal yards, foliage keep, smooth boost }
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
// surface, where contact shadows live. Precomputed, so a pixel needs one sin/cos for its rotation rather than ten.
static const float4 kKernel[10] = {
    float4(0.22361, 0.00000, 0.97468, 0.24000),
    float4(-0.28558, 0.26162, 0.92195, 0.32000),
    float4(0.04371, -0.49809, 0.86603, 0.40000),
    float4(0.35996, 0.46950, 0.80623, 0.48000),
    float4(-0.66057, -0.11684, 0.74162, 0.56000),
    float4(0.62575, -0.39805, 0.67082, 0.64000),
    float4(-0.20930, 0.77858, 0.59161, 0.72000),
    float4(-0.39916, -0.76855, 0.50000, 0.80000),
    float4(0.86601, 0.31627, 0.38730, 0.88000),
    float4(-0.90094, 0.37190, 0.22361, 0.96000)
};

float4 main(float2 uv : TEXCOORD0) : COLOR0
{
    float2 pixel = floor(uv / c0.xy + 0.25);
    float2 uvC = (pixel + 0.5) * c0.xy;
    float2 zc = tex2Dlod(sZ, float4(uv, 0, 0)).rg;
    float z = UnpackZ(zc);

    // Sky, anything past the far plane and anything marked as not to be occluded (the grass, the water) were all
    // written as "past the fade", where the term is exactly 1 and there is nothing to compute. The distance rides
    // along in the other two channels for the blur and the upsample.
    if (z >= c4.y * 0.999) { return float4(1, zc, 1); }

    float3 p = ViewPos(uvC, z);

    // Normal from the nearer side on each axis, so a silhouette does not tilt the normal of the pixel beside it. One
    // AO texel is the smallest step that lands on a different sample of the half-size distance.
    float2 t = c5.zw * lerp(1, c9.x, saturate(z / c9.y));
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

    // The automatic spread. How many screen pixels a yard covers here, and from that the reach in yards that
    // lands the kernel between the minimum and maximum size on screen: nearby surfaces get a tighter kernel
    // (cheaper, and proportioned to what is in front of the eye), distant ones a wider one (so they still read).
    float pixelsPerYard = c1.y * 0.5 * (c4.z / c0.y) / z;
    float reach = clamp(c3.x * pixelsPerYard, c6.x, c6.y) / pixelsPerYard;
    reach = min(reach, c3.x * c6.z);
    float bias = c3.z * reach / c3.x + z * c8.w;

    // Each AO texel rotates the kernel by an angle taken from a 3x3 tile of nine evenly spaced angles; without a
    // rotation the taps read as rings. The blur is a 3x3 box over the same texels, so every blur window holds each of
    // the nine rotations exactly once. That is what stops it flickering: a random or hashed rotation leaves residual
    // grain that changes as the scene slides under the fixed screen pattern, while a complete tile averages out the
    // same way wherever the window sits.
    float2 texel = floor(uv / c5.zw);
    float2 tile = texel - 3 * floor(texel / 3);
    float slot = tile.x + 3 * tile.y;
    float spin = frac(slot * (4.0 / 9.0)) * 6.2831853;

    float spinSin, spinCos;
    sincos(spin, spinSin, spinCos);

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

    float occlusion = 0;
    float weightSum = 0;

    [unroll] for (int i = 0; i < 10; ++i)
    {
        float4 k = kKernel[i];
        float2 disk = float2(k.x * spinCos - k.y * spinSin, k.x * spinSin + k.y * spinCos);
        float3 dir = tangent * disk.x + bitangent * disk.y + n * k.z;
        // The last few taps reach further, for the wide creases of large structures.
        bool coarse = i >= c8.z;
        float tapReach = coarse ? reach * c8.x : reach;
        float3 tap = p + dir * (tapReach * k.w);

        float2 ndc = float2(tap.x / tap.z * c1.x + c1.z, tap.y / tap.z * c1.y + c1.w);
        float2 tapUv = float2(c0.z + (ndc.x * 0.5 + 0.5) * c3.w, c0.w + (0.5 - ndc.y * 0.5) * c4.z);

        bool inside = tap.z > 0.05
                   && tapUv.x >= c0.z && tapUv.x <= c0.z + c3.w
                   && tapUv.y >= c0.w && tapUv.y <= c0.w + c4.z;
        float sceneZ = ZAt(tapUv);

        // Occluded when the neighbour is in front of the tap. The range weight fades an occluder that is much
        // nearer than the kernel reaches, which is what removes the halo a silhouette would cast on the wall behind.
        // A tap that lands on an excluded surface does not occlude: blades are not geometry that shades anything.
        float diff = tap.z - sceneZ;
        float range = smoothstep(0, 1, tapReach / max(abs(z - sceneZ), 1e-4));

        // How far in front of the tap the neighbour is, as a ramp: the whole step from clear to occluded takes
        // kSoftness of the tap's own distance from the surface, so a tap does not flip on a pixel's worth of depth.
        float blocked = saturate((diff - bias) / (c7.y * tapReach * k.w));

        // A tap near the surface says more about a contact than one at the rim of the kernel.
        float tapWeight = lerp(c7.z, c7.w, k.w) * (coarse ? c8.y : 1);

        occlusion += inside ? blocked * range * tapWeight : 0;
        weightSum += tapWeight;
    }

    float ao = 1 - c3.y * pow(saturate(occlusion * c6.w / weightSum * lerp(c9.w, c9.z, clutter)), c7.x);
    ao = lerp(ao, 1, saturate((z - c4.x) / (c4.y - c4.x)));
    return float4(ao, zc, 1);
}
)HLSL";

const char* const kBlurHLSL = R"HLSL(
sampler2D sAo : register(s0);

// The other eight texels of the 3x3 box around this one, exactly one texel apart (kBlurRadius 1).
static const float2 kOffsets[8] = {
    float2( 1,  0), float2(-1,  0), float2( 0,  1), float2( 0, -1),
    float2( 1,  1), float2(-1,  1), float2( 1, -1), float2(-1, -1)
};

float4 main(float2 uv : TEXCOORD0) : COLOR0
{
    float4 centre = tex2Dlod(sAo, float4(uv, 0, 0));
    float centreZ = UnpackZ(centre.gb);

    // Sky, far terrain and excluded surfaces hold exactly 1 and every neighbour that could reach them is either 1 as
    // well or weighted out by the depth test, so the blur would return the texel unchanged. Skipping it is exact, and
    // those texels are most of a typical frame.
    [branch] if (centreZ >= c4.y * 0.999)
    {
        return float4(1, centre.gb, 1);
    }

    // The box doubles its spacing with distance. Offsets of two texels still land on all three residues of the 3x3
    // rotation tile, so the grain keeps cancelling, while each tap reaches twice as far.
    float spacing = lerp(1, 2, saturate((centreZ - c4.x * 0.5) / max(c4.x, 1)));
    float sum = centre.r;
    float wsum = 1;

    [unroll] for (int i = 0; i < 8; ++i)
    {
        float4 tap = tex2Dlod(sAo, float4(uv + kOffsets[i] * c5.zw * c4.w * spacing, 0, 0));
        float tapZ = UnpackZ(tap.gb);

        // Bilateral weight: taps on the far side of a silhouette do not contribute, so blurring cannot paint a
        // dark fringe onto the wall behind a fence post.
        float w = saturate(1 - abs(tapZ - centreZ) / (c5.x * centreZ + 0.05));
        sum += tap.r * w;
        wsum += w;
    }

    return float4(sum / wsum, centre.gb, 1);
}
)HLSL";

// Brings the half-size term back to the frame's size. Each frame pixel takes the four AO texels around it, weighted
// by the usual bilinear share times how close that texel's depth is to the pixel's own, so the term does not leak
// across a silhouette. Also where the exclusion mask is applied at full resolution.
const char* const kCompositeHLSL = R"HLSL(
sampler2D sAo : register(s0);
sampler2D sDepth : register(s1);
sampler2D sExclude : register(s2);

float4 main(float2 uv : TEXCOORD0) : COLOR0
{
    float2 pixel = floor(uv / c0.xy);

    [branch] if (c5.y > 0.5)
    {
        if (tex2Dlod(sExclude, float4(uv, 0, 0)).r > 0.5) { return float4(1, 1, 1, 1); }
    }

    float centreZ = min(ViewZ(DepthAt(sDepth, uv)), c4.y);

    // Past the fade the term is exactly 1; the four taps below could only add the 0.001 leak of a nearer texel.
    [branch] if (centreZ >= c4.y * 0.999)
    {
        return float4(1, 1, 1, 1);
    }

    // AO texel j stands for frame pixel 2j + 1, so this pixel sits (pixel - 1) / 2 texels along.
    float2 t = (pixel - 1) * 0.5;
    float2 base = floor(t);
    float2 f = t - base;

    float sum = 0;
    float wsum = 0;

    [unroll] for (int i = 0; i < 4; ++i)
    {
        float2 corner = float2((i == 1 || i == 3) ? 1 : 0, i >= 2 ? 1 : 0);
        float2 texel = base + corner;
        float2 aoUv = (texel + 0.5) * c5.zw;
        float4 tap = tex2Dlod(sAo, float4(aoUv, 0, 0));

        float2 bil = lerp(1 - f, f, corner);
        float w = bil.x * bil.y * (saturate(1 - abs(UnpackZ(tap.gb) - centreZ) / (c5.x * centreZ + 0.05)) + 0.001);
        sum += tap.r * w;
        wsum += w;
    }

    float ao = sum / max(wsum, 1e-6);
    return float4(ao, ao, ao, 1);
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

// Set when a shader failed to compile. Compiling is expensive, so a failure is not retried every frame -- that
// turned a broken shader into a large frame-rate loss on top of the missing effect. Cleared by a device restore.
bool s_compileFailed;

}  // namespace

CVar* CAmbientOcclusion::s_cvarSsao;
CVar* CAmbientOcclusion::s_cvarSpread;
CVar* CAmbientOcclusion::s_cvarStrength;
CVar* CAmbientOcclusion::s_cvarDistance;
CGxShader* CAmbientOcclusion::s_zPS;
CGxShader* CAmbientOcclusion::s_aoPS;
CGxShader* CAmbientOcclusion::s_blurPS;
CGxShader* CAmbientOcclusion::s_compositePS;
CGxShader* CAmbientOcclusion::s_excludePS;
FFX::Tex CAmbientOcclusion::s_excludeTex;
CGxTex* CAmbientOcclusion::s_exclude;
bool CAmbientOcclusion::s_excludeStamped;
bool CAmbientOcclusion::s_stamping;
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
    s_cvarSsao = CVar::Register("ssao", "screen-space ambient occlusion: 0 off, 1 on, 2 show the occlusion term",
                                1, "1", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);

    // The two knobs of the look, registered here as well as by CWorldParam::Initialize for the same reason.
    s_cvarSpread = CVar::Register("ssaoSpread", "ambient occlusion spread, as a multiple of the default reach",
                                  1, "0.06", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
    s_cvarStrength = CVar::Register("ssaoStrength", "ambient occlusion strength: 0 none, 1 full",
                                    1, "0.5", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);

    s_cvarDistance = CVar::Register("ssaoDistance", "ambient occlusion fades out and stops at this distance, in yards",
                                    1, "145", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);

    if (s_registered) {
        return;
    }

    s_registered = true;
    g_theGxDevicePtr->AddDeviceRestoredCallback(CAmbientOcclusion::OnDeviceRestored);
}

bool CAmbientOcclusion::Enabled() {
    return s_cvarSsao && s_cvarSsao->m_intValue > 0 && g_theGxDevicePtr && g_theGxDevicePtr->m_context;
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

// Not in the original binary. See the declaration; modelled on CUnitOutline::StampMask, except that the render
// target is switched once for a whole run of stamps instead of once per stamp.
bool CAmbientOcclusion::BeginExcludeStamps(bool ignoreAlpha) {
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
    GxRsSet(GxRs_ColorWrite, 15);
    GxRsSet(GxRs_PixelShader, s_excludePS);
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

// Not in the original binary. The world -> interface boundary; see the header for the order and why.
void CAmbientOcclusion::Apply() {
    // Whether the mask was written this frame is consumed here, whatever the pass then decides: a stale mask from
    // a frame that stood down must not be read by the next one.
    const bool excludeStamped = s_excludeStamped;
    s_excludeStamped = false;

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

    float distance = s_cvarDistance ? s_cvarDistance->m_floatValue : kDistance;
    distance = std::min(std::max(distance, 10.0f), 500.0f);

    const float constants[kConstantRows * 4] = {
        1.0f / size.x, 1.0f / size.y, minX, minY,
        m[0], m[5], m[8], m[9],
        mapping.atInfinity, mapping.perInverseViewDepth, mapping.deepest, kMaxViewDepth,
        kRadius * spread, strength, kBias * spread, maxX - minX,
        distance * kFadeStartFraction, distance, maxY - minY, kBlurRadius,
        kBlurDepthSigma, useExclude ? 1.0f : 0.0f, 1.0f / aoSize.x, 1.0f / aoSize.y,
        kMinReachPixels, kMaxReachPixels, kMaxReachGrowth, kOcclusionGain,
        kCurvePower, kSoftness, kNearTapWeight, kFarTapWeight,
        kCoarseScale, kCoarseWeight, static_cast<float>(10 - kCoarseTaps), kBiasPerYard,
        kFarNormalSpan, kNormalFarYards, kFoliageKeep, kSmoothBoost,
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

    // Occupancy.
    GxRenderTargetSet(GxBuffers_Color, s_ao, 0);
    GxRsSet(GxRs_PixelShader, s_aoPS);
    GxRsSet(GxRs_Texture0, s_z);
    DrawFullscreenQuad(s_aoTex);

    // Blur.
    GxRenderTargetSet(GxBuffers_Color, s_aoBlur, 0);
    GxRsSet(GxRs_PixelShader, s_blurPS);
    GxRsSet(GxRs_Texture0, s_ao);
    DrawFullscreenQuad(s_aoBlurTex);

    // Blur again, back into the first target. Two 3x3 passes are a 5x5 tent, which takes most of what grain the
    // first leaves, for the price of one more cheap pass at a quarter of the frame's pixels.
    GxRenderTargetSet(GxBuffers_Color, s_ao, 0);
    GxRsSet(GxRs_Texture0, s_aoBlur);
    DrawFullscreenQuad(s_aoTex);

    // Composite onto the frame: upsample, then multiply by the term -- or, for the debug view, replace the frame
    // with it. Back to the default back buffer, clipped to the 3D rectangle the world drew into. The quad is the
    // AO target's size, which the ortho projection from SetupGx stretches over the whole frame.
    GxRenderTargetSet(GxBuffers_Color, nullptr, 0);
    GxXformSetViewport(minX, maxX, minY, maxY, 0.0f, 1.0f);
    GxRsSet(GxRs_PixelShader, s_compositePS);
    GxRsSet(GxRs_BlendingMode, mode > 1 ? GxBlend_Opaque : GxBlend_Mod);
    GxRsSet(GxRs_Texture0, s_ao);
    GxRsSet(GxRs_Texture1, depth);
    GxRsSet(GxRs_Texture2, mask);
    DrawFullscreenQuad(s_aoBlurTex);

    GxRsSet(GxRs_Texture0, nullptr);
    GxRsSet(GxRs_Texture1, nullptr);
    GxRsSet(GxRs_Texture2, nullptr);
    FFX::SetupReset();
}
