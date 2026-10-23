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

// Tuned for the client's world scale, which is yards: a player is about 2 yards tall, a door about 3. 0.75 is
// roughly shoulder-to-shoulder -- wide enough to catch a contact shadow under a chest, narrow enough that an
// open courtyard stays clean.
constexpr float kRadius = 0.75f;

// How dark a fully occluded pixel goes: the occlusion fraction is scaled by this before it is taken from 1. The
// default of the ssaoStrength cvar; ssaoSpread scales kRadius.
constexpr float kStrength = 0.85f;

// Depth offset, in yards, that stops a surface from occluding itself.
constexpr float kBias = 0.03f;

// Fade the term out with distance so the far clip does not get a uniform grey wash.
constexpr float kFadeStart = 40.0f;
constexpr float kFadeEnd = 110.0f;

// Blur tap spacing in texels, and how far apart two depths may be, as a fraction of the centre's own distance,
// before the tap stops contributing.
constexpr float kBlurRadius = 2.0f;
constexpr float kBlurDepthSigma = 0.04f;

// The world's depth mapping; see MapWorldDepth. These are the water pass's constants, for the same projection.
constexpr float kMinViewportDepthExtent = 0.01f;
constexpr float kDeepestWorldDepthInFullRangeViewport = 0.9999995f;
constexpr float kWorldDepthMargin = 2.0e-6f;
constexpr float kSkyMinDepth = 0.99903f;
// What a pixel with no geometry decodes to. Far past the fade, so it can never occlude or be occluded.
constexpr float kMaxViewDepth = 2000.0f;

// Registers shared by the occupancy and blur shaders. These replace the engine's pixel shader outright, so the
// whole constant range is free.
//
// c0 = { 1/width, 1/height, viewport origin x, viewport origin y }        (origin normalised to the frame)
// c1 = { P00, P11, P08, P09 }                                             (the world projection's terms)
// c2 = { atInfinity, perInverseViewDepth, deepest depth, max view depth }
// c3 = { radius, strength, bias, viewport width }                         (width normalised to the frame)
// c4 = { fade start, fade end, viewport height, blur radius in texels }
// c5 = { blur depth sigma, unused, unused, unused }
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

// The point on the surface the frame's uv sees at view distance z, camera at the origin looking down +z.
float3 ViewPos(float2 uv, float z)
{
    float2 ndc = float2((uv.x - c0.z) / c3.w * 2 - 1, 1 - (uv.y - c0.w) / c4.z * 2);
    return float3((ndc.x - c1.z) / c1.x * z, (ndc.y - c1.w) / c1.y * z, z);
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
sampler2D sDepth : register(s0);
sampler2D sExclude : register(s1);

// c5.y is 1 when a mask was stamped this frame. Without it the mask holds an old frame, so it is ignored.
float Excluded(float2 uv)
{
    return c5.y > 0.5 ? tex2D(sExclude, uv).r : 0;
}

float3 PosAt(float2 uv)
{
    return ViewPos(uv, ViewZ(tex2D(sDepth, uv).r));
}

float4 main(float2 uv : TEXCOORD0) : COLOR0
{
    float depth = tex2D(sDepth, uv).r;

    // Sky, and anything past the far plane, has no geometry to occlude against.
    if (depth >= c2.z) { return float4(1, 1, 1, 1); }

    // Marked as not to be occluded (the grass): left exactly as the world drew it.
    if (Excluded(uv) > 0.5) { return float4(1, 1, 1, 1); }

    float z = ViewZ(depth);
    float3 p = ViewPos(uv, z);

    // Normal from the nearer side on each axis, so a silhouette does not tilt the normal of the pixel beside it.
    float2 t = c0.xy;
    float3 pr = PosAt(uv + float2(t.x, 0));
    float3 pl = PosAt(uv - float2(t.x, 0));
    float3 pd = PosAt(uv + float2(0, t.y));
    float3 pu = PosAt(uv - float2(0, t.y));
    float3 dx = abs(pr.z - p.z) < abs(p.z - pl.z) ? pr - p : p - pl;
    float3 dy = abs(pd.z - p.z) < abs(p.z - pu.z) ? pd - p : p - pu;
    float3 n = normalize(cross(dx, dy));
    // The eye is at the origin, so a surface that faces it has a normal pointing back along -p.
    n = dot(n, p) > 0 ? -n : n;

    float3 up = abs(n.y) < 0.99 ? float3(0, 1, 0) : float3(1, 0, 0);
    float3 tangent = normalize(cross(up, n));
    float3 bitangent = cross(n, tangent);

    // Interleaved gradient noise rotates the kernel per pixel; without it, 12 taps read as 12 rings.
    float spin = frac(52.9829189 * frac(dot(uv / c0.xy, float2(0.06711056, 0.00583715)))) * 6.2831853;

    float occlusion = 0;

    [unroll] for (int i = 0; i < 12; ++i)
    {
        float s = (i + 0.5) / 12.0;
        float angle = i * 2.39996323 + spin;
        float r = sqrt(s);
        float2 disk = float2(cos(angle), sin(angle)) * r;
        float3 dir = tangent * disk.x + bitangent * disk.y + n * sqrt(max(1 - r * r, 0));

        // Taps cluster near the surface, where contact shadows live.
        float3 tap = p + dir * (c3.x * lerp(0.1, 1.0, s * s));

        float2 ndc = float2(tap.x / tap.z * c1.x + c1.z, tap.y / tap.z * c1.y + c1.w);
        float2 tapUv = float2(c0.z + (ndc.x * 0.5 + 0.5) * c3.w, c0.w + (0.5 - ndc.y * 0.5) * c4.z);

        bool inside = tap.z > 0.05
                   && tapUv.x >= c0.z && tapUv.x <= c0.z + c3.w
                   && tapUv.y >= c0.w && tapUv.y <= c0.w + c4.z;
        float sceneZ = ViewZ(tex2D(sDepth, tapUv).r);

        // Occluded when the neighbour is in front of the tap. The range weight fades an occluder that is much
        // nearer than the kernel reaches, which is what removes the halo a silhouette would cast on the wall behind.
        float diff = tap.z - sceneZ;
        float range = smoothstep(0, 1, c3.x / max(abs(z - sceneZ), 1e-4));
        // A tap that lands on an excluded surface does not occlude: blades are not geometry that shades anything.
        occlusion += (inside && diff > c3.z && Excluded(tapUv) < 0.5) ? range : 0;
    }

    float ao = 1 - c3.y * saturate(occlusion / 12.0);
    ao = lerp(ao, 1, saturate((z - c4.x) / (c4.y - c4.x)));
    return float4(ao, ao, ao, 1);
}
)HLSL";

const char* const kBlurHLSL = R"HLSL(
sampler2D sAo : register(s0);
sampler2D sDepth : register(s1);

static const float2 kOffsets[8] = {
    float2( 1,  0), float2(-1,  0), float2( 0,  1), float2( 0, -1),
    float2( 1.4,  1.4), float2(-1.4,  1.4), float2( 1.4, -1.4), float2(-1.4, -1.4)
};

float4 main(float2 uv : TEXCOORD0) : COLOR0
{
    float centreZ = ViewZ(tex2D(sDepth, uv).r);
    float sum = tex2D(sAo, uv).r;
    float wsum = 1;

    [unroll] for (int i = 0; i < 8; ++i)
    {
        float2 tapUv = uv + kOffsets[i] * c0.xy * c4.w;
        float tapZ = ViewZ(tex2D(sDepth, tapUv).r);

        // Bilateral weight: taps on the far side of a silhouette do not contribute, so blurring cannot paint a
        // dark fringe onto the wall behind a fence post.
        float w = saturate(1 - abs(tapZ - centreZ) / (c5.x * centreZ + 0.05));
        sum += tex2D(sAo, tapUv).r * w;
        wsum += w;
    }

    float ao = sum / wsum;
    return float4(ao, ao, ao, 1);
}
)HLSL";

const char* const kCompositeHLSL = R"HLSL(
sampler2D sAo : register(s0);

float4 main(float2 uv : TEXCOORD0) : COLOR0
{
    float ao = tex2D(sAo, uv).r;
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

        CGxTexFlags flags(GxTex_Linear, 0, 0, 0, 0, 1, 1, 0, 0, 0);
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

}  // namespace

CVar* CAmbientOcclusion::s_cvarSsao;
CVar* CAmbientOcclusion::s_cvarSpread;
CVar* CAmbientOcclusion::s_cvarStrength;
CGxShader* CAmbientOcclusion::s_aoPS;
CGxShader* CAmbientOcclusion::s_blurPS;
CGxShader* CAmbientOcclusion::s_compositePS;
CGxShader* CAmbientOcclusion::s_excludePS;
FFX::Tex CAmbientOcclusion::s_excludeTex;
CGxTex* CAmbientOcclusion::s_exclude;
bool CAmbientOcclusion::s_excludeStamped;
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
                                  1, "1", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
    s_cvarStrength = CVar::Register("ssaoStrength", "ambient occlusion strength: 0 none, 1 full",
                                    1, "0.85", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);

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
    if (s_aoPS) { g_theGxDevicePtr->ShaderDestroy(s_aoPS); }
    if (s_blurPS) { g_theGxDevicePtr->ShaderDestroy(s_blurPS); }
    if (s_compositePS) { g_theGxDevicePtr->ShaderDestroy(s_compositePS); }
    if (s_excludePS) { g_theGxDevicePtr->ShaderDestroy(s_excludePS); }

    RefreshGxTexture(s_excludeTex, s_exclude);
    s_excludeStamped = false;
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

    if (!s_aoPS) { s_aoPS = CompilePixelShader(kCommonHLSL, kAoHLSL); }
    if (!s_blurPS) { s_blurPS = CompilePixelShader(kCommonHLSL, kBlurHLSL); }
    if (!s_compositePS) { s_compositePS = CompilePixelShader("", kCompositeHLSL); }
    if (!s_excludePS) { s_excludePS = CompilePixelShader("", kExcludeHLSL); }

    return s_aoPS && s_blurPS && s_compositePS && s_excludePS;
}

// Not in the original binary. See the declaration; modelled on CUnitOutline::StampMask.
void CAmbientOcclusion::StampExcluded(const CGxBatch& batch) {
    if (!Active() || !EnsureResources()) {
        return;
    }

    // The mask is in the default back buffer's pixel space, like the depth it is tested against.
    CGxTex* oldTarget = nullptr;
    g_theGxDevicePtr->DeviceGetRenderTarget(GxBuffers_Color, oldTarget);

    if (oldTarget) {
        return;
    }

    const CRect& window = g_theGxDevicePtr->DeviceCurWindow();
    C2iVector size = { static_cast<int32_t>(window.maxX), static_cast<int32_t>(window.maxY) };

    if (!CreateFrameTex(s_excludeTex, size)) {
        return;
    }

    RefreshGxTexture(s_excludeTex, s_exclude);

    if (!s_exclude) {
        return;
    }

    const int32_t alphaRef = g_theGxDevicePtr->m_appRenderStates[GxRs_AlphaRef].m_value.m_data.i[0];
    const float cutoff[4] = { static_cast<float>(alphaRef) * (1.0f / 255.0f), 0.0f, 0.0f, 0.0f };

    // Render targets live outside RsPush/RsPop, so the caller's target is put back by hand below. The world's
    // viewport is left alone: the mask is drawn through the same projection and viewport as the frame, so its
    // pixels land where the frame's do.
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

    g_theGxDevicePtr->Draw(const_cast<CGxBatch*>(&batch), 1);

    g_theGxDevicePtr->RsPop();
    GxRenderTargetSet(GxBuffers_Color, oldTarget, 0);
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

    if (!CreateFrameTex(s_aoTex, size) || !CreateFrameTex(s_aoBlurTex, size)) {
        return;
    }

    RefreshGxTexture(s_aoTex, s_ao);
    RefreshGxTexture(s_aoBlurTex, s_aoBlur);

    if (!s_ao || !s_aoBlur) {
        return;
    }

    const int32_t mode = s_cvarSsao->m_intValue;

    // The player's two knobs scale the compiled-in defaults. Clamped here, so a hand-edited Config.wtf cannot hand
    // the shader a radius of zero (no occlusion at all) or one wide enough to shade the whole screen.
    float spread = s_cvarSpread ? s_cvarSpread->m_floatValue : 1.0f;
    float strength = s_cvarStrength ? s_cvarStrength->m_floatValue : kStrength;
    spread = std::min(std::max(spread, 0.1f), 4.0f);
    strength = std::min(std::max(strength, 0.0f), 1.0f);
    const DepthMapping mapping = MapWorldDepth(m, minZ, maxZ);

    const float constants[24] = {
        1.0f / size.x, 1.0f / size.y, minX, minY,
        m[0], m[5], m[8], m[9],
        mapping.atInfinity, mapping.perInverseViewDepth, mapping.deepest, kMaxViewDepth,
        kRadius * spread, strength, kBias * spread, maxX - minX,
        kFadeStart, kFadeEnd, maxY - minY, kBlurRadius,
        kBlurDepthSigma, excludeStamped && s_exclude ? 1.0f : 0.0f, 0.0f, 0.0f,
    };

    // The engine's own full-screen setup: pixel-space ortho projection, identity view and world, the AO target as
    // the colour target, and the render-state push that covers everything set from here on.
    FFX::SetupGx(&s_aoTex);
    GxXformSetViewport(0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 1.0f);

    // The quad is transformed by the fixed-function pipeline, so a vertex shader left over from the world's last
    // batch must not transform it first.
    GxRsSet(GxRs_VertexShader, nullptr);
    GxRsSet(GxRs_BlendingMode, GxBlend_Opaque);
    GxRsSet(GxRs_ColorWrite, 15);
    GxRsSet(GxRs_AlphaRef, 0);

    GxShaderConstantsSet(GxSh_Pixel, 0, constants, 6);

    // Occupancy.
    GxRsSet(GxRs_PixelShader, s_aoPS);
    GxRsSet(GxRs_Texture0, depth);
    // Stage 1 is always a real texture, even when nothing was stamped: the shader's flag says whether to read it.
    GxRsSet(GxRs_Texture1, excludeStamped && s_exclude ? s_exclude : depth);
    DrawFullscreenQuad(s_aoTex);

    // Blur.
    GxRenderTargetSet(GxBuffers_Color, s_aoBlur, 0);
    GxRsSet(GxRs_PixelShader, s_blurPS);
    GxRsSet(GxRs_Texture0, s_ao);
    GxRsSet(GxRs_Texture1, depth);
    DrawFullscreenQuad(s_aoBlurTex);
    GxRsSet(GxRs_Texture1, nullptr);

    // Composite onto the frame: multiply by the term, or, for the debug view, replace the frame with it. Back to
    // the default back buffer, clipped to the 3D rectangle the world drew into.
    GxRenderTargetSet(GxBuffers_Color, nullptr, 0);
    GxXformSetViewport(minX, maxX, minY, maxY, 0.0f, 1.0f);
    GxRsSet(GxRs_PixelShader, s_compositePS);
    GxRsSet(GxRs_BlendingMode, mode > 1 ? GxBlend_Opaque : GxBlend_Mod);
    GxRsSet(GxRs_Texture0, s_aoBlur);
    DrawFullscreenQuad(s_aoBlurTex);

    GxRsSet(GxRs_Texture0, nullptr);
    FFX::SetupReset();
}
