// Not in the original 3.3.5a binary. A glow around the sun over the finished world frame; see SunGlow.h.
#include "Engine/Source/SunGlow/SunGlow.h"

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

// The default of the sunGlowStrength cvar.
constexpr float kStrength = 0.5f;

// What the strength cvar's 1.0 means in light added to the frame at the glow's centre. The cvar is a 0..1 slider whose
// top end is the most the effect should ever do.
constexpr float kGain = 1.6f;

// The glow is a tight core and a wide halo, each falling off exponentially with distance from the sun in frame heights.
// Their weights are in the shader; these rates are the knobs worth turning.
constexpr float kCoreRate = 14.0f;
constexpr float kHaloRate = 3.2f;

// The share of the glow that bleeds over solid things (a tree edge or a roofline against the sun) rather than sky.
constexpr float kSolidShare = 0.3f;

// The sun's direction as a cosine against the view direction: no glow at the first, all of it at the second. The glow
// belongs to the sun's disc, so it goes with the sun out of the view rather than hanging at the screen's edge.
constexpr float kFacingFadeStart = 0.05f;
constexpr float kFacingFadeEnd = 0.35f;

// The stored depth of a pixel with no geometry; see MapWorldDepth.
constexpr float kMinViewportDepthExtent = 0.01f;
constexpr float kDeepestWorldDepthInFullRangeViewport = 0.9999995f;
constexpr float kWorldDepthMargin = 2.0e-6f;
constexpr float kSkyMinDepth = 0.99903f;

// The quad is drawn through a helper target at half the frame's size; FFX::SetupGx's ortho projection stretches it over
// the whole frame.
constexpr int32_t kResolutionDivisor = 2;

constexpr int32_t kConstantRows = 5;

// c0 = { 1/width, 1/height, view origin x, view origin y }       (frame size; origin normalised to the frame)
// c1 = { view width, view height, sun u, sun v }                  (sizes normalised to the frame; sun in frame uv)
// c2 = { 0, 0, deepest depth, 0 }                                 (a stored depth at or past this is sky)
// c3 = { view aspect, intensity, core rate, halo rate }
// c4 = { tint r, tint g, tint b, solid share }
const char* const kGlowHLSL = R"HLSL(
float4 c0 : register(c0);
float4 c1 : register(c1);
float4 c2 : register(c2);
float4 c3 : register(c3);
float4 c4 : register(c4);

sampler2D sDepth : register(s0);

bool IsSky(float2 uv)
{
    return tex2Dlod(sDepth, float4(uv, 0, 0)).r >= c2.z;
}

float4 main(float2 uv : TEXCOORD0) : COLOR0
{
    float2 viewMin = c0.zw;
    float2 viewMax = c0.zw + c1.xy;
    if (uv.x < viewMin.x || uv.y < viewMin.y || uv.x > viewMax.x || uv.y > viewMax.y) { return float4(0, 0, 0, 1); }

    // Distance from the sun in frame heights, so the glow is round whatever the aspect ratio.
    float2 toPixel = (uv - c1.zw) / c1.xy * float2(c3.x, 1);
    float dist = length(toPixel);

    // How much of the sun's own disc is uncovered: nine taps over a small disc around it. A tap off the picture can
    // not be looked at, so it counts as open.
    float open = 0;
    [unroll] for (int y = -1; y <= 1; ++y)
    {
        [unroll] for (int x = -1; x <= 1; ++x)
        {
            float2 tap = c1.zw + float2(x * 0.02 / c3.x * c1.x, y * 0.02 * c1.y);
            bool inside = tap.x >= viewMin.x && tap.y >= viewMin.y && tap.x <= viewMax.x && tap.y <= viewMax.y;
            open += (!inside || IsSky(tap)) ? 1 : 0;
        }
    }
    open *= 1.0 / 9.0;

    float core = exp(-dist * c3.z);
    float halo = exp(-dist * c3.w);
    float glow = 0.65 * core + 0.35 * halo;

    // Warm halo, white-hot core.
    float3 colour = lerp(c4.rgb, float3(1, 1, 1), core * core);

    float share = IsSky(uv) ? 1 : c4.w;
    return float4(colour * (glow * share * open * c3.y), 1);
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
    HRESULT result = D3DCompile(source, length, "SunGlow", nullptr, nullptr, "main", "ps_3_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &error);
    delete[] source;

    if (FAILED(result)) {
        FILE* file = DiagnosticLogsEnabled() ? fopen("Logs\\sunglow.log", "a") : nullptr;

        if (file) {
            fputs("SunGlow: a pixel shader failed to compile\n", file);

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
        tex.m_texture = TextureCreate(GxTex_NonPow2, size.x, size.y, 0, format, format, flags, nullptr, GxTexNullUserFunc, "SunGlow::CreateFrameTex", 0);

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

// Why the pass drew nothing this frame, written to Logs\sunglow.log about once every two seconds while diagnostics
// are on (WOWLOG_QUIET=0). A pass that quietly stands down is indistinguishable from one that is broken, so each exit
// says which it was and what the numbers were.
void Note(const char* reason, float a = 0.0f, float b = 0.0f, float c = 0.0f, float d = 0.0f) {
    static uint32_t s_lastMs;
    const uint32_t now = OsGetAsyncTimeMs();

    if (now - s_lastMs < 2000 || !DiagnosticLogsEnabled()) {
        return;
    }

    s_lastMs = now;
    FILE* file = fopen("Logs\\sunglow.log", "a");

    if (file) {
        fprintf(file, "%s  %.4f %.4f %.4f %.4f\n", reason, a, b, c, d);
        fclose(file);
    }
}

// Set when a shader failed to compile. Compiling is expensive, so a failure is not retried every frame -- that
// turned a broken shader into a large frame-rate loss on top of the missing effect. Cleared by a device restore.
bool s_compileFailed;

float SmoothStep(float edge0, float edge1, float x) {
    const float t = std::min(std::max((x - edge0) / (edge1 - edge0), 0.0f), 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

}  // namespace

CVar* CSunGlow::s_cvarGlow;
CVar* CSunGlow::s_cvarStrength;
CGxShader* CSunGlow::s_glowPS;
FFX::Tex CSunGlow::s_quadTex;
CGxTex* CSunGlow::s_quad;

// Not in the original binary. Called when a world frame is created, which happens again after a UI reload, so
// the callback registration is guarded: the device's list is append-only and OnDeviceRestored is not safe to run
// twice.
void CSunGlow::Init() {
    static bool s_registered;

    // Registered by CWorldParam::Initialize so the options panel can read its default at the login screen. This
    // call is the safety net, and Register reuses an existing var.
    s_cvarGlow = CVar::Register("sunGlow", "sun glow: 0 off, 1 on", 1, "1", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
    s_cvarStrength = CVar::Register("sunGlowStrength", "sun glow strength: 0 none, 1 full", 1, "0.5", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);

    if (s_registered) {
        return;
    }

    s_registered = true;
    g_theGxDevicePtr->AddDeviceRestoredCallback(CSunGlow::OnDeviceRestored);
}

bool CSunGlow::Enabled() {
    return s_cvarGlow && s_cvarGlow->m_intValue > 0 && g_theGxDevicePtr && g_theGxDevicePtr->m_context;
}

// Not in the original binary. A device reset frees the shader; the quad's target is a default-pool render target the
// device re-created itself, so only the cached pointer is stale. ShaderDestroy re-nulls what it is given.
void CSunGlow::OnDeviceRestored() {
    if (s_glowPS) { g_theGxDevicePtr->ShaderDestroy(s_glowPS); }

    s_compileFailed = false;
    RefreshGxTexture(s_quadTex, s_quad);
}

bool CSunGlow::EnsureResources() {
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

    if (!s_glowPS) { s_glowPS = CompilePixelShader("", kGlowHLSL); }

    if (!s_glowPS) {
        s_compileFailed = true;
        return false;
    }

    return true;
}

// Not in the original binary. The world -> interface boundary; see the header for the order and why.
void CSunGlow::Apply(const SunGlowLight& sun) {
    // The cheap exits first: most frames at night or indoors end here, before a shader is compiled or a target created.
    if (!Enabled()) {
        return;
    }

    float strength = s_cvarStrength ? s_cvarStrength->m_floatValue : kStrength;
    strength = std::min(std::max(strength, 0.0f), 1.0f);
    const float visibility = std::min(std::max(sun.visibility, 0.0f), 1.0f);

    if (visibility * strength < 0.004f) {
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

    // Behind the camera: there is no sun to glow.
    if (!(clipW > 1e-4f)) {
        Note("sun behind camera", clipW, vx, vy, vz);
        return;
    }

    const float facing = SmoothStep(kFacingFadeStart, kFacingFadeEnd, vz / length);
    const float intensity = strength * kGain * visibility * facing;

    if (intensity < 0.004f) {
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

    const C2iVector quadSize = { (size.x + kResolutionDivisor - 1) / kResolutionDivisor, (size.y + kResolutionDivisor - 1) / kResolutionDivisor };

    if (!CreateFrameTex(s_quadTex, quadSize)) {
        return;
    }

    RefreshGxTexture(s_quadTex, s_quad);

    if (!s_quad) {
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

    const float aspect = (viewWidth * size.x) / (viewHeight * size.y);

    const float constants[kConstantRows * 4] = {
        1.0f / size.x, 1.0f / size.y, minX, minY,
        viewWidth, viewHeight, sunU, sunV,
        0.0f, 0.0f, mapping.deepest, 0.0f,
        aspect, intensity, kCoreRate, kHaloRate,
        tint[0], tint[1], tint[2], kSolidShare,
    };

    // The engine's own full-screen setup: pixel-space ortho projection, identity view and world, and the render-state
    // push that covers everything set from here on.
    FFX::SetupGx(&s_quadTex);
    GxXformSetViewport(0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 1.0f);

    // The quad is transformed by the fixed-function pipeline, so a vertex shader left over from the world's last
    // batch must not transform it first.
    GxRsSet(GxRs_VertexShader, nullptr);
    GxRsSet(GxRs_ColorWrite, 15);
    GxRsSet(GxRs_AlphaRef, 0);

    GxShaderConstantsSet(GxSh_Pixel, 0, constants, kConstantRows);

    // Added onto the frame itself: back to the default back buffer, clipped to the 3D rectangle the world drew into.
    // The quad is the helper target's size, which the ortho projection from SetupGx stretches over the whole frame.
    GxRenderTargetSet(GxBuffers_Color, nullptr, 0);
    GxXformSetViewport(minX, maxX, minY, maxY, 0.0f, 1.0f);
    GxRsSet(GxRs_PixelShader, s_glowPS);
    GxRsSet(GxRs_BlendingMode, GxBlend_Add);
    GxRsSet(GxRs_Texture0, depth);
    DrawFullscreenQuad(s_quadTex);

    GxRsSet(GxRs_Texture0, nullptr);
    FFX::SetupReset();
}
