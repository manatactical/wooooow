// Not in the original 3.3.5a binary. Blur and depth fog over the finished world frame while the camera is underwater;
// see UnderwaterFx.h.
#include "Engine/Source/UnderwaterFx/UnderwaterFx.h"

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

// The default of the underwaterFxStrength cvar.
constexpr float kStrength = 0.9f;

// Optical depth reached at the engine's fog end when the strength cvar is 0.5: the fog is about 85% complete there.
constexpr float kOpticalDepthAtFogEnd = 1.0f;

// The fog end is clamped to this range before it scales the fog: a tiny value would turn the screen to paint, a huge
// one (a liquid with no underwater fog at all) would leave no fog.
constexpr float kMinFogEnd = 25.0f;
constexpr float kMaxFogEnd = 160.0f;

// Blur, in pixels at 1080 lines (scaled to the frame's height) when the strength cvar is 0.5: a little everywhere, then
// growing with distance to a cap.
constexpr float kBlurBasePixels = 0.5f;
constexpr float kBlurPixelsPerYard = 0.08f;
constexpr float kBlurMaxPixels = 5.0f;

// Seconds the effect takes to ramp in or out as the camera crosses the surface.
constexpr float kFadeSeconds = 0.6f;

// The stored depth of a pixel with no geometry; see MapWorldDepth.
constexpr float kMinViewportDepthExtent = 0.01f;
constexpr float kDeepestWorldDepthInFullRangeViewport = 0.9999995f;
constexpr float kWorldDepthMargin = 2.0e-6f;
constexpr float kSkyMinDepth = 0.99903f;
constexpr float kMaxViewDepth = 2000.0f;

// The shimmer, in pixels at 1080 lines (scaled to the frame's height) when the strength cvar is 0.5. Underwater the
// picture sways by this at range; the world above the surface sways and blurs far more, so it reads as seen through
// rippling water.
constexpr float kWobblePixels = 1.8f;
constexpr float kExteriorBlurPixels = 5.0f;
constexpr float kExteriorTint = 0.45f;

// The distance, in yards, at which the water is fully opaque whatever the strength: nothing past it shows.
constexpr float kOpaqueYards = 95.0f;

constexpr int32_t kConstantRows = 7;

// c0 = { 1/width, 1/height, 0, 0 }
// c1 = { atInfinity, perInverseViewDepth, deepest depth, max view depth }
// c2 = { tint r, tint g, tint b, effect amount 0..1 }
// c3 = { fog density per yard, blur pixels per yard, max blur pixels, base blur pixels }   (blur already in frame pixels)
// c4 = { seconds, wobble pixels, exterior blur pixels, how far the surface is above the camera, in yards }
// c5 = { world z of a view vector: row z of the view matrix, exterior tint amount }
// c6 = { 1/projection x, 1/projection y, view-space z of a unit ray (+1 or -1), yards at which the water is opaque }
const char* const kHLSL = R"HLSL(
float4 c0 : register(c0);
float4 c1 : register(c1);
float4 c2 : register(c2);
float4 c3 : register(c3);
float4 c4 : register(c4);
float4 c5 : register(c5);
float4 c6 : register(c6);

sampler2D sScene : register(s0);
sampler2D sDepth : register(s1);

float4 main(float2 uv : TEXCOORD0) : COLOR0
{
    float depth = tex2Dlod(sDepth, float4(uv, 0, 0)).r;
    bool sky = depth >= 0.99903;
    float z = sky ? c1.w : (depth >= c1.z ? c1.w : min(c1.y / (depth - c1.x), c1.w));
    z = max(z, 0);

    // Whether this pixel is the world above the water: the surface seen from below, and everything past it. The pixel's
    // height above the camera is its view depth times the world-z of its unit ray.
    float3 ray = float3((uv.x - 0.5) * 2 * c6.x, (0.5 - uv.y) * 2 * c6.y, c6.z);
    float height = z * dot(ray, c5.xyz);
    float exterior = sky ? 1 : smoothstep(c4.w * 0.85 - 0.5, c4.w * 0.95 - 0.1, height);

    // The water sways the picture, more so with distance, and a great deal more above the surface.
    float t = c4.x;
    float2 wave = float2(sin(uv.y * 23 + t * 1.3) + 0.7 * sin(uv.y * 9 - t * 0.8) + 0.4 * sin(uv.y * 57 + t * 2.1),
                         cos(uv.x * 19 + t * 1.1) + 0.7 * cos(uv.x * 7 + t * 0.6) + 0.4 * cos(uv.x * 43 - t * 1.9));
    // Scaled by the effect amount, as the blur is: the sway grows in with the fade and shrinks away with it, instead of
    // being at full size from the first frame of the transition and gone on the last.
    float sway = c4.y * lerp(0.35 + 0.65 * saturate(z / 60), 2.5, exterior) * c2.w;
    float2 suv = uv + wave * sway * c0.xy;

    float radius = lerp(min(c3.w + z * c3.y, c3.z), c4.z, exterior) * c2.w;

    float3 colour = tex2Dlod(sScene, float4(suv, 0, 0)).rgb;

    [branch] if (radius > 0.4)
    {
        // Twenty taps on a golden-angle spiral: an even disc without the banding of a ring pattern.
        float3 sum = colour;
        [unroll] for (int i = 0; i < 20; ++i)
        {
            float tap = (i + 0.5) / 20;
            float angle = i * 2.39996323;
            float2 offset = float2(cos(angle), sin(angle)) * sqrt(tap) * radius * c0.xy;
            sum += tex2Dlod(sScene, float4(suv + offset, 0, 0)).rgb;
        }
        colour = sum / 21;
    }

    // Red is absorbed first, blue last, and what is absorbed is replaced by the water's own colour.
    // The optical depth grows faster than the distance does, so the near water is clear and the far water closes in:
    // the further, the less is seen, until nothing is left at about the engine's fog end.
    float3 depthTerm = z * c3.x * float3(1.0, 0.7, 0.55);
    float3 transmit = exp(-depthTerm * (1 + 0.6 * depthTerm));
    // However thin the exponential is, the water closes completely by c6.w yards.
    float3 keep = lerp(1, transmit, c2.w) * (1 - c2.w * smoothstep(c6.w * 0.4, c6.w, z));
    colour = lerp(c2.rgb, colour, keep);

    // The outside is only a hint of itself, in the water's colour. A bright patch (the sun) keeps most of its light.
    float bright = smoothstep(0.7, 1.0, max(colour.r, max(colour.g, colour.b)));
    colour = lerp(colour, c2.rgb, exterior * c5.w * c2.w * (1 - 0.85 * bright));
    return float4(colour, 1);
}
)HLSL";

struct DepthMapping {
    float atInfinity;
    float perInverseViewDepth;
    float deepest;
};

// shortcut: duplicated from the sun glow and ambient occlusion passes (see SunGlow.cpp); a shared header is overdue.
DepthMapping MapWorldDepth(const float* m, float viewportMinZ, float viewportMaxZ) {
    const bool usableRange = viewportMaxZ - viewportMinZ > kMinViewportDepthExtent && viewportMinZ >= 0.0f && viewportMaxZ <= 1.0f;
    const float minZ = usableRange ? viewportMinZ : 0.0f;
    const float extent = usableRange ? viewportMaxZ - viewportMinZ : 1.0f;
    const float maxZ = minZ + extent;
    const float deepest = maxZ >= kDeepestWorldDepthInFullRangeViewport ? kDeepestWorldDepthInFullRangeViewport : maxZ + kWorldDepthMargin;

    return { minZ + extent * m[10], extent * m[14], std::max(deepest, kSkyMinDepth) };
}

// Optimised, and no CGxDevice::Log on the failure path, as in the sun glow pass: a failed compile leaves the shader
// null and the module silently draws nothing.
CGxShader* CompilePixelShader(const char* body) {
    ID3DBlob* code = nullptr;
    ID3DBlob* error = nullptr;
    HRESULT result = D3DCompile(body, strlen(body), "UnderwaterFx", nullptr, nullptr, "main", "ps_3_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &error);

    if (FAILED(result)) {
        FILE* file = DiagnosticLogsEnabled() ? fopen("Logs\\underwaterfx.log", "a") : nullptr;

        if (file) {
            fputs("UnderwaterFx: the pixel shader failed to compile\n", file);

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

// A texture of exactly the given size, bilinear, so the frame's uv addresses the copy and the shared depth alike.
// Copied from the sun glow pass; see there.
bool CreateFrameTex(FFX::Tex& tex, const C2iVector& size) {
    EGxTexFormat format;

    if (g_theGxDevicePtr->Caps().m_rttFormat[GxTex_Argb8888]) {
        format = GxTex_Argb8888;
    } else if (g_theGxDevicePtr->Caps().m_rttFormat[GxTex_Abgr8888]) {
        format = GxTex_Abgr8888;
    } else {
        return false;
    }

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
        tex.m_texture = TextureCreate(GxTex_NonPow2, size.x, size.y, 0, format, format, flags, nullptr, GxTexNullUserFunc, "UnderwaterFx::CreateFrameTex", 0);

        if (!tex.m_texture) {
            return false;
        }

        tex.m_texSize = size;
    }

    tex.m_size = size;
    tex.m_texelSize.x = 1.0f / size.x;
    tex.m_texelSize.y = 1.0f / size.y;

    return TextureGetGxTex(tex.m_texture, 1, nullptr) != nullptr;
}

void DrawFullscreenQuad(const C2iVector& dstSize, const FFX::Tex& source) {
    C3Vector positions[4];
    C2Vector texcoords[4];
    C2Vector offsets[4] = { { 0.0f, 0.0f }, { 0.0f, 0.0f }, { 0.0f, 0.0f }, { 0.0f, 0.0f } };
    C2Vector scale = { 0.0f, 0.0f };

    FFX::SetupGeo_Quad(dstSize, source.m_size, source.m_texSize, positions, texcoords, false);

    CImVector white(0xFF, 0xFF, 0xFF, 0xFF);
    FFX::SetupVB_PCT4(positions, white, texcoords, offsets, scale);

    g_theGxDevicePtr->PrimVertexMask(0x3D1);
    FFX::SetupIB();
    g_theGxDevicePtr->Draw(&FFX::s_quadBatch, 1);
}

// Set when the shader failed to compile; a failure is not retried every frame. Cleared by a device restore.
bool s_compileFailed;

// How much of the effect is showing, 0..1, ramped toward whether the camera is submerged.
float s_amount;
uint32_t s_lastMs;

}  // namespace

CVar* CUnderwaterFx::s_cvarEnabled;
CVar* CUnderwaterFx::s_cvarStrength;
CGxShader* CUnderwaterFx::s_ps;
FFX::Tex CUnderwaterFx::s_sceneTex;
CGxTex* CUnderwaterFx::s_scene;

void CUnderwaterFx::Init() {
    static bool s_registered;

    s_cvarEnabled = CVar::Register("underwaterFx", "underwater blur and fog: 0 off, 1 on", 1, "1", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
    s_cvarStrength = CVar::Register("underwaterFxStrength", "underwater blur and fog strength: 0 none, 1 full", 1, "0.9", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);

    if (s_registered) {
        return;
    }

    s_registered = true;
    g_theGxDevicePtr->AddDeviceRestoredCallback(CUnderwaterFx::OnDeviceRestored);
}

bool CUnderwaterFx::Enabled() {
    return s_cvarEnabled && s_cvarEnabled->m_intValue > 0 && g_theGxDevicePtr && g_theGxDevicePtr->m_context;
}

void CUnderwaterFx::OnDeviceRestored() {
    if (s_ps) {
        g_theGxDevicePtr->ShaderDestroy(s_ps);
    }

    s_compileFailed = false;
    RefreshGxTexture(s_sceneTex, s_scene);
}

bool CUnderwaterFx::EnsureResources() {
    if (!Enabled() || s_compileFailed) {
        return false;
    }

    if (g_theGxDevicePtr->DeviceApi() != GxApi_D3d9 && g_theGxDevicePtr->DeviceApi() != GxApi_D3d9Ex) {
        return false;
    }

    // The scene depth can only be sampled when it is INTZ.
    if (!g_theGxDevicePtr->Caps().m_rttFormat[GxTex_INTZ]) {
        return false;
    }

    if (!s_ps) {
        s_ps = CompilePixelShader(kHLSL);
    }

    if (!s_ps) {
        s_compileFailed = true;
        return false;
    }

    return true;
}

void CUnderwaterFx::Apply(const UnderwaterMedium& medium) {
    if (!Enabled()) {
        return;
    }

    // Ramp toward the target by wall-clock time, so the fade is the same at any frame rate.
    const uint32_t now = OsGetAsyncTimeMs();
    const float elapsed = std::min(static_cast<float>(now - s_lastMs) * 0.001f, 0.1f);
    s_lastMs = now;
    const float step = elapsed / kFadeSeconds;
    s_amount = medium.submerged ? std::min(s_amount + step, 1.0f) : std::max(s_amount - step, 0.0f);

    float strength = s_cvarStrength ? s_cvarStrength->m_floatValue : kStrength;
    strength = std::min(std::max(strength, 0.0f), 1.0f);

    if (s_amount < 0.004f || strength < 0.004f) {
        return;
    }

    if (!CUnitOutline::HasFrameDepth() || g_theGxDevicePtr->m_format.m_multisampleCount > 1) {
        return;
    }

    CGxTex* boundTarget = nullptr;
    g_theGxDevicePtr->DeviceGetRenderTarget(GxBuffers_Color, boundTarget);

    if (boundTarget) {
        return;
    }

    CGxTex* depth = CUnitOutline::DepthPassTexture();
    const CRect& window = g_theGxDevicePtr->DeviceCurWindow();
    C2iVector size = { static_cast<int32_t>(window.maxX), static_cast<int32_t>(window.maxY) };

    if (!depth || static_cast<int32_t>(depth->m_width) != size.x || static_cast<int32_t>(depth->m_height) != size.y) {
        return;
    }

    // The world's own projection and viewport, read before FFX::SetupGx replaces them.
    float minX = 0.0f, maxX = 0.0f, minY = 0.0f, maxY = 0.0f, minZ = 0.0f, maxZ = 0.0f;
    GxXformViewport(minX, maxX, minY, maxY, minZ, maxZ);

    // The pass is built for the whole target, as the anti-aliasing pass is: a world drawn into a sub-rectangle is left
    // alone rather than squeezed into it.
    if (minX > 0.0001f || minY > 0.0001f || maxX < 0.9999f || maxY < 0.9999f) {
        return;
    }

    const C44Matrix projection = g_theGxDevicePtr->m_projNative;
    const float* p = reinterpret_cast<const float*>(&projection);

    if (p[0] == 0.0f || p[5] == 0.0f) {
        return;
    }

    if (!EnsureResources()) {
        return;
    }

    if (!CreateFrameTex(s_sceneTex, size)) {
        return;
    }

    RefreshGxTexture(s_sceneTex, s_scene);

    if (!s_scene) {
        return;
    }

    // The frame as it stands, whole, into the scratch: a surface cannot be read and written in the same pass.
    C2iVector origin = { 0, 0 };

    if (!GxTexCopy(s_scene, origin, origin, size, 0, 0)) {
        return;
    }

    // The world's own viewport z range, not the device's current one: the viewport at this point is not the one the world
    // was drawn with, and decoding against it puts every distance in the wrong place (see CDepthFog).
    DepthMapping mapping = MapWorldDepth(p, minZ, medium.depthMaxZ > minZ ? medium.depthMaxZ : maxZ);

    if (medium.depthMaxZ > minZ) {
        mapping.deepest = medium.depthMaxZ + kWorldDepthMargin;
    }

    C44Matrix view;
    GxXformView(view);
    const float* v = reinterpret_cast<const float*>(&view);

    const float fogEnd = std::min(std::max(medium.fogEnd, kMinFogEnd), kMaxFogEnd);
    const float density = strength * 2.0f * kOpticalDepthAtFogEnd / fogEnd;
    const float blurScale = strength * 2.0f * static_cast<float>(size.y) / 1080.0f;

    const float constants[kConstantRows * 4] = {
        1.0f / size.x, 1.0f / size.y, 0.0f, 0.0f,
        mapping.atInfinity, mapping.perInverseViewDepth, mapping.deepest, kMaxViewDepth,
        // The engine's fog colour leaned toward deep water, so the fog reads as water even where that colour is pale.
        medium.color[0] * 0.65f + 0.02f, medium.color[1] * 0.65f + 0.09f, medium.color[2] * 0.65f + 0.11f, s_amount,
        density, kBlurPixelsPerYard * blurScale, kBlurMaxPixels * blurScale, kBlurBasePixels * blurScale,
        static_cast<float>(now % 1000000u) * 0.001f, kWobblePixels * blurScale, kExteriorBlurPixels * blurScale, std::fabs(medium.surfaceDepth),
        v[8], v[9], v[10], kExteriorTint,
        1.0f / p[0], 1.0f / p[5], p[11] > 0.0f ? 1.0f : -1.0f, kOpaqueYards,
    };

    // A view of the frame itself for SetupGx: the texture is null, so the setup puts the default back buffer back and
    // only the pixel-space projection and the render-state push are wanted from it.
    FFX::Tex frame;
    frame.m_texture = nullptr;
    frame.m_size = size;
    frame.m_texSize = size;

    FFX::SetupGx(&frame);

    GxRsSet(GxRs_VertexShader, nullptr);
    GxRsSet(GxRs_ColorWrite, 15);
    GxRsSet(GxRs_AlphaRef, 0);
    GxShaderConstantsSet(GxSh_Pixel, 0, constants, kConstantRows);

    GxRsSet(GxRs_PixelShader, s_ps);
    GxRsSet(GxRs_BlendingMode, GxBlend_Opaque);
    GxRsSet(GxRs_Texture0, s_scene);
    GxRsSet(GxRs_Texture1, depth);
    DrawFullscreenQuad(size, s_sceneTex);

    GxRsSet(GxRs_Texture0, nullptr);
    GxRsSet(GxRs_Texture1, nullptr);
    FFX::SetupReset();
}
