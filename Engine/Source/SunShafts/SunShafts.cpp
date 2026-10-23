// Not in the original 3.3.5a binary. Sun shafts over the finished world frame; see SunShafts.h for the pipeline.
#include "Engine/Source/SunShafts/SunShafts.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include <d3dcompiler.h>

#include "Engine/Source/Base/Handle.h"
#include "Engine/Source/ConsoleBase/ConsoleVar.h"
#include "Engine/Source/ConsoleBase/Types.h"
#include "Engine/Source/Event/PerfLog.h"
#include "Engine/Source/FFX/FFX.h"
#include "Engine/Source/Gx/CGxDevice/CGxDevice.h"
#include "Engine/Source/Gx/CGxDevice/CGxShader.h"
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

// The default of the sunShaftsStrength cvar: how much light a fully lit shaft adds, as a fraction of white.
constexpr float kStrength = 0.5f;

// What the strength cvar's 1.0 means in light added to the frame: the cvar is a 0..1 slider whose top end is the most
// the effect should ever do. Tuned by eye: 6.0 was far too strong, and a fifth of that is the ceiling.
constexpr float kGain = 1.2f;

// The march runs at half the frame's size in each direction and is brought back to full size by the composite's
// bilinear filter -- shafts are a smooth field, so there is nothing for a depth-aware upsample to protect.
constexpr int32_t kResolutionDivisor = 2;

// The longest walk toward the sun, in frame heights. A pixel on the far side of the screen from the sun would
// otherwise average a very long, thinly sampled ray; capping it keeps the shafts crisp and the taps dense.
constexpr float kMaxRayHeights = 0.9f;

// How quickly the shafts fade with distance from the sun's screen position, per frame height: exp(-d * this).
constexpr float kFalloffPerHeight = 0.6f;

// Air in front of a surface. A pixel at distance z scatters 1 - exp(-z / kAirYards) of what a pixel in the far
// distance does, so shafts do not paint the wall beside the camera but do fill a valley.
constexpr float kAirYards = 60.0f;

// Sky is already bright and sits in front of the sun's own glow; this is the share of the shaft term it receives, so it
// brightens toward the sun without clipping.
constexpr float kSkyShare = 0.5f;

// The shape of the final ramp: above 1, a faintly lit pixel stays almost clean and a fully lit one stands out, which is
// what separates a shaft from a general haze.
constexpr float kCurvePower = 1.0f;

// How far past the facing cone the shafts reach, as the cosine of the angle between the view direction and the sun:
// fully faded at the first, fully present at the second. The sun can be well off screen and still throw shafts into view.
constexpr float kFacingFadeStart = 0.15f;
constexpr float kFacingFadeEnd = 0.75f;

// The world's depth mapping; see MapWorldDepth. These are the ambient occlusion pass's constants, for the same
// projection.
constexpr float kMinViewportDepthExtent = 0.01f;
constexpr float kDeepestWorldDepthInFullRangeViewport = 0.9999995f;
constexpr float kWorldDepthMargin = 2.0e-6f;
constexpr float kSkyMinDepth = 0.99903f;
// What a pixel with no geometry decodes to. Far past any fade, so a sky pixel has all the air there is.
constexpr float kMaxViewDepth = 2000.0f;

constexpr int32_t kConstantRows = 6;

// Registers shared by all three shaders. These replace the engine's pixel shader outright, so the whole constant
// range is free.
//
// c0 = { 1/width, 1/height, view origin x, view origin y }       (frame size; origin normalised to the frame)
// c1 = { view width, view height, sun u, sun v }                 (sizes normalised to the frame; sun in frame uv)
// c2 = { atInfinity, perInverseViewDepth, deepest depth, max view depth }
// c3 = { 1/shaft width, 1/shaft height, max ray length in frame heights, 1/air yards }
// c4 = { sky share, falloff per frame height, frame aspect, curve power }
// c5 = { tint r, tint g, tint b, intensity }                      (intensity.w < 0: debug view, grey and opaque)
const char* const kCommonHLSL = R"HLSL(
float4 c0 : register(c0);
float4 c1 : register(c1);
float4 c2 : register(c2);
float4 c3 : register(c3);
float4 c4 : register(c4);
float4 c5 : register(c5);

// Stored depth back to yards in front of the eye. The denominator is negative for real geometry (the client's
// projection stores depth = m22 + m32 / z with m32 < 0), which is why there is no clamp on it.
float ViewZ(float depth)
{
    return depth >= c2.z ? c2.w : min(c2.y / (depth - c2.x), c2.w);
}
)HLSL";

// The march. 48 taps from the pixel toward the sun, each lit when it lands on sky.
const char* const kMarchHLSL = R"HLSL(
sampler2D sDepth : register(s0);

float4 main(float2 uv : TEXCOORD0) : COLOR0
{
    // Outside the world's rectangle the frame is not the world.
    float2 viewMin = c0.zw;
    float2 viewMax = c0.zw + c1.xy;
    if (uv.x < viewMin.x || uv.y < viewMin.y || uv.x > viewMax.x || uv.y > viewMax.y) { return float4(0, 0, 0, 1); }

    // A shaft texel covers a 2x2 block of frame pixels; its own depth is always read from the lower right one (the
    // quarter-pixel nudge keeps the floor off the exact boundary the texel centre sits on).
    float2 pixel = floor(uv / c0.xy + 0.25);
    float2 uvC = (pixel + 0.5) * c0.xy;
    float depth = tex2Dlod(sDepth, float4(uvC, 0, 0)).r;
    bool sky = depth >= c2.z;

    // The walk toward the sun, shortened to the cap. Distances are measured in frame heights so the cap and the
    // falloff do not depend on the aspect ratio.
    float2 toSun = c1.zw - uv;
    float heights = length(toSun * float2(c4.z, 1));
    toSun *= min(1, c3.z / max(heights, 1e-4));

    // Each texel starts its walk a ninth of a step further along than its neighbour, over a 3x3 tile. The box blur
    // covers each of the nine exactly once, so the banding of a fixed number of taps averages out.
    float2 texel = floor(uv / c3.xy);
    float2 tile = texel - 3 * floor(texel / 3);
    float jitter = frac((tile.x + 3 * tile.y) * (4.0 / 9.0));

    float lit = 0;
    float total = 0;

    [loop] for (int i = 0; i < 48; ++i)
    {
        float t = (i + jitter) * (1.0 / 48.0);
        float2 p = uv + toSun * t;

        // Past the edge of the world's rectangle nothing is known, so it counts as neither lit nor blocked: the walk
        // simply ends there, and the share of it that was lit falls as it gets cut short.
        bool inside = p.x >= viewMin.x && p.y >= viewMin.y && p.x <= viewMax.x && p.y <= viewMax.y;
        float d = tex2Dlod(sDepth, float4(p, 0, 0)).r;

        // The nearer part of the walk says more about what lights this pixel.
        float w = (1 - t) * (1 - t);
        lit += (inside && d >= c2.z) ? w : 0;
        total += w;
    }

    float air = sky ? c4.x : 1 - exp(-ViewZ(depth) * c3.w);
    float shaft = lit / total * exp(-heights * c4.y) * air;
    return float4(shaft, shaft, shaft, 1);
}
)HLSL";

// One 3x3 box in shaft texels.
const char* const kBlurHLSL = R"HLSL(
sampler2D sShaft : register(s0);

float4 main(float2 uv : TEXCOORD0) : COLOR0
{
    float sum = 0;

    [unroll] for (int y = -1; y <= 1; ++y)
    {
        [unroll] for (int x = -1; x <= 1; ++x)
        {
            sum += tex2Dlod(sShaft, float4(uv + float2(x, y) * c3.xy, 0, 0)).r;
        }
    }

    float shaft = sum * (1.0 / 9.0);
    return float4(shaft, shaft, shaft, 1);
}
)HLSL";

// Brings the half-size term back to the frame's size (the target is bilinear) and tints it. Added to the frame, or --
// for the debug view -- replacing it.
const char* const kCompositeHLSL = R"HLSL(
sampler2D sShaft : register(s0);

float4 main(float2 uv : TEXCOORD0) : COLOR0
{
    float shaft = pow(saturate(tex2Dlod(sShaft, float4(uv, 0, 0)).r), c4.w);

    [branch] if (c5.w < 0)
    {
        return float4(shaft, shaft, shaft, 1);
    }

    return float4(c5.rgb * (shaft * c5.w), 1);
}
)HLSL";

// The decode terms for the stored depth. shortcut: duplicated from the ambient occlusion pass, which duplicated it
// from water_renderer.cpp's MapWorldDepth. This is the third consumer, so the shared header the first copy promised is
// now due. See there for why the viewport's z range is folded in as it is.
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

float SmoothStep(float edge0, float edge1, float x) {
    const float t = std::min(std::max((x - edge0) / (edge1 - edge0), 0.0f), 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// Compiles one of the shaders above. The same pattern as the ambient occlusion pass: optimised, because a failed
// compile leaves the shader null and the whole module silently draws nothing, and no CGxDevice::Log on the failure
// path, which is exactly when the process can least afford a varargs logger.
CGxShader* CompilePixelShader(const char* prefix, const char* body) {
    const size_t prefixLength = strlen(prefix);
    const size_t length = prefixLength + strlen(body);
    char* source = new char[length + 1];
    memcpy(source, prefix, prefixLength);
    memcpy(source + prefixLength, body, strlen(body) + 1);

    ID3DBlob* code = nullptr;
    ID3DBlob* error = nullptr;
    HRESULT result = D3DCompile(source, length, "SunShafts", nullptr, nullptr, "main", "ps_3_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &error);
    delete[] source;

    if (FAILED(result)) {
        FILE* file = DiagnosticLogsEnabled() ? fopen("Logs\\sunshafts.log", "a") : nullptr;

        if (file) {
            fputs("SunShafts: a pixel shader failed to compile\n", file);

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

// A render target of exactly the given size, bilinear. Copied from the ambient occlusion pass's CreateFrameTex;
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
        tex.m_texture = TextureCreate(GxTex_NonPow2, size.x, size.y, 0, format, format, flags, nullptr, GxTexNullUserFunc, "SunShafts::CreateFrameTex", 0);

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

// Why the pass drew nothing this frame, written to Logs\sunshafts.log about once every two seconds while diagnostics
// are on (WOWLOG_QUIET=0). A pass that quietly stands down is indistinguishable from one that is broken, so each exit
// says which it was and what the numbers were.
void Note(const char* reason, float a = 0.0f, float b = 0.0f, float c = 0.0f, float d = 0.0f) {
    static uint32_t s_lastMs;
    const uint32_t now = OsGetAsyncTimeMs();

    if (now - s_lastMs < 2000 || !DiagnosticLogsEnabled()) {
        return;
    }

    s_lastMs = now;
    FILE* file = fopen("Logs\\sunshafts.log", "a");

    if (file) {
        fprintf(file, "%s  %.4f %.4f %.4f %.4f\n", reason, a, b, c, d);
        fclose(file);
    }
}

// Set when a shader failed to compile. Compiling is expensive, so a failure is not retried every frame -- that
// turned a broken shader into a large frame-rate loss on top of the missing effect. Cleared by a device restore.
bool s_compileFailed;

}  // namespace

CVar* CSunShafts::s_cvarShafts;
CVar* CSunShafts::s_cvarStrength;
CGxShader* CSunShafts::s_marchPS;
CGxShader* CSunShafts::s_blurPS;
CGxShader* CSunShafts::s_compositePS;
FFX::Tex CSunShafts::s_shaftTex;
FFX::Tex CSunShafts::s_shaftBlurTex;
CGxTex* CSunShafts::s_shaft;
CGxTex* CSunShafts::s_shaftBlur;

// Not in the original binary. Called when a world frame is created, which happens again after a UI reload, so
// the callback registration is guarded: the device's list is append-only and OnDeviceRestored is not safe to run
// twice.
void CSunShafts::Init() {
    static bool s_registered;

    // Registered by CWorldParam::Initialize so the options panel can read its default at the login screen. This
    // call is the safety net, and Register reuses an existing var.
    s_cvarShafts = CVar::Register("sunShafts", "sun shafts: 0 off, 1 on, 2 show the shaft term", 1, "1", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
    s_cvarStrength = CVar::Register("sunShaftsStrength", "sun shaft strength: 0 none, 1 full", 1, "0.5", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);

    if (s_registered) {
        return;
    }

    s_registered = true;
    g_theGxDevicePtr->AddDeviceRestoredCallback(CSunShafts::OnDeviceRestored);
}

bool CSunShafts::Enabled() {
    return s_cvarShafts && s_cvarShafts->m_intValue > 0 && g_theGxDevicePtr && g_theGxDevicePtr->m_context;
}

// Not in the original binary. A device reset frees the shaders; the shaft targets are default-pool render targets the
// device re-created itself, so only the cached pointers are stale. ShaderDestroy re-nulls what it is given.
void CSunShafts::OnDeviceRestored() {
    if (s_marchPS) { g_theGxDevicePtr->ShaderDestroy(s_marchPS); }
    if (s_blurPS) { g_theGxDevicePtr->ShaderDestroy(s_blurPS); }
    if (s_compositePS) { g_theGxDevicePtr->ShaderDestroy(s_compositePS); }

    s_compileFailed = false;
    RefreshGxTexture(s_shaftTex, s_shaft);
    RefreshGxTexture(s_shaftBlurTex, s_shaftBlur);
}

bool CSunShafts::EnsureResources() {
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

    if (!s_marchPS) { s_marchPS = CompilePixelShader(kCommonHLSL, kMarchHLSL); }
    if (!s_blurPS) { s_blurPS = CompilePixelShader(kCommonHLSL, kBlurHLSL); }
    if (!s_compositePS) { s_compositePS = CompilePixelShader(kCommonHLSL, kCompositeHLSL); }

    if (!s_marchPS || !s_blurPS || !s_compositePS) {
        s_compileFailed = true;
        return false;
    }

    return true;
}

// Not in the original binary. The world -> interface boundary; see the header for the order and why.
void CSunShafts::Apply(const SunShaftsLight& sun) {
    // The cheap exits first: most frames at night, indoors or facing away from the sun end here, before a shader is
    // compiled or a target created.
    if (!Enabled()) {
        return;
    }

    float strength = s_cvarStrength ? s_cvarStrength->m_floatValue : kStrength;
    strength = std::min(std::max(strength, 0.0f), 1.0f);
    const int32_t mode = s_cvarShafts->m_intValue;

    // The debug view shows the term whatever the sky is doing, so the pass can be told apart from "nothing to see".
    const float visibility = mode > 1 ? 1.0f : std::min(std::max(sun.visibility, 0.0f), 1.0f);

    if (visibility * strength < 0.004f && mode <= 1) {
        Note("no sun: visibility * strength too low", sun.visibility, strength);
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

    if (p[0] == 0.0f || p[5] == 0.0f) {
        return;
    }

    // Where the sun is on screen: its direction through the world's view and projection, exactly the transform the
    // sky drew the sun's disc with. The matrices are row-major with row vectors (Direct3D's convention), and a
    // direction has no translation, so only the upper 3x3 of the view applies.
    C44Matrix view;
    GxXformView(view);
    const float* v = reinterpret_cast<const float*>(&view);

    const C3Vector& d = sun.toSun;
    const float vx = d.x * v[0] + d.y * v[4] + d.z * v[8];
    const float vy = d.x * v[1] + d.y * v[5] + d.z * v[9];
    const float vz = d.x * v[2] + d.y * v[6] + d.z * v[10];
    const float length = std::sqrt(vx * vx + vy * vy + vz * vz);

    if (!(length > 1e-6f)) {
        return;
    }

    const float clipX = vx * p[0] + vy * p[4] + vz * p[8];
    const float clipY = vx * p[1] + vy * p[5] + vz * p[9];
    const float clipW = vx * p[3] + vy * p[7] + vz * p[11];

    // Behind the camera, or so far to the side that the projection is meaningless: no shafts.
    if (!(clipW > 1e-4f)) {
        Note("sun behind camera", clipW, vx, vy, vz);
        return;
    }

    const float facing = SmoothStep(kFacingFadeStart, kFacingFadeEnd, std::fabs(vz) / length);
    const float intensity = strength * kGain * visibility * facing;

    if (intensity < 0.004f && mode <= 1) {
        Note("sun out of view cone", facing, vz / length, visibility);
        return;
    }

    const float sunU = minX + (clipX / clipW * 0.5f + 0.5f) * viewWidth;
    const float sunV = minY + (0.5f - clipY / clipW * 0.5f) * viewHeight;

    if (!std::isfinite(sunU) || !std::isfinite(sunV)) {
        return;
    }

    if (!EnsureResources()) {
        Note("resources unavailable (shader compile failed or no INTZ)", s_compileFailed ? 1.0f : 0.0f);
        return;
    }

    Note("drawing", intensity, sunU, sunV, visibility);

    // The term lives at a fraction of the frame's size. Rounded up, so an odd frame still has a texel for its last
    // column; the shaders address it in the frame's own uv, so the exact size only has to be consistent.
    const C2iVector shaftSize = { (size.x + kResolutionDivisor - 1) / kResolutionDivisor, (size.y + kResolutionDivisor - 1) / kResolutionDivisor };

    if (!CreateFrameTex(s_shaftTex, shaftSize) || !CreateFrameTex(s_shaftBlurTex, shaftSize)) {
        return;
    }

    RefreshGxTexture(s_shaftTex, s_shaft);
    RefreshGxTexture(s_shaftBlurTex, s_shaftBlur);

    if (!s_shaft || !s_shaftBlur) {
        return;
    }

    const DepthMapping mapping = MapWorldDepth(p, minZ, maxZ);

    // Normalised so the brightest channel is 1: the colour only says which way the light leans, and the strength says
    // how much of it there is.
    float tint[3] = { sun.color[0], sun.color[1], sun.color[2] };
    const float peak = std::max(tint[0], std::max(tint[1], tint[2]));

    for (float& channel : tint) {
        channel = peak > 1e-4f ? channel / peak : 1.0f;
    }

    const float constants[kConstantRows * 4] = {
        1.0f / size.x, 1.0f / size.y, minX, minY,
        viewWidth, viewHeight, sunU, sunV,
        mapping.atInfinity, mapping.perInverseViewDepth, mapping.deepest, kMaxViewDepth,
        1.0f / shaftSize.x, 1.0f / shaftSize.y, kMaxRayHeights, 1.0f / kAirYards,
        kSkyShare, kFalloffPerHeight, static_cast<float>(size.x) / static_cast<float>(size.y), kCurvePower,
        tint[0], tint[1], tint[2], mode > 1 ? -1.0f : intensity,
    };

    // The engine's own full-screen setup: pixel-space ortho projection, identity view and world, the shaft target as
    // the colour target, and the render-state push that covers everything set from here on.
    FFX::SetupGx(&s_shaftTex);
    GxXformSetViewport(0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 1.0f);

    // The quad is transformed by the fixed-function pipeline, so a vertex shader left over from the world's last
    // batch must not transform it first.
    GxRsSet(GxRs_VertexShader, nullptr);
    GxRsSet(GxRs_BlendingMode, GxBlend_Opaque);
    GxRsSet(GxRs_ColorWrite, 15);
    GxRsSet(GxRs_AlphaRef, 0);

    GxShaderConstantsSet(GxSh_Pixel, 0, constants, kConstantRows);

    // March.
    GxRsSet(GxRs_PixelShader, s_marchPS);
    GxRsSet(GxRs_Texture0, depth);
    DrawFullscreenQuad(s_shaftTex);

    // Blur, and again back into the first target. Two 3x3 boxes are a 5x5 tent, which takes the grain of the starting
    // offsets and leaves the shafts' soft edges.
    GxRenderTargetSet(GxBuffers_Color, s_shaftBlur, 0);
    GxRsSet(GxRs_PixelShader, s_blurPS);
    GxRsSet(GxRs_Texture0, s_shaft);
    DrawFullscreenQuad(s_shaftBlurTex);

    GxRenderTargetSet(GxBuffers_Color, s_shaft, 0);
    GxRsSet(GxRs_Texture0, s_shaftBlur);
    DrawFullscreenQuad(s_shaftTex);

    // Composite onto the frame: added, or -- for the debug view -- replacing it. Back to the default back buffer,
    // clipped to the 3D rectangle the world drew into. The quad is the shaft target's size, which the ortho
    // projection from SetupGx stretches over the whole frame.
    GxRenderTargetSet(GxBuffers_Color, nullptr, 0);
    GxXformSetViewport(minX, maxX, minY, maxY, 0.0f, 1.0f);
    GxRsSet(GxRs_PixelShader, s_compositePS);
    GxRsSet(GxRs_BlendingMode, mode > 1 ? GxBlend_Opaque : GxBlend_Add);
    GxRsSet(GxRs_Texture0, s_shaft);
    DrawFullscreenQuad(s_shaftBlurTex);

    GxRsSet(GxRs_Texture0, nullptr);
    FFX::SetupReset();
}
