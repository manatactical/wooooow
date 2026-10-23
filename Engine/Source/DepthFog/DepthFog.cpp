// Not in the original 3.3.5a binary. Distance and height fog over the finished world frame; see DepthFog.h.
#include "Engine/Source/DepthFog/DepthFog.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include <d3dcompiler.h>

#include "Engine/Source/ConsoleBase/ConsoleVar.h"
#include "Engine/Source/ConsoleBase/Types.h"
#include "Engine/Source/Event/PerfLog.h"
#include "Engine/Source/FFX/FFX.h"
#include "Engine/Source/Gx/CGxDevice/CGxDevice.h"
#include "Engine/Source/Gx/CGxDevice/CGxShader.h"
#include "Engine/Source/Base/Handle.h"
#include "Engine/Source/Gx/CGxTex.h"
#include "Engine/Source/Gx/CGxTexFlags.h"
#include "Engine/Source/Gx/Gx.h"
#include "Engine/Source/Os/Time.h"
#include "Engine/Source/Services/Texture.h"
#include "Engine/Source/Tempest/C2Vector.h"
#include "Engine/Source/Tempest/C2iVector.h"
#include "Engine/Source/Tempest/C3Vector.h"
#include "Engine/Source/Tempest/C44Matrix.h"
#include "Engine/Source/Tempest/CImVector.h"
#include "Engine/Source/UnitOutline/UnitOutline.h"

namespace {

// The default of the depthFogStrength cvar.
constexpr float kStrength = 0.5f;

// Air density at the reference height, per yard, when the strength cvar is 1. Half that is the default: at it, 400 yards
// of level air at the camera's height take about a quarter of the view away.
constexpr float kDensityAtFull = 0.0010f;

// The reference height the density is quoted at sits this far below the camera, so air is thinner at the camera than at
// the reference and thicker still below it.
constexpr float kReferenceDrop = 20.0f;

// The far clip the density is tuned at. The density scales with the inverse of the far clip, so the same share of the view
// is hazed whether the view distance is 700 or 3000.
constexpr float kReferenceFarClip = 1500.0f;

// The height over which the density falls by a factor e. Large scenery (a valley wall) spans well over this.
constexpr float kScaleHeight = 70.0f;

// The far fade runs from this fraction of the far clip to this one, in distance from the eye. Ending short of the clip
// keeps the last tiles from showing a seam where they are culled, and puts every layer, and the full fade, inside the
// draw distance.
constexpr float kFarFadeStart = 0.6f;
constexpr float kFarFadeEnd = 0.9f;

// Plain distance fog, on top of the height fog: the optical depth at the end of the fade, per unit of strength. It is
// what makes the haze build up steadily with distance (the sense of depth) even where the height fog is thin.
constexpr float kDistanceDensity = 1.5f;

// How much of the sun's colour the in-scatter adds at its brightest, toward the sun.
constexpr float kScatter = 0.25f;

// The world's depth mapping; see MapWorldDepth. These are the ambient occlusion pass's constants, for the same
// projection.
constexpr float kMinViewportDepthExtent = 0.01f;
constexpr float kDeepestWorldDepthInFullRangeViewport = 0.9999995f;
constexpr float kWorldDepthMargin = 2.0e-6f;
constexpr float kSkyMinDepth = 0.99903f;
constexpr float kMaxViewDepth = 2000.0f;

constexpr int32_t kConstantRows = 18;

// c0  = { 1/width, 1/height, view origin x, view origin y }      (frame size; origin normalised to the frame)
// c1  = { view width, view height, 1/projection x, 1/projection y }
// c2  = { atInfinity, perInverseViewDepth, deepest depth, max view depth }
// c3  = view -> world, row for world x (the view matrix's columns are the rows of its inverse: it is a rotation)
// c4  = ... world y
// c5  = ... world z
// c6  = { sun direction (world, unit), sun scatter strength }
// c7  = { sun tint r, g, b, air density at the camera }
// c8  = { 1/scale height, far fade start, far fade end, view-space z of a unit ray (+1 or -1) }
// c9  = the sky gradient, horizon (c9) up to zenith (c14)
// c15 = { amount, debug (< 0: opaque grey view of the opacity), 0, 0 }
// c16 = the distances of the fog layers, in yards (0: unused)
// c17 = how much each layer adds, 0..1
const char* const kFogHLSL = R"HLSL(
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
float4 c15 : register(c15);
float4 c16 : register(c16);
float4 c17 : register(c17);

sampler2D sDepth : register(s0);

// Stored depth back to yards in front of the eye. The denominator is negative for real geometry (the client's
// projection stores depth = m22 + m32 / z with m32 < 0), which is why there is no clamp on it.
float ViewZ(float depth)
{
    return min(c2.y / (depth - c2.x), c2.w);
}

// The sky's colour at a given elevation of the view ray (sine of the angle above the horizon). Stops at 0, 0.06, 0.18,
// 0.4, 0.7 and 1; the long low steps are where a distant ridge stands against the sky.
float3 SkyAt(float e)
{
    float3 c = c9.rgb;
    c = lerp(c, c10.rgb, saturate(e / 0.06));
    c = lerp(c, c11.rgb, saturate((e - 0.06) / 0.12));
    c = lerp(c, c12.rgb, saturate((e - 0.18) / 0.22));
    c = lerp(c, c13.rgb, saturate((e - 0.4) / 0.3));
    c = lerp(c, c14.rgb, saturate((e - 0.7) / 0.3));
    return c;
}

float4 main(float2 uv : TEXCOORD0) : COLOR0
{
    // Debug view 4: a flat red, to tell a broken pipeline from a broken fog.
    if (abs(c15.z - 4) < 0.5) { return float4(1, 0, 0, 1); }

    // Outside the world's rectangle the frame is not the world; sky is left to the sky.
    float2 viewMin = c0.zw;
    float2 viewMax = c0.zw + c1.xy;
    if (uv.x < viewMin.x || uv.y < viewMin.y || uv.x > viewMax.x || uv.y > viewMax.y) { return float4(0, 0, 0, 0); }

    float depth = tex2Dlod(sDepth, float4(uv, 0, 0)).r;

    // Debug view 3: grey is the distance, white at the end of the full fade (sky is the maximum, so white too). Debug view
    // 5: grey is the raw depth, (1 - depth) * 100, so nearer is brighter.
    if (abs(c15.z - 3) < 0.5) { float g = saturate(ViewZ(depth) / c8.z); return float4(g, g, g, 1); }
    if (abs(c15.z - 5) < 0.5) { float g = saturate((1 - depth) * 100); return float4(g, g, g, 1); }
    // Debug view 6: stripes of the raw depth, frac(depth * 32). Debug view 7: the raw depth itself, nothing done to it.
    if (abs(c15.z - 6) < 0.5) { float g = frac(depth * 32); return float4(g, g, g, 1); }
    if (abs(c15.z - 7) < 0.5) { return float4(depth, depth, depth, 1); }

    // The sky is left to the sky. Between the world's own slice of the depth range and the sky's lies the horizon's: the
    // low-detail terrain drawn past the far clip. It has no distance worth decoding (it is beyond the fade by definition),
    // so it is the fog wall itself, fully fogged, and the mountains there dissolve into the sky instead of standing
    // against it.
    if (depth >= 0.99903) { return float4(0, 0, 0, 0); }
    bool horizon = depth >= c2.z;

    // The view ray through this pixel, unit length along the view axis, and the distance to the surface along it.
    float2 ndc = float2(((uv.x - c0.z) / c1.x - 0.5) * 2, (0.5 - (uv.y - c0.w) / c1.y) * 2);
    float3 ray = float3(ndc.x * c1.z, ndc.y * c1.w, c8.w);
    float dist = horizon ? 1.0e6 : ViewZ(depth) * length(ray);
    float3 dir = normalize(float3(dot(ray, c3.xyz), dot(ray, c4.xyz), dot(ray, c5.xyz)));

    // Height fog: density falls off exponentially with altitude, integrated along the ray. With x the fall in the
    // exponent over the whole ray, the integral is density * dist * (1 - exp(-x)) / x, which is just density * dist
    // for a level ray; the clamp keeps a ray plunging far down from overflowing.
    float x = clamp(c8.x * dir.z * dist, -12.0, 12.0);
    float shape = abs(x) < 0.001 ? 1.0 : (1.0 - exp(-x)) / x;
    float tau = c7.w * dist * shape;

    // Distance fog: nothing for the first fifth of the draw distance, then a ramp (squared, so it builds late) to its full
    // optical depth c15.w at the end of the fade.
    float distStart = c8.y / 3;
    float ramp = saturate((dist - distStart) / (c8.z - distStart));
    tau += c15.w * ramp * ramp;

    // Far fade: everything is the sky by the end of the draw distance.
    float far = smoothstep(c8.y, c8.z, dist);

    // Fog layers: each one is a step of extra haze that comes in over the last 30% of the way to its distance, so the
    // world reads as a few receding planes of air rather than one smooth ramp.
    float keep = 1;
    [unroll] for (int i = 0; i < 4; ++i)
    {
        keep *= 1 - c17[i] * smoothstep(c16[i] * 0.7, c16[i], dist);
    }

    float opacity = saturate(1 - (1 - far) * exp(-tau) * keep) * c15.x;

    // The colour: the sky where this ray would hit it, brightened toward the sun.
    float3 color = lerp(c9.rgb, c10.rgb, saturate(dir.z / 0.3));
    float toward = pow(saturate(dot(dir, c6.xyz)), 16);
    color = saturate(color + c7.rgb * (toward * c6.w));

    return float4(color, opacity);
}
)HLSL";

// Puts the finished fog onto the frame. It is a separate pass because the scene depth is also the depth buffer the back
// buffer is drawn with, and a texture cannot be read while it is bound for writing: the fog is worked out into a target of
// its own, from the depth, and this pass only reads that. The debug view replaces the frame with the opacity.
const char* const kCompositeHLSL = R"HLSL(
float4 c15 : register(c15);
sampler2D sFog : register(s0);

float4 main(float2 uv : TEXCOORD0) : COLOR0
{
    float4 fog = tex2Dlod(sFog, float4(uv, 0, 0));

    [branch] if (c15.y < 0)
    {
        return c15.z >= 3 ? float4(fog.rgb, 1) : float4(fog.a, fog.a, fog.a, 1);
    }

    return fog;
}
)HLSL";

// The decode terms for the stored depth. shortcut: duplicated from the sun glow pass, which duplicated it from the
// ambient occlusion pass, which duplicated it from water_renderer.cpp's MapWorldDepth. This is the fourth consumer, so
// the shared header the first copy promised is long overdue. See there for why the viewport's z range is folded in as
// it is.
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

// Compiles the shader above. The same pattern as the sun glow pass: optimised, because a failed compile leaves the
// shader null and the whole module silently draws nothing, and no CGxDevice::Log on the failure path, which is exactly
// when the process can least afford a varargs logger.
CGxShader* CompilePixelShader(const char* body) {
    ID3DBlob* code = nullptr;
    ID3DBlob* error = nullptr;
    HRESULT result = D3DCompile(body, strlen(body), "DepthFog", nullptr, nullptr, "main", "ps_3_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &error);

    if (FAILED(result)) {
        FILE* file = DiagnosticLogsEnabled() ? fopen("Logs\\depthfog.log", "a") : nullptr;

        if (file) {
            fputs("DepthFog: the pixel shader failed to compile\n", file);

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

// The depthFogLayers cvar: a comma-separated list of up to four distances, nearest first, as fractions of the far clip so
// the layers move with the view distance. Each layer adds a step
// of haze that has fully arrived at its distance; the steps grow with the layer's place in the list. "0" or an empty
// list turns the layers off. Unused slots get a harmless distance of 1 and a weight of 0, so the shader needs no branch.
void ParseLayers(const char* text, float* distances, float* weights, float farClip) {
    constexpr float kLayerWeights[4] = { 0.15f, 0.20f, 0.30f, 0.40f };
    int32_t count = 0;

    while (text && *text && count < 4) {
        char* end = nullptr;
        const float fraction = strtof(text, &end);

        if (end == text) {
            break;
        }

        text = end;

        while (*text == ',' || *text == ' ') {
            text++;
        }

        if (fraction > 0.0f && fraction < kFarFadeEnd) {
            distances[count] = std::max(fraction * farClip, 1.0f);
            weights[count] = kLayerWeights[count];
            count++;
        }
    }

    for (int32_t i = count; i < 4; i++) {
        distances[i] = 1.0f;
        weights[i] = 0.0f;
    }
}

void RefreshGxTexture(FFX::Tex& tex, CGxTex*& gxTexture) {
    gxTexture = tex.m_texture ? TextureGetGxTex(tex.m_texture, 1, nullptr) : nullptr;
}

// A render target of exactly the given size, bilinear. Copied from the sun glow pass;
// FFX::CreateTex is not used because it rounds up to a power of two, and the shaders address the targets in the
// frame's own uv.
bool CreateFrameTex(FFX::Tex& tex, const C2iVector& size) {
    EGxTexFormat format;

    if (g_theGxDevicePtr->Caps().m_rttFormat[GxTex_Argb8888]) {
        format = GxTex_Argb8888;
    } else if (g_theGxDevicePtr->Caps().m_rttFormat[GxTex_Abgr8888]) {
        format = GxTex_Abgr8888;
    } else {
        return false;
    }

    // GxTexCreate refuses a non-power-of-two size on GxTex_2d, so the non-pow2 target is the only one that can
    // hold the exact size. The depth sharing needs it too, so a device without it never gets here.
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

        CGxTexFlags flags(GxTex_Linear, 0, 0, 0, 0, 1, 1, 0, 0, 0);
        tex.m_texture = TextureCreate(GxTex_NonPow2, size.x, size.y, 0, format, format, flags, nullptr, GxTexNullUserFunc, "DepthFog::CreateFrameTex", 0);

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

// The quad the pass draws through, built as the engine's own full-screen passes build theirs.
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

// Why the pass drew nothing this frame, written to Logs\depthfog.log about once every two seconds while diagnostics are
// on (WOWLOG_QUIET=0). A pass that quietly stands down is indistinguishable from one that is broken, so each exit says
// which it was and what the numbers were.
void Note(const char* reason, float a = 0.0f, float b = 0.0f, float c = 0.0f, float d = 0.0f) {
    static uint32_t s_lastMs;
    const uint32_t now = OsGetAsyncTimeMs();

    if (now - s_lastMs < 2000 || !DiagnosticLogsEnabled()) {
        return;
    }

    s_lastMs = now;
    FILE* file = fopen("Logs\\depthfog.log", "a");

    if (file) {
        fprintf(file, "%s  %.4f %.4f %.4f %.4f\n", reason, a, b, c, d);
        fclose(file);
    }
}

// Set when the shader failed to compile. Compiling is expensive, so a failure is not retried every frame. Cleared by a
// device restore.
bool s_compileFailed;

}  // namespace

CVar* CDepthFog::s_cvarFog;
CVar* CDepthFog::s_cvarStrength;
CVar* CDepthFog::s_cvarLayers;
CGxShader* CDepthFog::s_fogPS;
CGxShader* CDepthFog::s_compositePS;
FFX::Tex CDepthFog::s_fogTex;
CGxTex* CDepthFog::s_fog;

// Not in the original binary. Called when a world frame is created, which happens again after a UI reload, so the
// callback registration is guarded: the device's list is append-only and OnDeviceRestored is not safe to run twice.
void CDepthFog::Init() {
    static bool s_registered;

    s_cvarFog = CVar::Register("depthFog", "distance and height fog: 0 off, 1 on, 2 show the fog opacity", 1, "1", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
    s_cvarStrength = CVar::Register("depthFogStrength", "height fog density: 0 none (the far fade stays), 1 thick, up to 5", 1, "0.5", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);

    s_cvarLayers = CVar::Register("depthFogLayers", "extra fog layers at these fractions of the far clip, up to four, comma separated; 0 for none", 1, "0.35,0.5,0.65,0.8", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
    // Read by the day/night update (by name; it does not link this module): 0 removes the engine's own distance fog
    // outdoors while depthFog is on, 1 keeps it.
    CVar::Register("nativeFog", "keep the engine's own distance fog outdoors while depthFog is on: 0 removed, 1 kept", 1, "0", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);

    if (s_registered) {
        return;
    }

    s_registered = true;
    g_theGxDevicePtr->AddDeviceRestoredCallback(CDepthFog::OnDeviceRestored);
}

bool CDepthFog::Enabled() {
    return s_cvarFog && s_cvarFog->m_intValue > 0 && g_theGxDevicePtr && g_theGxDevicePtr->m_context;
}

// Not in the original binary. A device reset frees the shader; ShaderDestroy re-nulls what it is given.
void CDepthFog::OnDeviceRestored() {
    if (s_fogPS) { g_theGxDevicePtr->ShaderDestroy(s_fogPS); }
    if (s_compositePS) { g_theGxDevicePtr->ShaderDestroy(s_compositePS); }

    s_compileFailed = false;
    RefreshGxTexture(s_fogTex, s_fog);
}

bool CDepthFog::EnsureResources() {
    if (!Enabled()) {
        return false;
    }

    if (g_theGxDevicePtr->DeviceApi() != GxApi_D3d9 && g_theGxDevicePtr->DeviceApi() != GxApi_D3d9Ex) {
        // The shader is D3D9 bytecode, and the depth sharing is a D3D9 concept.
        return false;
    }

    // The scene depth can only be sampled when it is INTZ; the D24X8 fallback is unreadable.
    if (!g_theGxDevicePtr->Caps().m_rttFormat[GxTex_INTZ]) {
        return false;
    }

    if (s_compileFailed) {
        return false;
    }

    if (!s_fogPS) { s_fogPS = CompilePixelShader(kFogHLSL); }
    if (!s_compositePS) { s_compositePS = CompilePixelShader(kCompositeHLSL); }

    if (!s_fogPS || !s_compositePS) {
        s_compileFailed = true;
        return false;
    }

    return true;
}

// Not in the original binary. Called inside the world render before the water; see the header for why.
void CDepthFog::Apply(const DepthFogAtmosphere& air) {
    if (!Enabled()) {
        return;
    }

    const int32_t mode = s_cvarFog->m_intValue;
    float strength = s_cvarStrength ? s_cvarStrength->m_floatValue : kStrength;
    strength = std::min(std::max(strength, 0.0f), 5.0f);

    const float amount = std::min(std::max(air.amount, 0.0f), 1.0f);

    if (amount < 0.004f && mode <= 1) {
        Note("no fog: indoors or underwater", air.amount);
        return;
    }

    if (!(air.farClip > 1.0f)) {
        Note("no far clip", air.farClip);
        return;
    }

    // The depth is only this frame's world depth when the shared-depth bind succeeded. A multisampled frame never
    // gets one (see CUnitOutline::OcclusionSupported), so this is also what stands the pass down under MSAA.
    if (!CUnitOutline::HasFrameDepth() || g_theGxDevicePtr->m_format.m_multisampleCount > 1) {
        Note("no shared depth (or multisampling)", CUnitOutline::HasFrameDepth() ? 1.0f : 0.0f, static_cast<float>(g_theGxDevicePtr->m_format.m_multisampleCount));
        return;
    }

    // The shared depth belongs to the default back buffer. If the world drew somewhere else -- the glow buffer --
    // the depth does not describe it.
    CGxTex* boundTarget = nullptr;
    g_theGxDevicePtr->DeviceGetRenderTarget(GxBuffers_Color, boundTarget);

    if (boundTarget) {
        Note("world drew into another target");
        return;
    }

    CGxTex* depth = CUnitOutline::DepthPassTexture();
    const CRect& window = g_theGxDevicePtr->DeviceCurWindow();
    C2iVector size = { static_cast<int32_t>(window.maxX), static_cast<int32_t>(window.maxY) };

    // Never sample a depth of another size.
    if (!depth || static_cast<int32_t>(depth->m_width) != size.x || static_cast<int32_t>(depth->m_height) != size.y) {
        Note("depth size mismatch");
        return;
    }

    // The world's own projection and viewport, read before FFX::SetupGx replaces them with its pixel-space ones.
    float minX = 0.0f, maxX = 0.0f, minY = 0.0f, maxY = 0.0f, minZ = 0.0f, maxZ = 0.0f;
    GxXformViewport(minX, maxX, minY, maxY, minZ, maxZ);

    const float viewWidth = maxX - minX;
    const float viewHeight = maxY - minY;

    if (viewWidth <= 0.001f || viewHeight <= 0.001f) {
        return;
    }

    const C44Matrix projection = g_theGxDevicePtr->m_projNative;
    const float* p = reinterpret_cast<const float*>(&projection);

    // p[11] is the projection's w row for view z: +1 or -1 depending on which way the view looks down its axis.
    if (p[0] == 0.0f || p[5] == 0.0f || p[11] == 0.0f) {
        return;
    }

    C44Matrix view;
    GxXformView(view);
    const float* v = reinterpret_cast<const float*>(&view);

    // The sun's direction in the world, normalised.
    float sunDir[3] = { air.toSun.x, air.toSun.y, air.toSun.z };
    const float sunLength = std::sqrt(sunDir[0] * sunDir[0] + sunDir[1] * sunDir[1] + sunDir[2] * sunDir[2]);
    float scatter = 0.0f;

    if (sunLength > 1e-3f) {
        for (float& channel : sunDir) {
            channel /= sunLength;
        }

        scatter = kScatter * std::min(std::max(air.sunVisibility, 0.0f), 1.0f);
    }

    // Normalised so the brightest channel is 1: the colour only says which way the light leans.
    float tint[3] = { air.sunColor[0], air.sunColor[1], air.sunColor[2] };
    const float peak = std::max(tint[0], std::max(tint[1], tint[2]));

    for (float& channel : tint) {
        channel = peak > 1e-4f ? channel / peak : 1.0f;
    }

    if (!EnsureResources()) {
        Note("resources unavailable (shader compile failed or no INTZ)", s_compileFailed ? 1.0f : 0.0f);
        return;
    }

    Note("drawing", amount, strength, air.farClip, air.cameraZ);

    // The world's own viewport z range, not the device's current one: see DepthFogAtmosphere::depthMaxZ.
    DepthMapping mapping = MapWorldDepth(p, minZ, air.depthMaxZ > minZ ? air.depthMaxZ : maxZ);

    // Everything past the world's own slice of the depth range is the horizon's or the sky's, and has no distance to decode.
    if (air.depthMaxZ > minZ) {
        mapping.deepest = air.depthMaxZ + kWorldDepthMargin;
    }
    if (DiagnosticLogsEnabled()) {
        static uint32_t s_lastMapMs;
        const uint32_t now = OsGetAsyncTimeMs();

        if (now - s_lastMapMs > 2000) {
            s_lastMapMs = now;

            if (FILE* file = fopen("Logs\\depthfog.log", "a")) {
                fprintf(file, "proj m10=%.7f m14=%.7f m11=%.4f m0=%.4f m5=%.4f | viewport z %.4f..%.4f | atInf=%.7f perInv=%.7f deepest=%.7f\n", p[10], p[14], p[11], p[0], p[5], minZ, maxZ, mapping.atInfinity, mapping.perInverseViewDepth, mapping.deepest);
                fclose(file);
            }
        }
    }

    const float density = kDensityAtFull * strength * (kReferenceFarClip / air.farClip) * std::exp(-kReferenceDrop / kScaleHeight);

    float constants[kConstantRows * 4] = {
        1.0f / size.x, 1.0f / size.y, minX, minY,
        viewWidth, viewHeight, 1.0f / p[0], 1.0f / p[5],
        mapping.atInfinity, mapping.perInverseViewDepth, mapping.deepest, kMaxViewDepth,
        // The view matrix is row-major with row vectors, so a view vector v maps back to the world as v * transpose(V).
        v[0], v[1], v[2], 0.0f,
        v[4], v[5], v[6], 0.0f,
        v[8], v[9], v[10], 0.0f,
        sunDir[0], sunDir[1], sunDir[2], scatter,
        tint[0], tint[1], tint[2], density,
        1.0f / kScaleHeight, air.farClip * kFarFadeStart, air.farClip * kFarFadeEnd, p[11] > 0.0f ? 1.0f : -1.0f,
    };

    for (int32_t i = 0; i < 6; i++) {
        float* row = constants + (9 + i) * 4;
        row[0] = air.sky[i][0];
        row[1] = air.sky[i][1];
        row[2] = air.sky[i][2];
        row[3] = 0.0f;
    }

    float* last = constants + 15 * 4;
    last[0] = amount;
    last[1] = mode > 1 ? -1.0f : 1.0f;
    last[2] = mode >= 3 ? static_cast<float>(mode) : 0.0f;
    last[3] = kDistanceDensity * strength;

    float* distances = constants + 16 * 4;
    float* weights = constants + 17 * 4;
    ParseLayers(s_cvarLayers ? s_cvarLayers->m_stringValue.m_str : "", distances, weights, air.farClip);

    // The fog is worked out into a target of its own, the size of the frame: the scene depth is also the depth buffer the
    // back buffer is drawn with, so it cannot be read while the back buffer is the target.
    if (!CreateFrameTex(s_fogTex, size)) {
        return;
    }

    RefreshGxTexture(s_fogTex, s_fog);

    if (!s_fog) {
        return;
    }

    // The engine's own full-screen setup: pixel-space ortho projection, identity view and world, the fog target as the
    // colour target, and the render-state push that covers everything set from here on.
    FFX::SetupGx(&s_fogTex);

    // The quad is transformed by the fixed-function pipeline, so a vertex shader left over from the world's last batch
    // must not transform it first.
    GxRsSet(GxRs_VertexShader, nullptr);
    GxRsSet(GxRs_BlendingMode, GxBlend_Opaque);
    GxRsSet(GxRs_ColorWrite, 15);
    GxRsSet(GxRs_AlphaRef, 0);

    // Fog: colour and opacity per pixel, from the depth. The shader leaves everything outside the world's rectangle and
    // every sky pixel transparent.
    // The pass runs mid-frame, and the engine sets some pixel constants once a frame and relies on them staying put (the
    // fog parameters among them), so what this overwrites is put back afterwards.
    float savedConstants[kConstantRows * 4];
    memcpy(savedConstants, g_theGxDevicePtr->ShaderConstantsLock(GxSh_Pixel), sizeof(savedConstants));

    GxShaderConstantsSet(GxSh_Pixel, 0, constants, kConstantRows);
    GxRsSet(GxRs_PixelShader, s_fogPS);
    GxRsSet(GxRs_Texture0, depth);
    DrawFullscreenQuad(s_fogTex);

    // Composite onto the frame: blended by the fog's opacity, or -- for the debug view -- replacing it.
    GxRenderTargetSet(GxBuffers_Color, nullptr, 0);
    GxRsSet(GxRs_PixelShader, s_compositePS);
    GxRsSet(GxRs_BlendingMode, mode > 1 ? GxBlend_Opaque : GxBlend_Alpha);
    GxRsSet(GxRs_Texture0, s_fog);
    DrawFullscreenQuad(s_fogTex);

    GxRsSet(GxRs_Texture0, nullptr);
    FFX::SetupReset();
    GxShaderConstantsSet(GxSh_Pixel, 0, savedConstants, kConstantRows);

    // SetupReset puts the viewport back to the whole frame and the full depth range, which is right at the end of the
    // frame but not here: the world still has the water, the weather and the second model pass to draw, into its own
    // rectangle and its own slice of the depth range.
    GxXformSetViewport(minX, maxX, minY, maxY, minZ, maxZ);

    // Switching the colour target away and back leaves the device on its own depth surface, not the shared one the world
    // was drawing with. The water finds its pixels by what it writes into that shared depth, so it has to be bound again.
    GxRenderTargetSet(GxBuffers_Depth, depth, 0);
}
