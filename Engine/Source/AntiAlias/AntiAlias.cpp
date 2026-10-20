// Not in the original 3.3.5a binary. A post-process anti-aliasing pass over the finished world frame; see
// AntiAlias.h for the pipeline and for why it sits at the world -> interface boundary. This file is the
// implementation, and says where the port had to differ from the WXL module wxl-vignette-grain it follows.
#include "Engine/Source/AntiAlias/AntiAlias.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

#include <d3dcompiler.h>

#include "Engine/Source/ConsoleBase/ConsoleVar.h"
#include "Engine/Source/ConsoleBase/Types.h"
#include "Engine/Source/FFX/FFX.h"
#include "Engine/Source/Gx/CGxDevice/CGxDevice.h"
#include "Engine/Source/Gx/CGxDevice/CGxShader.h"
#include "Engine/Source/Gx/CGxTex.h"
#include "Engine/Source/Gx/Gx.h"
#include "Engine/Source/Services/Texture.h"
#include "Engine/Source/Tempest/C2Vector.h"
#include "Engine/Source/Tempest/C2iVector.h"
#include "Engine/Source/Tempest/C3Vector.h"
#include "Engine/Source/Tempest/CImVector.h"

namespace {

// The look. The WXL module read these from an INI beside its DLL and exposed them in an overlay panel; the
// video options tab carries the mode and the quality, so the thresholds are derived from those.
//
// The pass is the same shader and the same nine taps at every quality level. What changes is how eagerly a
// gradient counts as an edge and how much the surrounding luma damps a long edge direction. Low only filters
// the strong, obvious silhouettes and leaves everything else exactly as it was rasterised, which is the
// cheapest and the least visible on a still frame. Ultra also filters faint gradients -- the shimmer on thin
// geometry and foliage -- at the cost of softening detailed textures. The middle two are FXAA's own shipping
// values and the default.
constexpr int32_t kQualityLevels = 4;

// c0.z: relative low-contrast cutoff, times the local maximum. Lower means more gradients count as edges.
constexpr float kQualityEdgeMul[kQualityLevels] = { 0.250f, 0.200f, 0.166f, 0.125f };

// c0.w: damping the four-neighbour average applies to the edge direction. Higher means the blur reaches
// further along a faint edge.
constexpr float kQualityReduceMul[kQualityLevels] = { 0.125f, 0.187f, 0.250f, 0.375f };

// The default, matching the cvar's own default: FXAA's shipping thresholds.
constexpr int32_t kQualityDefault = 2;

// The shader. One fullscreen pass, written for ps_2_0 so every shader model the client can select is served
// by the one blob: 9 texel fetches, which is the ps_2_0 texture-instruction ceiling, and no loop. The luma
// weights are Rec.601, the standard FXAA ones.
//
// c0 = { 1/texWidth, 1/texHeight, kEdgeMul, kReduceMul }
// s0 = the world frame, captured before the pass.
//
// This is the FXAA2/3 formulation. The four diagonal luminances and the centre's bound the local range; a
// pixel whose range is under the threshold is not on an edge, and its colour passes through untouched, so
// flat interiors and already-sharp detail keep every bit of their contrast. When the pixel is on an edge,
// the four corner luminances give the edge's direction, and the colour is resampled *along* that direction
// rather than across it: sampling along the edge keeps the gradient the rasteriser made, which is what the
// eye reads as a smooth line, where blending across it would soften both sides of the silhouette.
const char* const kAntiAliasHLSL =
    "sampler2D s0 : register(s0);\n"
    "float4 px : register(c0);\n"
    "float4 main(float2 uv : TEXCOORD0) : COLOR0 {\n"
    "  float3 rgbM  = tex2D(s0, uv).rgb;\n"
    "  float3 rgbNW = tex2D(s0, uv + float2(-px.x, -px.y)).rgb;\n"
    "  float3 rgbNE = tex2D(s0, uv + float2( px.x, -px.y)).rgb;\n"
    "  float3 rgbSW = tex2D(s0, uv + float2(-px.x,  px.y)).rgb;\n"
    "  float3 rgbSE = tex2D(s0, uv + float2( px.x,  px.y)).rgb;\n"
    "  float3 luma = float3(0.299, 0.587, 0.114);\n"
    "  float lM  = dot(rgbM,  luma);\n"
    "  float lNW = dot(rgbNW, luma);\n"
    "  float lNE = dot(rgbNE, luma);\n"
    "  float lSW = dot(rgbSW, luma);\n"
    "  float lSE = dot(rgbSE, luma);\n"
    "  float lMin = min(lM, min(min(lNW, lNE), min(lSW, lSE)));\n"
    "  float lMax = max(lM, max(max(lNW, lNE), max(lSW, lSE)));\n"
    "  float range = lMax - lMin;\n"
    "  if (range < max(0.0625, lMax * px.z)) { return float4(rgbM, 1.0); }\n"
    "  float2 dir;\n"
    "  dir.x = -((lNW + lNE) - (lSW + lSE));\n"
    "  dir.y =  ((lNW + lSW) - (lNE + lSE));\n"
    "  float dirReduce = max((lNW + lNE + lSW + lSE) * 0.25 * px.w, 0.0078125);\n"
    "  float rcpDirMin = 1.0 / (min(abs(dir.x), abs(dir.y)) + dirReduce);\n"
    "  dir = clamp(dir * rcpDirMin, -8.0, 8.0) * px.xy;\n"
    "  float3 rgbA = 0.5 * (tex2D(s0, uv + dir * (1.0 / 3.0 - 0.5)).rgb\n"
    "                     + tex2D(s0, uv + dir * (2.0 / 3.0 - 0.5)).rgb);\n"
    "  float3 rgbB = rgbA * 0.5 + 0.25 * (tex2D(s0, uv + dir * -0.5).rgb\n"
    "                                   + tex2D(s0, uv + dir *  0.5).rgb);\n"
    "  return float4(rgbB, 1.0);\n"
    "}\n";

CGxShader* CompilePixelShader(const char* hlsl) {
    ID3DBlob* code = nullptr;
    ID3DBlob* error = nullptr;

    if (FAILED(D3DCompile(hlsl, strlen(hlsl), "AntiAlias", nullptr, nullptr, "main", "ps_2_0", 0, 0, &code, &error))) {
        // The blob's buffer is not guaranteed to be NUL-terminated -- it is a byte count and a pointer -- so
        // passing it straight to %s hands strnlen a run of bytes with no terminator and faults. The length is
        // copied out and terminated here.
        char message[1024] = "(no compiler message)";

        if (error) {
            uint32_t length = static_cast<uint32_t>(error->GetBufferSize());

            if (length >= sizeof(message)) {
                length = sizeof(message) - 1;
            }

            memcpy(message, error->GetBufferPointer(), length);
            message[length] = '\0';
            error->Release();
        }

        CGxDevice::Log("AntiAlias: the pixel shader failed to compile: %s", message);
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

// The quad the pass draws through, built exactly as the engine's own full-screen passes build theirs: FFX
// owns the vertex buffer, the index buffer and the PCT4 layout, and SetupGx has already put the world in the
// pixel-space projection these coordinates are expressed in. dstSize is the part of the target being
// written and source the texture being read, which is what makes the texel-centre inset come out right even
// when the scratch had to be rounded up to a power of two.
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

// Makes the gx texture current. Called after creation and after a device restore, since both leave the
// cached pointer stale.
void RefreshGxTexture(FFX::Tex& tex, CGxTex*& gxTexture) {
    gxTexture = tex.m_texture ? TextureGetGxTex(tex.m_texture, 1, nullptr) : nullptr;
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// Module state
// ---------------------------------------------------------------------------------------------
CVar* CAntiAlias::s_cvarAntiAlias;
CVar* CAntiAlias::s_cvarFxaaQuality;
CGxShader* CAntiAlias::s_shader;
FFX::Tex CAntiAlias::s_sceneTex;
CGxTex* CAntiAlias::s_scene;

// ---------------------------------------------------------------------------------------------
// Endpoints
// ---------------------------------------------------------------------------------------------

// Not in the original binary. Called when a world frame is created, which happens again after a UI reload,
// so the callback registration is guarded: the device's list is append-only and OnDeviceRestored is not safe
// to run twice, because the first pass releases the shader the second one would release again.
void CAntiAlias::Init() {
    static bool s_registered;

    // Registered by CWorldParam::Initialize so the options panel can read its default at the login screen.
    // This call is the safety net for a console that never ran that, and Register reuses an existing var.
    s_cvarAntiAlias = CVar::Register("antiAlias", "anti-aliasing mode: 0 off, 1 FXAA",
                                     1, "0", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);

    // The FXAA quality preset: 0 low, 1 medium, 2 high, 3 ultra. Registered here as well as by
    // CWorldParam::Initialize, for the same reason as the mode above -- the quality dropdown asks for its
    // default before any world frame exists.
    s_cvarFxaaQuality = CVar::Register("fxaaQuality", "FXAA quality: 0 low, 1 medium, 2 high, 3 ultra",
                                       1, "2", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);

    if (s_registered) {
        return;
    }

    s_registered = true;
    g_theGxDevicePtr->AddDeviceRestoredCallback(CAntiAlias::OnDeviceRestored);
}

bool CAntiAlias::Enabled() {
    return s_cvarAntiAlias && s_cvarAntiAlias->m_intValue && g_theGxDevicePtr && g_theGxDevicePtr->m_context;
}

// Not in the original binary. A device reset frees every default-pool texture and the shader object with
// them. The scratch is default-pool with its render-target bit set, so the device re-created it itself and
// only the cached pointer is stale. ShaderDestroy re-nulls the pointer it is given, so it is called for a
// shader that is not null rather than to clear one.
void CAntiAlias::OnDeviceRestored() {
    if (s_shader) {
        g_theGxDevicePtr->ShaderDestroy(s_shader);
    }

    RefreshGxTexture(s_sceneTex, s_scene);
}

// Not in the original binary. Compiles the shader and sizes the scratch to the frame. False means the pass
// cannot run this frame, and the caller does nothing.
bool CAntiAlias::EnsureResources() {
    if (!Enabled()) {
        return false;
    }

    if (g_theGxDevicePtr->DeviceApi() != GxApi_D3d9 && g_theGxDevicePtr->DeviceApi() != GxApi_D3d9Ex) {
        // The shader is D3D9 bytecode. Any other backend -- OpenGL, the GL low-detail path -- would hand
        // the blob to a compiler that is not expecting it.
        return false;
    }

    if (FFX::s_useRECT) {
        // The taps are offset in normalised texture coordinates. A rectangle texture samples in pixels
        // instead, so both the edge direction and the tap distance would be wrong by the texture's size.
        // That only happens on the GL device with ffxRectangle on, and D3D9 is already required above.
        return false;
    }

    if (!s_shader) {
        s_shader = CompilePixelShader(kAntiAliasHLSL);
    }

    if (!s_shader) {
        return false;
    }

    const CRect& window = g_theGxDevicePtr->DeviceCurWindow();
    C2iVector size = { static_cast<int32_t>(window.maxX), static_cast<int32_t>(window.maxY) };

    if (size.x < 8 || size.y < 8) {
        return false;
    }

    // Back-buffer sized so the pass samples it 1:1 and the shader's texel distance holds. Built through
    // FFX::CreateTex, which is a no-op unless the size actually changed and rounds the allocation up to a
    // power of two where the device cannot take the frame's own size; it records the requested size and the
    // allocated size separately, which is what the quad's texel-centre inset is computed from.
    FFX::CreateTex(s_sceneTex, size, false, true);
    RefreshGxTexture(s_sceneTex, s_scene);

    return s_scene != nullptr;
}

// Not in the original binary. The world -> interface boundary. The frame is filtered here so the outline and
// the interface drawn immediately afterwards are the only things that stay outside the pass.
void CAntiAlias::Apply() {
    if (!EnsureResources()) {
        return;
    }

    // Hardware MSAA has already resolved this frame; filtering it as well is cost with no visible return,
    // and the two together fight over the same edges.
    if (g_theGxDevicePtr->m_format.m_multisampleCount > 1) {
        return;
    }

    // The capture reads the default back buffer, whichever target happens to be bound. If the world is
    // drawing somewhere else -- the off-screen buffer the glow effect swaps in, the character selection
    // screen -- the image taken would be the wrong one, and the pass would draw a stale frame over it.
    CGxTex* boundTarget = nullptr;
    g_theGxDevicePtr->DeviceGetRenderTarget(GxBuffers_Color, boundTarget);

    if (boundTarget) {
        return;
    }

    // The world narrows the viewport to the 3D rectangle and FFX::Start shrinks it further when the glow
    // buffer is smaller than the screen. The pass is built for the whole target, so it only runs when the
    // whole target is what was drawn.
    float minX = 0.0f;
    float maxX = 0.0f;
    float minY = 0.0f;
    float maxY = 0.0f;
    float minZ = 0.0f;
    float maxZ = 0.0f;
    GxXformViewport(minX, maxX, minY, maxY, minZ, maxZ);

    if (minX > 0.0001f || minY > 0.0001f || maxX < 0.9999f || maxY < 0.9999f) {
        return;
    }

    // The frame as it stands, whole and unmodified, into the scratch. A surface cannot be read and written
    // in the same pass, and the frame's own target is what the copy is taken from.
    C2iVector framePos = { 0, 0 };
    C2iVector frameSize = { s_sceneTex.m_size.x, s_sceneTex.m_size.y };

    if (!GxTexCopy(s_scene, framePos, framePos, frameSize, 0, 0)) {
        return;
    }

    // A view of the frame itself for SetupGx: the texture is null, so the setup puts the default back
    // buffer back and only the pixel-space projection and the render-state push are wanted from it.
    FFX::Tex frame;
    frame.m_texture = nullptr;
    frame.m_size = frameSize;
    frame.m_texSize = frameSize;

    FFX::SetupGx(&frame);

    // The quad is transformed by the fixed-function pipeline, so a vertex shader left over from the world's
    // last batch must not transform it first. The colour comes from the vertex buffer, which is why the mask
    // includes GxVA_Color0 (see FFX::SetupVB_PCT4's own passes).
    GxRsSet(GxRs_VertexShader, nullptr);
    GxRsSet(GxRs_PixelShader, s_shader);
    GxRsSet(GxRs_BlendingMode, GxBlend_Opaque);
    GxRsSet(GxRs_ColorWrite, 15);
    GxRsSet(GxRs_AlphaRef, 0);
    GxRsSet(GxRs_Texture0, s_scene);

    // The step is the scratch's own texel size, not one over the frame's, because the two differ when the
    // allocation was rounded up to a power of two. The thresholds come from the quality preset.
    int32_t quality = s_cvarFxaaQuality ? s_cvarFxaaQuality->m_intValue : kQualityDefault;

    if (quality < 0) {
        quality = 0;
    } else if (quality >= kQualityLevels) {
        quality = kQualityLevels - 1;
    }

    float constants[4] = { s_sceneTex.m_texelSize.x, s_sceneTex.m_texelSize.y, kQualityEdgeMul[quality], kQualityReduceMul[quality] };
    GxShaderConstantsSet(GxSh_Pixel, 0, constants, 1);

    DrawFullscreenQuad(frameSize, s_sceneTex);

    GxRsSet(GxRs_Texture0, nullptr);
    FFX::SetupReset();
}