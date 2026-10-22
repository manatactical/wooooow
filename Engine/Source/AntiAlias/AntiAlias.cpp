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
// video options tab carries the mode and the quality, so the presets are derived from those.
//
// The pass is the same shader and the same nine taps at every level. Two things change:
//   strength  how much of the filtered result is applied over the original.
//   passes    how many times the pass is run. Soothed runs it twice.
//
// The knob matters. It was measured with Tools/model-fxaa.py, which scores against a 16x-supersampled
// reference and also reports how large the change is *on the pixels that move* -- the number that decides
// whether a setting is visible at all. Blend strength moves an edge pixel by 0.03 at the weakest setting and
// 0.23 at the strongest, against one 8-bit level being 0.004.
//
// Two earlier versions of this table were inert, and both looked plausible:
//   - a per-level edge threshold. Every preset measured identically, because the absolute floor in the shader
//     (0.0625, i.e. 16 luma levels) is what actually gates the pass on rendered frames.
//   - a per-level edge span. It changed the image by at most 0.006 luma at the strongest edge, which is about
//     one 8-bit level: real, monotonic, and far too small to see. Its own test passed because it only checked
//     that the number moved in the right direction.
constexpr int32_t kQualityLevels = 5;

// c0.z: how much of the filtered colour is applied, 0 keeping the frame untouched and 1 taking the full pass.
// Ultra is the full pass, so the strongest single-pass setting reproduces exactly the image signed off in game.
constexpr float kQualityStrength[kQualityLevels] = { 0.60f, 0.75f, 0.90f, 1.00f, 1.00f };

// c0.w: damping the four-neighbour average applies to the edge direction. Higher means the blur reaches
// further along a faint edge.
constexpr float kQualityReduceMul[kQualityLevels] = { 0.125f, 0.187f, 0.250f, 0.375f, 0.375f };

// How many times the pass runs. Soothed, the last level, runs it twice, and does so with the thin-feature term
// switched off: that term exists to protect one-pixel detail, and Soothed is the setting for someone who would
// rather lose the detail than keep the stair step. Measured against Ultra it moves an edge pixel by 0.043
// (double the Ultra-to-High step) and takes texture detail from 0.096 to 0.150, while smoothing shallow
// staircases better than Ultra does -- two passes compound on the ones the single pass only partly removes.
constexpr int32_t kQualityPasses[kQualityLevels] = { 1, 1, 1, 1, 2 };

// The thin-feature term's weight, c1.x. Off for Soothed, on for everything else.
constexpr float kQualityThin[kQualityLevels] = { 0.75f, 0.75f, 0.75f, 0.75f, 0.0f };

// The default, matching the cvar's own default: High.
constexpr int32_t kQualityDefault = 2;

// The shader. One fullscreen pass, written for ps_2_0 so every shader model the client can select is served
// by the one blob: 9 texel fetches, which is the ps_2_0 texture-instruction ceiling, and no loop. The luma
// weights are Rec.601, the standard FXAA ones.
//
// c0 = { 1/texWidth, 1/texHeight, blend strength, kReduceMul }
// c1 = { thin-feature weight, unused, unused, unused }
// s0 = the world frame, captured before the pass.
//
// This is the FXAA2/3 formulation. The four diagonal luminances and the centre's bound the local range; a
// pixel whose range is under the threshold is not on an edge, and its colour passes through untouched, so
// flat interiors and already-sharp detail keep every bit of their contrast. When the pixel is on an edge,
// the four corner luminances give the edge's direction, and the colour is resampled *along* that direction
// rather than across it: sampling along the edge keeps the gradient the rasteriser made, which is what the
// eye reads as a smooth line, where blending across it would soften both sides of the silhouette.
//
// The last two lines are the thin-feature term, which v1.0 did not have and which is where most of the
// improvement comes from. The diagonal average sits close to the centre on any wide edge and far from it on a
// one-pixel line; blending the line back toward its original colour is what stops the filter erasing thin
// geometry -- fences, ropes, foliage, the edge of a weapon -- while leaving wide edges fully filtered.
const char* const kAntiAliasHLSL =
    "sampler2D s0 : register(s0);\n"
    "float4 px : register(c0);\n"
    "float4 thin : register(c1);\n"
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
    "  if (range < 0.0625) { return float4(rgbM, 1.0); }\n"
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
    "  float lAvg = (lNW + lNE + lSW + lSE) * 0.25;\n"
    "  float thin = saturate(1.0 - abs(lAvg - lM) / range);\n"
    "  thin = thin * thin * thin.x;\n"
    "  return float4(lerp(rgbM, rgbB, px.z * (1.0 - thin)), 1.0);\n"
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

    // The bytecode came from D3DCompile, not from a .bls container, so the permute is filled directly and the
    // backend creates the device object. m_data belongs to the shader after this point (CGxShader::~CGxShader
    // frees it), and the permute reads the bytecode out of it.
    uint32_t bytecodeSize = static_cast<uint32_t>(code->GetBufferSize());
    uint8_t* bytecode = static_cast<uint8_t*>(SMemAlloc(bytecodeSize, __FILE__, __LINE__, 0));
    memcpy(bytecode, code->GetBufferPointer(), bytecodeSize);
    shader->m_data = bytecode;
    code->Release();

    shader->m_permutes.SetCount(1);
    CGxShaderPermute& permute = shader->m_permutes[0];
    permute.m_index = 0;
    permute.m_shader = shader;
    permute.m_apiSpecific = nullptr;
    permute.m_target = static_cast<uint8_t>(GxSh_Pixel);

    // A full-screen pass takes no vertex inputs and uploads no vertex constants, but the fields must be
    // defined: the input-layout and constant-flush paths read them.
    permute.SetInterface(bytecodeSize, 0, 0);

    if (g_theGxDevicePtr) {
        g_theGxDevicePtr->ShaderCreateCompiled(&permute, bytecode, bytecodeSize);
    }

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
FFX::Tex CAntiAlias::s_sceneTex2;
CGxTex* CAntiAlias::s_scene2;

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
    s_cvarFxaaQuality = CVar::Register("fxaaQuality", "FXAA quality: 0 low, 1 medium, 2 high, 3 ultra, 4 soothed",
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
    RefreshGxTexture(s_sceneTex2, s_scene2);
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

    // Only needed by the two-pass preset, but sized here so a preset switch mid-session does not allocate
    // during a frame, and so a device restore has it already created.
    FFX::CreateTex(s_sceneTex2, size, false, true);
    RefreshGxTexture(s_sceneTex2, s_scene2);

    return s_scene != nullptr && s_scene2 != nullptr;
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
    GxRsSet(GxRs_PixelShader, s_shader, 0);
    GxRsSet(GxRs_BlendingMode, GxBlend_Opaque);
    GxRsSet(GxRs_ColorWrite, 15);
    GxRsSet(GxRs_AlphaRef, 0);
    // The step is the scratch's own texel size, not one over the frame's, because the two differ when the
    // allocation was rounded up to a power of two. The blend strength and the damping come from the preset.
    int32_t quality = s_cvarFxaaQuality ? s_cvarFxaaQuality->m_intValue : kQualityDefault;

    if (quality < 0) {
        quality = 0;
    } else if (quality >= kQualityLevels) {
        quality = kQualityLevels - 1;
    }

    float constants[4] = { s_sceneTex.m_texelSize.x, s_sceneTex.m_texelSize.y, kQualityStrength[quality], kQualityReduceMul[quality] };
    GxShaderConstantsSet(GxSh_Pixel, 0, constants, 1);

    float thinConstants[4] = { kQualityThin[quality], 0.0f, 0.0f, 0.0f };
    GxShaderConstantsSet(GxSh_Pixel, 1, thinConstants, 1);

    GxRsSet(GxRs_Texture0, s_scene);
    DrawFullscreenQuad(frameSize, s_sceneTex);

    // Soothed only. The second pass cannot read the target it writes, so it reads the first pass's result out
    // of the scratch and writes the frame; the first pass above already wrote over the frame from the other
    // scratch. Both draws share the states set above.
    if (kQualityPasses[quality] > 1) {
        CGxTex* result = nullptr;
        g_theGxDevicePtr->DeviceGetRenderTarget(GxBuffers_Color, result);

        if (result == nullptr) {
            C2iVector pos = { 0, 0 };

            if (GxTexCopy(s_scene2, pos, pos, frameSize, 0, 0)) {
                GxRsSet(GxRs_Texture0, s_scene2);
                DrawFullscreenQuad(frameSize, s_sceneTex2);
            }
        }
    }


    GxRsSet(GxRs_Texture0, nullptr);
    FFX::SetupReset();
}