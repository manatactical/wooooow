// Not in the original 3.3.5a binary. The unit outline: a reaction-coloured silhouette line on the
// mouseover and the target unit, ported from the WXL module wxl-mana-unit-outline (WarcraftXL), where
// the same four pixel shaders were driven by the core's render events. See UnitOutline.h for the
// pipeline; this file is the implementation, and says where the port had to differ from the original.
#include "Engine/Source/UnitOutline/UnitOutline.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include <d3dcompiler.h>

#include "Engine/Source/ConsoleBase/ConsoleVar.h"
#include "Engine/Source/ConsoleBase/Types.h"
#include "Engine/Source/FFX/FFX.h"
#include "Engine/Source/Gx/CGxDevice/CGxDevice.h"
#include "Engine/Source/Gx/CGxDevice/CGxShader.h"
#include "Engine/Source/Base/Handle.h"
#include "Engine/Source/Gx/CGxTex.h"
#include "Engine/Source/Gx/CGxTexFlags.h"
#include "Engine/Source/Gx/Gx.h"
#include "Engine/Source/Model2/M2Model.h"
#include "Engine/Source/Os/TimeManager.h"
#include "Engine/Source/Services/Texture.h"
#include "Engine/Source/Tempest/C2Vector.h"
#include "Engine/Source/Tempest/C2iVector.h"
#include "Engine/Source/Tempest/C3Vector.h"
#include "Engine/Source/Tempest/CImVector.h"

// The blend state the module sets and reads. GxBlend_Alpha is the engine's own name for
// SRCALPHA / INVSRCALPHA, which is what the WXL module set by hand through D3D9.
static_assert(GxBlend_Opaque == 0 && GxBlend_Alpha == 2, "the stamp compares blend modes by number");

namespace {

// ---------------------------------------------------------------------------------------------
// The look. The WXL module read these from an INI beside its DLL and exposed them in an overlay panel;
// the video options tab carries only the master switch, so they are constants here. Distances are in
// screen pixels.
// ---------------------------------------------------------------------------------------------
constexpr float kThickness = 1.75f;   // edge width
constexpr float kIntensity = 4.0f;    // edge accumulation weight (brightness)
constexpr float kOpacity   = 1.0f;    // final outline alpha scale
constexpr float kSoftness  = 0.225f;  // blur radius, times kSoftnessScale
constexpr float kThreshold = 0.0f;    // clip cutoff; trims faint pixels

// Blur radius, in mask texels, that kSoftness maps to. The mask is back-buffer sized, so one texel is
// one screen pixel and the value reads as a couple of pixels of softening: enough to take the hard step
// off a spike or a shoulder without visibly fattening the line.
constexpr float kSoftnessScale = 2.0f;

constexpr int32_t kMaxTargets   = 16;  // 2 selections, each plus its mount chain
constexpr int32_t kMaxModelHops = 6;   // attachment-chain depth
constexpr uint32_t kFadeMs         = 400;
constexpr float    kFadeEndOpacity = 0.5f;

// ---------------------------------------------------------------------------------------------
// The shaders. Ported from the WXL module's hlsl (its Hlsl.hpp); the port change is that they are
// compiled here, on the first frame that needs them, instead of living in a .bls container the game
// ships. The WXL module issued the device's DrawIndexedPrimitive directly from a batch event; this port
// draws the batch through the gx facade, so these are ordinary pixel shaders it binds. They use c0 and
// c1 because the engine's own pixel shaders start at c2.
// ---------------------------------------------------------------------------------------------

// Writes opaque model batches into the mask. The model's own diffuse alpha is deliberately not sampled:
// many diffuse alpha channels carry material or detail data, and would turn the silhouette into internal
// noise.
const char* const kColorHLSL =
    "float4 c0 : register(c0);\n"
    "float4 main(float2 uv : TEXCOORD0) : COLOR0 {\n"
    "  return c0;\n"
    "}\n";

// Writes alpha-tested batches into the mask, following the real cutouts (wings, hair cards, drapes).
const char* const kCutoutColorHLSL =
    "sampler2D s0 : register(s0);\n"
    "float4 c0 : register(c0);\n"
    "float4 main(float2 uv : TEXCOORD0) : COLOR0 {\n"
    "  clip(tex2D(s0, uv).a - 0.5);\n"
    "  return c0;\n"
    "}\n";

// 8-tap edge detect on the mask: a thin anti-aliased line outside the silhouette.
//   c0 = {texel.x, texel.y, thickness, intensity}   (texel is 1/mask texture size; thickness in pixels)
//   c1 = {opacity, threshold, player-mask-on, unused} (scale on the default alpha, clip cutoff, 1 when
//        the player mask was stamped this frame so a stale silhouette cannot punch the line)
//
// The mask stores premultiplied colour (rgb * weight, weight), where the weight lets a target's edge
// brightness vary (the fade). rgb/alpha recovers the colour; the interior test is center-vs-average, so
// it stays correct whatever the weight is.
//
// The tap ring is blown up to BLUR_TAP times the configured thickness and its weight rolled off towards
// the ring's outside, so a pointy model's edge (shoulders, spikes, weapon tips) lands as a soft gradient
// instead of a hard 1-pixel step. The line still thins and thickens with the setting, because the ring
// and the roll-off both scale with px.z.
const char* const kEdgeHLSL =
    "sampler2D m : register(s0);\n"
    "sampler2D occ : register(s1);\n"
    "float4 px : register(c0);\n"
    "float4 par : register(c1);\n"
    "#define BLUR_TAP 1.7\n"
    "float4 main(float2 uv : TEXCOORD0) : COLOR0 {\n"
    "  float2 o = px.xy * px.z * BLUR_TAP;\n"
    "  float4 a = tex2D(m,uv+float2(o.x,0)) + tex2D(m,uv+float2(-o.x,0))\n"
    "           + tex2D(m,uv+float2(0,o.y)) + tex2D(m,uv+float2(0,-o.y))\n"
    "           + tex2D(m,uv+float2(o.x,o.y)) + tex2D(m,uv+float2(-o.x,-o.y))\n"
    "           + tex2D(m,uv+float2(o.x,-o.y)) + tex2D(m,uv+float2(-o.x,o.y));\n"
    "  float center = tex2D(m, uv).a;\n"
    "  float inside = a.a > 0.0 ? saturate(center * 8.0 / a.a) : 0.0;\n"
    "  float outline = saturate(a.a * (0.125 / BLUR_TAP) * px.w) * (1.0 - inside);\n"
    "  clip(outline - par.y);\n"
    "  float3 col = a.a > 0.001 ? a.rgb / a.a : float3(1,1,1);\n"
    "  float om = tex2D(occ, uv).a * par.z;\n"
    "  return float4(col, saturate(outline * 1.4 * par.x) * (1.0 - om));\n"
    "}\n";

// A separable gaussian tap for the softening pass: 5 explicit samples along one axis, written out rather
// than looped because ps_2_0 only compiles the taps reliably as literals. Run once per axis and the
// silhouette the edge pass samples has a gradient instead of a hard 1-pixel boundary; the interior is a
// flat plateau, so only the rim actually widens. The spacing is a fixed fraction of a texel, not a
// multiple of Thickness: at that scale Thickness reads as line shape, and blurring it proportionally
// would just mud the whole line.
//   c0 = {offset.x, offset.y, unused, unused}
const char* const kBlurHLSL =
    "sampler2D src : register(s0);\n"
    "float4 dir : register(c0);\n"
    "float4 main(float2 uv : TEXCOORD0) : COLOR0 {\n"
    "  float4 s = tex2D(src, uv) * 0.2270270270;\n"
    "  s += (tex2D(src, uv + dir.xy) + tex2D(src, uv - dir.xy)) * 0.1945945946;\n"
    "  s += (tex2D(src, uv + dir.xy * 2.0) + tex2D(src, uv - dir.xy * 2.0)) * 0.1216216216;\n"
    "  s += (tex2D(src, uv + dir.xy * 3.0) + tex2D(src, uv - dir.xy * 3.0)) * 0.0540540541;\n"
    "  s += (tex2D(src, uv + dir.xy * 4.0) + tex2D(src, uv - dir.xy * 4.0)) * 0.0162162162;\n"
    "  return s;\n"
    "}\n";

// ---------------------------------------------------------------------------------------------
// Module state
// ---------------------------------------------------------------------------------------------
CVar* s_cvarUnitOutline;
CGxShader* s_colorPS;
CGxShader* s_cutoutPS;
CGxShader* s_edgePS;
CGxShader* s_blurPS;

// The mask targets, held as FFX::Tex so the engine's own texture creation can own them: it picks the
// format and texture target the device supports, rounds the size to what a plain texture needs, clamps it
// to the device's maximum, and re-creates on a device change. The gx texture is cached beside each one
// because the batch stamps resolve it once per batch.
FFX::Tex s_maskTex;
FFX::Tex s_playerMaskTex;
FFX::Tex s_blurTex;
CGxTex* s_mask;
CGxTex* s_playerMask;
CGxTex* s_blur;

// The frame's depth, held so the stamps can test against the scene. Null on a device that cannot provide
// one (see OcclusionSupported), and the stamps then run as they did before -- no depth test at all.
CTexture* s_depthTexture;
CGxTex* s_depth;
C2iVector s_depthSize;
bool s_depthBound;
// TEMP diagnostic. Whether the device refused the depth bind this frame -- see BeginDepthPass -- and what
// the stamp saw for its depth test. Delete with the investigation.
bool s_depthBindMismatch;
int32_t s_stampDepthTest;
// The depth target that was bound before this module took over. Null means the device's own surface, which
// DeviceSetRenderTarget re-derives from the colour target; that is the state CWorld::Render expects back.
CGxTex* s_savedDepth;

bool s_maskCleared;
bool s_playerMaskCleared;

struct OutlineTarget {
    CM2Model* m_model;
    float m_color[4];
    // A quest giver is a standing "quest here" marker the player never selected, so it skips the fade and
    // stays at full weight instead of settling into the weaker line a hovered or targeted unit does.
    bool m_keepFull;
    uint32_t m_birth;
};

OutlineTarget s_targets[kMaxTargets];
int32_t s_count;

// Last frame's list. A model that is still selected keeps the tick it first appeared on, so the fade does
// not restart from full strength every time the list is rebuilt -- which is every frame.
OutlineTarget s_prevTargets[kMaxTargets];
int32_t s_prevCount;

CM2Model* s_playerModel;

// Re-entry guard. A stamp reached from inside the composite would write into the mask the passes are
// reading; nothing in the engine does that today, and the guard is what keeps it harmless if something
// ever starts to.
bool s_inComposite;

// Compiles one of the shaders above. D3DCompile rather than a .bls container, because these sources are
// not in the game's data; the device takes whatever CreatePixelShader accepts.
CGxShader* CompilePixelShader(const char* hlsl) {
    ID3DBlob* code = nullptr;
    ID3DBlob* error = nullptr;

    if (FAILED(D3DCompile(hlsl, strlen(hlsl), "UnitOutline", nullptr, nullptr, "main", "ps_2_0", 0, 0, &code, &error))) {
        CGxDevice::Log("UnitOutline: a pixel shader failed to compile: %s",
                       error ? static_cast<const char*>(error->GetBufferPointer()) : "(no compiler message)");

        if (error) {
            error->Release();
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

// Makes the gx texture of a mask current. Called after creation and after a device restore, since both
// leave the cached pointer stale.
void RefreshGxTexture(FFX::Tex& tex, CGxTex*& gxTexture) {
    gxTexture = tex.m_texture ? TextureGetGxTex(tex.m_texture, 1, nullptr) : nullptr;
}

// A frame-sized render target. FFX::CreateTex is not used because it rounds the size up to a power of two,
// and these masks can no longer be rounded: each has to be exactly the frame's size so the scene's depth
// surface -- which is that size -- can be paired with it when a stamp depth tests.
bool CreateFrameTex(FFX::Tex& tex, const C2iVector& size, bool bit8) {
    EGxTexFormat format;

    if (g_theGxDevicePtr->Caps().m_rttFormat[GxTex_Argb8888]) {
        format = GxTex_Argb8888;
    } else if (g_theGxDevicePtr->Caps().m_rttFormat[GxTex_Abgr8888]) {
        format = GxTex_Abgr8888;
    } else {
        format = g_theGxDevicePtr->Caps().m_rttFormat[GxTex_Rgb565] ? GxTex_Rgb565 : GxTex_Unknown;
    }

    if (format == GxTex_Unknown || size.x < 8 || size.y < 8) {
        return false;
    }

    // GxTexCreate refuses a size that is not a power of two for every target but GxTex_NonPow2 and
    // GxTex_Rectangle (see its guard in Gx.cpp). The masks want the frame's exact size so they can share its
    // depth surface, so the non-pow2 target is asked for directly where the device has one. Without it the
    // size is rounded up instead -- the masks still draw, and the depth test stands down, because a rounded
    // mask is no longer the depth surface's size. FFX::GetTexTarget cannot be used here for the same reason
    // it rounds: with ffxRectangle on it answers GxTex_2d and leaves the non-pow2 target unused.
    EGxTexTarget target = g_theGxDevicePtr->Caps().m_texTarget[GxTex_NonPow2] == 1 ? GxTex_NonPow2 : GxTex_2d;
    int32_t width = size.x;
    int32_t height = size.y;

    if (target == GxTex_2d) {
        for (width = 1; size.x > width; width *= 2) {
        }

        for (height = 1; size.y > height; height *= 2) {
        }
    }

    int32_t maxSize = static_cast<int32_t>(g_theGxDevicePtr->Caps().m_texMaxSize[target]);

    if (width > maxSize || height > maxSize) {
        return false;
    }

    if (!tex.m_texture || tex.m_texSize.x != width || tex.m_texSize.y != height) {
        if (tex.m_texture) {
            HandleClose(reinterpret_cast<HOBJECT>(tex.m_texture));
            tex.m_texture = nullptr;
        }

        CGxTexFlags flags(GxTex_Linear, 0, 0, 0, 0, 1, 1, bit8, 0, 0);
        tex.m_texture = TextureCreate(target, width, height, 0, format, format, flags, nullptr, GxTexNullUserFunc, "UnitOutline::CreateFrameTex", 0);

        if (!tex.m_texture) {
            return false;
        }

        tex.m_texSize.x = width;
        tex.m_texSize.y = height;
    }

    // m_size is the part the frame draws into and m_texSize the whole texture; they are equal on the
    // non-pow2 target and differ only on the rounded fallback, which is exactly what the composite's quad
    // and the shaders' texel size need to know.
    tex.m_size = size;
    tex.m_texelSize.x = 1.0f / tex.m_texSize.x;
    tex.m_texelSize.y = 1.0f / tex.m_texSize.y;

    // The handle is non-null even when the device refused the texture, so the gx texture is what says whether
    // this actually worked; without it the stamps would silently draw nothing.
    return TextureGetGxTex(tex.m_texture, 1, nullptr) != nullptr;
}

// The quad the two passes draw through, built exactly as the engine's own full-screen passes build theirs:
// FFX owns the vertex buffer, the index buffer and the PCT4 layout. FFX::SetupGx has already put the world
// in the pixel-space projection these coordinates are expressed in.
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

// ---------------------------------------------------------------------------------------------
// Endpoints
// ---------------------------------------------------------------------------------------------

// Not in the original binary. Called when a world frame is created, which happens again after a UI reload,
// so the callback registration is guarded: the device's list is append-only and OnDeviceRestored is not
// safe to run twice, because the first pass nulls every shader pointer it destroys.
void CUnitOutline::Init() {
    static bool s_registered;

    // Registered by CWorldParam::Initialize so the options panel can read its default at the login screen.
    // This call is the safety net for a console that never ran that, and Register reuses an existing var.
    s_cvarUnitOutline = CVar::Register("unitOutline", "Outline the mouseover and target units",
                                       1, "1", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);

    if (s_registered) {
        return;
    }

    s_registered = true;
    g_theGxDevicePtr->AddDeviceRestoredCallback(CUnitOutline::OnDeviceRestored);
}

bool CUnitOutline::Enabled() {
    return s_cvarUnitOutline && s_cvarUnitOutline->m_intValue && g_theGxDevicePtr && g_theGxDevicePtr->m_context;
}

// Not in the original binary. A device reset frees every default-pool texture and the shader objects with
// them. The mask textures have their render-target bit set, so the device has already had them re-created
// by the time this runs; only the cached gx pointers need refreshing.
//
// The shader pointers can still be null here: this callback is registered from Init(), which runs from the
// CGWorldFrame constructor, while the shaders are not compiled until EnsureResources() -- so any reset before
// the first composite would otherwise dereference null. ShaderDestroy re-nulls the pointer it is given, so
// the call still has to happen for the ones that are not null.
void CUnitOutline::OnDeviceRestored() {
    if (s_colorPS)  { g_theGxDevicePtr->ShaderDestroy(s_colorPS); }
    if (s_cutoutPS) { g_theGxDevicePtr->ShaderDestroy(s_cutoutPS); }
    if (s_edgePS)   { g_theGxDevicePtr->ShaderDestroy(s_edgePS); }
    if (s_blurPS)   { g_theGxDevicePtr->ShaderDestroy(s_blurPS); }

    RefreshGxTexture(s_maskTex, s_mask);
    RefreshGxTexture(s_playerMaskTex, s_playerMask);
    RefreshGxTexture(s_blurTex, s_blur);

    // The depth texture is default-pool with its render-target bit set, so the device re-created it too and
    // only the cached pointer is stale.
    s_depth = nullptr;
    s_depthSize.x = 0;
    s_depthSize.y = 0;
    s_depthBound = false;

    // The mutation guard is cleared too: a reset that somehow arrived mid-composite would otherwise leave
    // the flag set and silently disable every later composite.
    s_inComposite = false;
    s_maskCleared = false;
    s_playerMaskCleared = false;
}

// Not in the original binary.
void CUnitOutline::BeginTargets() {
    s_count = 0;
    s_playerModel = nullptr;
}

// Not in the original binary.
void CUnitOutline::SetPlayerModel(CM2Model* model) {
    s_playerModel = model;
}

// Not in the original binary. The chain is walked because a mounted unit is drawn as more than one M2
// instance: the rider hangs off the mount in the attachment tree, so recording the whole chain is what
// puts the mount on the same side of the line as the rider.
void CUnitOutline::AddTarget(CM2Model* model, const float rgb[3], bool keepFull) {
    if (!model || s_count >= kMaxTargets) {
        return;
    }

    const float color[4] = { rgb[0], rgb[1], rgb[2], 1.0f };

    // Bounded by kMaxModelHops like the lookups below, so every recorded entry is reachable by FindTarget
    // and IsModel; one more here would spend a target slot on a model nothing could ever stamp.
    for (int32_t hop = 0; model && hop < kMaxModelHops; ++hop) {
        AddEntry(model, color, keepFull);
        model = model->m_attachParent;
    }
}

// Not in the original binary.
void CUnitOutline::AddDoodad(CM2Model* model, const float rgb[3]) {
    if (!model || s_count >= kMaxTargets) {
        return;
    }

    const float color[4] = { rgb[0], rgb[1], rgb[2], 1.0f };
    AddEntry(model, color, false);
}

void CUnitOutline::AddEntry(CM2Model* model, const float color[4], bool keepFull) {
    if (!model || s_count >= kMaxTargets) {
        return;
    }

    for (int32_t i = 0; i < s_count; i++) {
        if (s_targets[i].m_model != model) {
            continue;
        }

        // Already recorded. A model can arrive twice -- a quest giver that is also the current target, or
        // a mount recorded from two different selections -- and "must stay visible" wins over whichever
        // entry landed first, because it comes from what the unit is, not from how it was selected. The
        // colour stays as the first caller chose, so targeting keeps the reaction tint over the quest
        // tint.
        if (keepFull) {
            s_targets[i].m_keepFull = true;
        }

        return;
    }

    OutlineTarget& target = s_targets[s_count];

    target.m_model = model;
    target.m_keepFull = keepFull;
    target.m_birth = OsGetAsyncTimeMs();
    memcpy(target.m_color, color, sizeof(target.m_color));

    // A model that was already on screen last frame has been fading for a while, so it keeps its original
    // birth tick rather than replaying the fade. The list is rebuilt every frame, so without this the line
    // would never leave full strength.
    for (int32_t i = 0; i < s_prevCount; i++) {
        if (s_prevTargets[i].m_model == model) {
            target.m_birth = s_prevTargets[i].m_birth;
            break;
        }
    }

    s_count++;
}

// ---------------------------------------------------------------------------------------------
// Picking out the batches that get stamped
// ---------------------------------------------------------------------------------------------

int32_t CUnitOutline::FindTarget(CM2Model* model) {
    for (int32_t hop = 0; model && hop < kMaxModelHops; ++hop, model = model->m_attachParent) {
        for (int32_t i = 0; i < s_count; i++) {
            if (s_targets[i].m_model == model) {
                return i;
            }
        }
    }

    return -1;
}

bool CUnitOutline::IsModel(CM2Model* model, CM2Model* want) {
    if (!want) {
        return false;
    }

    for (int32_t hop = 0; model && hop < kMaxModelHops; ++hop, model = model->m_attachParent) {
        if (model == want) {
            return true;
        }
    }

    return false;
}

// Not in the original binary. FindTarget already walks the attach chain, so a rider is a target when its
// mount is, and vice versa -- which is what the sort needs: the whole chain has to land on the same side of
// the split or the mount would be drawn with the world and stamped over by its own rider.
bool CUnitOutline::IsTarget(CM2Model* model) {
    // Enabled() is checked here as well as at the stamp: the gather fills the list whether or not the option
    // is on, and this answer reorders the model pass, so without it a player who never turned the outline on
    // would still be drawing their world in a different order.
    if (!model || s_inComposite || !s_count || !Enabled()) {
        return false;
    }

    return FindTarget(model) >= 0;
}

// Not in the original binary.
bool CUnitOutline::HasTargets() {
    return !s_inComposite && s_count > 0 && Enabled();
}

// Not in the original binary. Called for every model batch the scene is about to draw.
void CUnitOutline::StampBatch(const CGxBatch& batch, CM2Model* model) {
    // The first guard is what keeps the module a pure pass-through while the option is off: no mask is
    // stepped into and no batch is drawn twice.
    if (!s_count || s_inComposite || !model || !s_colorPS || !s_cutoutPS || !s_mask || !s_playerMask) {
        return;
    }

    int32_t index = FindTarget(model);

    if (index >= 0) {
        StampSilhouette(batch, index);
        return;
    }

    // The player is not a target. Its exact silhouette is collected into its own mask so the edge pass can
    // subtract it, which makes the result order-independent: it does not matter whether the player draws
    // before or after the unit it covers.
    if (IsModel(model, s_playerModel)) {
        StampOccluder(batch);
    }
}

// ---------------------------------------------------------------------------------------------
// Resources
// ---------------------------------------------------------------------------------------------

bool CUnitOutline::EnsureResources() {
    if (!Enabled()) {
        return false;
    }

    if (g_theGxDevicePtr->DeviceApi() != GxApi_D3d9 && g_theGxDevicePtr->DeviceApi() != GxApi_D3d9Ex) {
        // The shaders are D3D9 bytecode and the masks are set up as that device's render targets. On any
        // other backend the module stays quiet rather than drawing a wrong-coloured quad.
        return false;
    }

    if (FFX::s_useRECT) {
        // The passes offset their samples in normalised texture coordinates. A rectangle texture samples
        // in pixels instead, so the offsets -- and with them the line width and the blur -- would be wrong
        // by a factor of the texture size. That only happens on the GL device with ffxRectangle on; the
        // module stands down there.
        return false;
    }

    if (!s_colorPS) {
        s_colorPS = CompilePixelShader(kColorHLSL);
        s_cutoutPS = CompilePixelShader(kCutoutColorHLSL);
        s_edgePS = CompilePixelShader(kEdgeHLSL);
        s_blurPS = CompilePixelShader(kBlurHLSL);
    }

    if (!s_colorPS || !s_cutoutPS || !s_edgePS || !s_blurPS) {
        return false;
    }

    const CRect& window = g_theGxDevicePtr->DeviceCurWindow();
    C2iVector size = { static_cast<int32_t>(window.maxX), static_cast<int32_t>(window.maxY) };

    if (size.x <= 0 || size.y <= 0) {
        return false;
    }

    // Back-buffer sized so the composite samples them 1:1 and the shaders' pixel distances hold. Built by
    // hand rather than FFX::CreateTex because they must not be rounded to a power of two: the scene's depth
    // surface is exactly this size and has to pair with them. A no-op unless the size actually changed.
    if (!CreateFrameTex(s_maskTex, size, false) || !CreateFrameTex(s_playerMaskTex, size, false) || !CreateFrameTex(s_blurTex, size, false)) {
        return false;
    }

    RefreshGxTexture(s_maskTex, s_mask);
    RefreshGxTexture(s_playerMaskTex, s_playerMask);
    RefreshGxTexture(s_blurTex, s_blur);

    return s_mask && s_playerMask && s_blur;
}

// Not in the original binary. Whether the scene's depth can be shared with the masks at all.
//
// D3D9 pairs a colour target and a depth surface only when both have the same dimensions and the same
// multisample count, and the engine builds every depth texture here with D3DMULTISAMPLE_NONE (see
// CGxDeviceD3d::ITexCreate). So a multisampled frame is the one case this cannot serve: the module keeps
// working, with the line simply drawn over whatever it covers. The depth format has to be one the device
// reports as a render target, and the masks are the frame's size by construction, so the pairing is
// symmetric.
bool CUnitOutline::OcclusionSupported() {
    if (!Enabled()) {
        return false;
    }

    if (g_theGxDevicePtr->DeviceApi() != GxApi_D3d9 && g_theGxDevicePtr->DeviceApi() != GxApi_D3d9Ex) {
        return false;
    }

    if (g_theGxDevicePtr->m_format.m_multisampleCount > 1) {
        return false;
    }

    // The masks have to be the frame's exact size to pair with its depth, and that needs the non-pow2 target:
    // GxTexCreate refuses a non-power-of-two size on GxTex_2d, so without this the masks are rounded and the
    // pairing would be between two different sizes. Standing the test down is the safe answer -- the line is
    // still drawn, it just is not clipped by the scene.
    if (g_theGxDevicePtr->Caps().m_texTarget[GxTex_NonPow2] != 1) {
        return false;
    }

    return g_theGxDevicePtr->Caps().m_rttFormat[GxTex_D24X8] != 0;
}

// Creates the frame-sized depth texture the 3D pass shares. A no-op once it exists at the right size, which
// is checked against the window rather than the masks, because the device sizes the viewport from the
// window: a mask and this texture then cover the same pixels even if the mask was rounded.
bool CUnitOutline::EnsureDepthTexture() {
    if (!OcclusionSupported()) {
        return false;
    }

    const CRect& window = g_theGxDevicePtr->DeviceCurWindow();
    C2iVector size = { static_cast<int32_t>(window.maxX), static_cast<int32_t>(window.maxY) };

    if (size.x <= 0 || size.y <= 0) {
        return false;
    }

    if (s_depthTexture && s_depthSize.x == size.x && s_depthSize.y == size.y) {
        return true;
    }

    if (s_depthTexture) {
        HandleClose(reinterpret_cast<HOBJECT>(s_depthTexture));
        s_depthTexture = nullptr;
        s_depth = nullptr;
    }

    // Same non-power-of-two target as the masks, and for the same reason: the depth has to be the frame's
    // exact size to pair with a colour target that size, and GxTexCreate rejects 1920x1200 on GxTex_2d.
    CGxTexFlags flags(GxTex_Linear, 0, 0, 0, 0, 1, 1, 0, 0, 0);
    s_depthTexture = TextureCreate(GxTex_NonPow2, size.x, size.y, 0, GxTex_D24X8, GxTex_D24X8, flags, nullptr, GxTexNullUserFunc, "UnitOutline::s_depthTexture", 0);

    if (!s_depthTexture) {
        s_depthSize.x = 0;
        s_depthSize.y = 0;
        return false;
    }

    s_depth = TextureGetGxTex(s_depthTexture, 1, nullptr);

    if (!s_depth) {
        HandleClose(reinterpret_cast<HOBJECT>(s_depthTexture));
        s_depthTexture = nullptr;
        s_depthSize.x = 0;
        s_depthSize.y = 0;
        return false;
    }

    s_depthSize = size;

    return s_depth != nullptr;
}

// Not in the original binary. Hands the 3D pass one depth buffer to share. Called around CWorld::Render, so
// terrain and world objects write into it before any unit is stamped, and the stamps that follow test
// against exactly the depth the frame has already accumulated.
void CUnitOutline::BeginDepthPass() {
    s_depthBound = false;
    s_depthBindMismatch = false;
    s_stampDepthTest = -1;

    // Deliberately not gated on s_count: the targets are picked out while the world draws, and the depth
    // has to be in place before the first batch reaches the stamp, which is well before the composite.
    if (!EnsureDepthTexture()) {
        return;
    }

    // Bind the depth beside whatever colour target is already current -- the back buffer, or FFX's own buffer
    // when the glow effect is running. The colour target is deliberately left untouched: re-binding it would be
    // a no-op when it already matches, and a null bind here would tear the glow buffer down. The viewport needs
    // no attention either, because binding a target re-applies the one the device is already holding.
    CGxTex* colorTarget = nullptr;
    g_theGxDevicePtr->DeviceGetRenderTarget(GxBuffers_Color, colorTarget);

    // D3D9 pairs a colour target and a depth surface only at equal dimensions, and a failed bind is silent --
    // the old depth would stay bound while the test was switched on, clipping the line against the wrong
    // frame. So anything but an exact match stands the depth test down for the frame rather than guessing.
    if (colorTarget && (static_cast<int32_t>(colorTarget->m_width) != s_depthSize.x || static_cast<int32_t>(colorTarget->m_height) != s_depthSize.y)) {
        return;
    }

    // Whatever the 3D pass was using is remembered so EndDepthPass can hand the same one back, which is the
    // device's own surface for the back buffer and something else under the shadow pass.
    s_savedDepth = nullptr;
    g_theGxDevicePtr->DeviceGetRenderTarget(GxBuffers_Depth, s_savedDepth);

    GxRenderTargetSet(GxBuffers_Depth, s_depth, 0);

    // The device does not check SetDepthStencilSurface's HRESULT, so a bind the driver refused is silent: the
    // previous depth would stay bound while this module believed the scene's was. A stamp testing against the
    // wrong surface then rejects the pixels it covers -- which erases the whole line rather than clipping it,
    // the failure this module was ported to avoid. So the bind is not trusted until the device reports the
    // surface as the one bound; when it disagrees the depth test is left off and the line is drawn unclipped.
    if (g_theGxDevicePtr->DeviceGetBoundDepthSurface() != g_theGxDevicePtr->DeviceGetTexSurface(s_depth)) {
        s_depthBindMismatch = true;

        // The rejected bind left the device on its old depth but gx's cache naming this texture, and that
        // cache early-outs on a match (see DeviceSetRenderTarget), so the disagreement would outlive the frame.
        // Naming the old one back puts both on the same surface again; the bind itself is a no-op.
        RestoreSavedDepth();
        return;
    }

    // The world clears the device's own depth, which is not this surface, so this one would otherwise start the
    // frame holding the previous frame's geometry. Cleared to 1.0 -- the far plane -- so whatever the world does
    // not draw reads as open space and no unit is clipped by a stale occluder.
    g_theGxDevicePtr->SceneClear(GxBufferBit_Depth, CImVector(0, 0, 0, 0));

    s_depthBound = true;
}

// Not in the original binary. See the declaration: the one safe way to hand the depth back, because asking
// for a null depth while a colour *texture* is bound dereferences that texture without checking it.
void CUnitOutline::RestoreSavedDepth() {
    if (s_savedDepth) {
        GxRenderTargetSet(GxBuffers_Depth, s_savedDepth, 0);
        return;
    }

    CGxTex* colorTarget = nullptr;
    g_theGxDevicePtr->DeviceGetRenderTarget(GxBuffers_Color, colorTarget);

    if (colorTarget) {
        // FFX is drawing into its own buffer, and its depth is one the device derives from that buffer. The
        // colour binding is cleared first, the depth is dropped while the back buffer is current -- the one
        // case that is safe -- and the colour texture is bound again, which is what makes the device build the
        // depth that pairs with it.
        GxRenderTargetSet(GxBuffers_Color, nullptr, 0);
        GxRenderTargetSet(GxBuffers_Depth, nullptr, 0);
        GxRenderTargetSet(GxBuffers_Color, colorTarget, 0);
        return;
    }

    GxRenderTargetSet(GxBuffers_Depth, nullptr, 0);
}

// Not in the original binary. Detaches the shared depth and restores the device's own, so the composite and
// everything after the world sees the same state it did before.
void CUnitOutline::EndDepthPass() {
    if (!s_depthBound) {
        return;
    }

    s_depthBound = false;

    // The depth the 3D pass had before this module is put back by name when there was one -- the shadow pass
    // binds its own -- and there is nothing else to do: the colour target is left as it is, which is what keeps
    // FFX's glow buffer intact.
    RestoreSavedDepth();
}

// ---------------------------------------------------------------------------------------------
// The mask
// ---------------------------------------------------------------------------------------------

// Draws one of the model's own batches into a mask, under this module's state and colour. A pixel shader
// is fixed for the span of a draw, so the batch is re-drawn with the module's shader rather than tinted in
// place; the model's vertex data and its index range are untouched and the caller draws the batch for real
// immediately afterwards, which is why the vertex stream and the pixel constants are left alone.
void CUnitOutline::StampMask(const CGxBatch& batch, const float color[4], CGxTex* target, bool& cleared) {
    if (!target) {
        return;
    }

    // The blend state says what the batch is. An opaque batch is filled flat. An alpha-tested one follows
    // its real cutout, and keeps the test in the stamp so wings and hair cards do not become solid slabs.
    // Anything else -- additive effects, attachments, glows -- is left out: those broad billboard quads
    // would otherwise be outlined as if they were the unit's own mesh.
    int32_t blend = g_theGxDevicePtr->m_appRenderStates[GxRs_BlendingMode].m_value.m_data.i[0];

    if (blend != GxBlend_Opaque && blend != GxBlend_Alpha) {
        return;
    }

    bool cutout = blend == GxBlend_Alpha && g_theGxDevicePtr->m_appRenderStates[GxRs_AlphaRef].m_value.m_data.i[0] > 0;

    // The world may be drawing through a scaled or offset viewport (FFX::Start shrinks it when the glow
    // buffer is smaller than the screen). The mask has to end up in the pixel space the composite later
    // samples it in, so the stamp runs at a plain 1:1 viewport and the world's own is put back after.
    float oldMinX;
    float oldMaxX;
    float oldMinY;
    float oldMaxY;
    float oldMinZ;
    float oldMaxZ;

    GxXformViewport(oldMinX, oldMaxX, oldMinY, oldMaxY, oldMinZ, oldMaxZ);

    // RsPush/RsPop cover render states only: the bound render target lives outside that stack, so the
    // caller's own target has to be put back by hand or the caller's real draw -- and every batch after it
    // -- would land in this mask instead of the scene.
    CGxTex* oldTarget = nullptr;
    g_theGxDevicePtr->DeviceGetRenderTarget(GxBuffers_Color, oldTarget);

    g_theGxDevicePtr->RsPush();

    GxRenderTargetSet(GxBuffers_Color, target, 0);
    // The stamp has to compare against the scene's depth, and the viewport's z range is what turns a clip-space
    // z into a stored one. The world draws through [minZ, 0.94] -- see CGWorldFrame::OnWorldRender and
    // CWorld::s_unk_ADEEE4 -- so forcing [0, 1] here would store every unit about six per cent deeper than it
    // is and let geometry at its own distance occlude it. The saved z range is carried over instead. X and Y
    // stay at 1:1 because the mask is sampled in whole-target pixel space by the composite.
    GxXformSetViewport(0.0f, 1.0f, 0.0f, 1.0f, oldMinZ, oldMaxZ);

    if (!cleared) {
        g_theGxDevicePtr->SceneClear(GxBufferBit_Color, CImVector(0, 0, 0, 0));
        cleared = true;
    }

    // Unconditionally opaque: the mask holds one silhouette per pixel, so a batch overwrites what is there
    // rather than blending with it. The cutout shader does its clipping itself.
    GxRsSet(GxRs_BlendingMode, GxBlend_Opaque);
    GxRsSet(GxRs_Lighting, 0);
    GxRsSet(GxRs_Fog, 0);
    // Depth tested against the scene when BeginDepthPass supplied its depth texture: the unit is transformed
    // by the view and projection the world just used, so a wall that is nearer rejects the pixels it covers
    // and the line ends at its edge instead of switching the whole outline off. Writes stay off -- the mask
    // must not add the unit to the depth the later batches of the same unit will test against.
    GxRsSet(GxRs_DepthTest, s_depthBound ? 1 : 0);
    s_stampDepthTest = s_depthBound ? 1 : 0;
    GxRsSet(GxRs_DepthFunc, 0);
    GxRsSet(GxRs_DepthWrite, 0);
    // Nothing here touches the vertex shader or the vertex stream: the stamp has to redraw the same
    // geometry the caller is about to. With the shader system on, that geometry is skinned by the vertex
    // shader the caller just set up, so it stays bound and this only replaces the pixel stage. With the
    // shader system off, the stream already holds transformed vertices and the fixed-function pipeline
    // carries them in the same way. The texture at stage 0 is likewise left alone: the cutout shader
    // samples the model's own diffuse from it, and the plain fill ignores it.
    GxRsSet(GxRs_ColorWrite, 15);
    GxRsSet(GxRs_PixelShader, cutout ? s_cutoutPS : s_colorPS);
    GxShaderConstantsSet(GxSh_Pixel, 0, color, 1);

    g_theGxDevicePtr->Draw(const_cast<CGxBatch*>(&batch), 1);

    g_theGxDevicePtr->RsPop();
    GxRenderTargetSet(GxBuffers_Color, oldTarget, 0);
    GxXformSetViewport(oldMinX, oldMaxX, oldMinY, oldMaxY, oldMinZ, oldMaxZ);
}

// Not in the original binary. The fade: full strength when the entry appears, eased to kFadeEndOpacity
// over kFadeMs and then held there. The edge shader recovers the line colour as rgb/alpha, so scaling the
// premultiplied colour and its weight together keeps the hue exact while the line fades.
void CUnitOutline::StampSilhouette(const CGxBatch& batch, int32_t index) {
    float color[4];
    memcpy(color, s_targets[index].m_color, sizeof(color));

    if (!s_targets[index].m_keepFull) {
        uint32_t age = OsGetAsyncTimeMs() - s_targets[index].m_birth;
        float t = age >= kFadeMs ? 1.0f : static_cast<float>(age) / static_cast<float>(kFadeMs);
        float weight = 1.0f - (1.0f - kFadeEndOpacity) * t;

        color[0] *= weight;
        color[1] *= weight;
        color[2] *= weight;
        color[3] *= weight;
    }

    StampMask(batch, color, s_mask, s_maskCleared);
}

void CUnitOutline::StampOccluder(const CGxBatch& batch) {
    const float white[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    StampMask(batch, white, s_playerMask, s_playerMaskCleared);
}

// ---------------------------------------------------------------------------------------------
// The composite
// ---------------------------------------------------------------------------------------------

// Softens the mask in place: the result is left in the mask, and the next frame's first stamp clears it
// again. Two ping-pong passes so one scratch target covers both axes -- the mask is blurred into the
// scratch, then the scratch is blurred back. Called with FFX::SetupGx already in effect, which is what
// supplies the pixel-space projection these quads are built in.
//
// Silhouettes arrive from the stamp as hard one-pixel shapes, so this pass is what gives the edge detect a
// gradient to work from: a spike or a shoulder becomes a short ramp, not a step.
void CUnitOutline::BlurPass() {
    if (kSoftness <= 0.0f) {
        return;
    }

    // The offsets are in the masks' own texture coordinates, so a radius in screen pixels is one texel
    // step times the texture size the device actually gave us.
    float stepX = kSoftness * kSoftnessScale * s_maskTex.m_texelSize.x;
    float stepY = kSoftness * kSoftnessScale * s_maskTex.m_texelSize.y;

    GxRsSet(GxRs_PixelShader, s_blurPS);
    GxRsSet(GxRs_BlendingMode, GxBlend_Opaque);
    // Inherited state the world's last draw left behind would clip or discard this full-screen quad, so
    // only part of the target is written: the alpha test runs on a pixel shader's OUTPUT, so a stale alpha
    // reference throws away the low-alpha taps of the blur. A partial write leaves the scratch holding the
    // previous frame's silhouette, which the second axis then blurs back into the mask beside the current
    // pose.
    GxRsSet(GxRs_AlphaRef, 0);

    // The scratch is cleared before the ping-pong, so neither axis depends on it having been written in
    // full.
    GxRenderTargetSet(GxBuffers_Color, s_blur, 0);
    g_theGxDevicePtr->SceneClear(GxBufferBit_Color, CImVector(0, 0, 0, 0));

    for (int32_t axis = 0; axis < 2; axis++) {
        float direction[4] = { axis ? 0.0f : stepX, axis ? stepY : 0.0f, 0.0f, 0.0f };

        GxShaderConstantsSet(GxSh_Pixel, 0, direction, 1);
        // axis 0 reads the mask and writes the scratch; axis 1 swaps, leaving the blurred result back in
        // the mask.
        GxRsSet(GxRs_Texture0, axis ? s_blur : s_mask);
        GxRenderTargetSet(GxBuffers_Color, axis ? s_mask : s_blur, 0);
        DrawFullscreenQuad(axis ? s_maskTex : s_blurTex);
    }
}

// The edge detect: the mask's rim into the frame, in the colour the mask carries, with the player's own
// silhouette taken out of it.
void CUnitOutline::EdgePass() {
    float c0[4] = { s_maskTex.m_texelSize.x, s_maskTex.m_texelSize.y, kThickness, kIntensity };
    float c1[4] = { kOpacity, kThreshold, s_playerMaskCleared ? 1.0f : 0.0f, 0.0f };

    GxRsSet(GxRs_PixelShader, s_edgePS);
    // The line is blended over the frame it was made from.
    GxRsSet(GxRs_BlendingMode, GxBlend_Alpha);
    GxRsSet(GxRs_AlphaRef, 0);
    GxRsSet(GxRs_Texture0, s_mask);
    GxRsSet(GxRs_Texture1, s_playerMask);

    GxShaderConstantsSet(GxSh_Pixel, 0, c0, 1);
    GxShaderConstantsSet(GxSh_Pixel, 1, c1, 1);

    DrawFullscreenQuad(s_maskTex);
}

// Not in the original binary. The world -> interface boundary. The line goes under the interface, so the
// HUD, the nameplates and the menus stay on top of it. Drawn while the world's own render target is still
// the one bound, because the mask is in that target's pixel space.
// TEMP diagnostic. One line a second to Logs\unitoutline.log while the option is on, so a silent early-out
// can be told apart from a pipeline that runs and draws nothing. Delete with the investigation.
void OutlineDiag(const char* tag) {
    static uint32_t s_last;
    uint32_t now = OsGetAsyncTimeMs();

    if (now - s_last < 1000) {
        return;
    }

    s_last = now;

    FILE* file = fopen("Logs\\unitoutline.log", "a");

    if (!file) {
        return;
    }

    fprintf(file,
            "%s api=%d ms=%u rtt24=%d ps=%d/%d/%d/%d on=%d gx=%d/%d/%d tex=%dx%d/%dx%d/%dx%d "
            "targets=%d maskCleared=%d depthBound=%d depthSize=%dx%d mismatch=%d stampDepth=%d\n",
            tag,
            static_cast<int>(g_theGxDevicePtr->DeviceApi()),
            g_theGxDevicePtr->m_format.m_multisampleCount,
            g_theGxDevicePtr->Caps().m_rttFormat[GxTex_D24X8],
            s_colorPS != nullptr, s_cutoutPS != nullptr, s_edgePS != nullptr, s_blurPS != nullptr,
            (s_cvarUnitOutline && s_cvarUnitOutline->m_intValue) ? 1 : 0,
            s_mask != nullptr, s_playerMask != nullptr, s_blur != nullptr,
            s_maskTex.m_size.x, s_maskTex.m_size.y, s_maskTex.m_texSize.x, s_maskTex.m_texSize.y,
            s_blurTex.m_size.x, s_blurTex.m_size.y,
            s_count, s_maskCleared ? 1 : 0, s_depthBound ? 1 : 0,
            s_depthSize.x, s_depthSize.y,
            s_depthBindMismatch ? 1 : 0, s_stampDepthTest);

    fclose(file);
}

void CUnitOutline::Composite() {
    OutlineDiag("composite");

    if (s_inComposite) {
        return;
    }

    // This frame's list becomes the reference for the next one's fade clocks, whatever happens below.
    memcpy(s_prevTargets, s_targets, sizeof(s_prevTargets));
    s_prevCount = s_count;

    if (!EnsureResources()) {
        BeginTargets();
        return;
    }

    if (!s_count && !s_maskCleared && !s_playerMaskCleared) {
        // Nothing was recorded last frame and neither mask was written: there is nothing to composite and
        // no reason to spend the passes. The list still drains, so the next frame starts clean.
        BeginTargets();
        return;
    }

    if (!s_maskCleared) {
        // A selected unit that was culled, off screen or occluded this frame stamped nothing, so the mask
        // still holds the silhouette from the last frame it was drawn in. Compositing that would leave a
        // line behind at the old screen position, softening a little every frame, so it is dropped instead.
        s_playerMaskCleared = false;
        BeginTargets();
        return;
    }

    s_inComposite = true;

    // Wherever the world just drew to -- the back buffer, or FFX's own buffer if the glow effect is
    // running -- is where the line belongs, so the frame's own target is saved and put back.
    CGxTex* frameTarget = nullptr;
    g_theGxDevicePtr->DeviceGetRenderTarget(GxBuffers_Color, frameTarget);

    // The engine's own full-screen setup: pixel-space ortho projection, identity view and world, the mask
    // as the colour target, and the render-state push that covers everything set from here on.
    FFX::SetupGx(&s_maskTex);

    // The world was drawing through a sub-viewport (OnWorldRender narrows it to the 3D rect, and FFX::Start
    // shrinks it further when the glow buffer is smaller than the screen). The quad is built for the whole
    // target, so the viewport is opened up to match; OnWorldRender puts the world's own back after this
    // returns.
    GxXformSetViewport(0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 1.0f);

    BlurPass();

    GxRenderTargetSet(GxBuffers_Color, frameTarget, 0);
    EdgePass();

    GxRsSet(GxRs_Texture0, nullptr);
    GxRsSet(GxRs_Texture1, nullptr);

    FFX::SetupReset();

    if (frameTarget) {
        // SetupReset puts the default back buffer back; the world was drawing somewhere else.
        GxRenderTargetSet(GxBuffers_Color, frameTarget, 0);
    }

    s_inComposite = false;

    BeginTargets();
    s_maskCleared = false;
    s_playerMaskCleared = false;
}
