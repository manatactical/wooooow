// Screen-space ambient occlusion for the world pass.
//
// Reading the depth. Direct3D 9 gives no way to read the window's own depth-stencil surface: it belongs to
// the swap chain, is not a texture, and cannot be bound to a texture unit -- and the engine's
// DeviceResolveDepthBuffer is dead code, nothing calls it. So this pass owns a D24X8 texture and binds it as
// the device's depth target for the duration of the world draw (FFX::Start() .. FFX::Finish(), the same
// bracket the engine already uses for its full-screen effects). The scene tests depth against that surface
// exactly as it did against the window's, and afterwards the occlusion pass can sample it.
//
// Multisampling. A depth-stencil surface cannot be paired with a multisampled colour target, so the depth
// has to be single-sampled, which is also what the occlusion pass wants. With gxMultisample > 1 the effect
// therefore does nothing and says so once in gx.log.
//
// Where the result goes. The occlusion is multiplied straight into the backbuffer with GxBlend_Mod, exactly
// the way the engine's own post-process passes composite: GxRenderTargetSet(GxBuffers_Color, nullptr, 0)
// makes the backbuffer the target (CGxDeviceD3d::DeviceSetRenderTarget falls back to m_defColorSurface), so
// no copy of the frame is needed and the pass composes with the engine's own effect whichever draws last.
//
// Recovering view Z. The depth in that texture was written with the scene's projection, and the shader
// reconstructs the eye distance from the two coefficients it can rely on: GxuXformCreateProjection_Exact
// (Gxu.cpp) builds zClip = c2*z + d2 with w = z, so z = d2/(depth - c2). Both are set from
// CGxDevice::m_projection below; the shader warns it off a frame whose c2 is not the perspective one.

// Why not Data\custom.MPQ's own Custom\Shaders\Pixel\ps_4_0\PostAO.bls: it is compiled Shader Model 4/5
// bytecode (HSXG version 0x10003, shader model field 2) and this client is D3D9, capped at ps_3_0 -- the .bls
// loader in CGxDevice::IShaderLoad only understands models 1-3 and there is no ps_3_0 PostAO in the archive.
// So the pass below carries its own ps_3_0 bytecode (SsaoShaders.h) instead of loading that file.

#include <cstdint>
#include <cstring>
#include <cstdio>
#include "Engine/Source/Base/Handle.h"
#include "Engine/Source/ConsoleBase/ConsoleVar.h"
#include "Engine/Source/ConsoleBase/Types.h"
#include "Engine/Source/FFX/FFX.h"
#include "Engine/Source/FFX/SsaoShaders.h"
#include "Engine/Source/Gx/CGxBuf.h"
#include "Engine/Source/Gx/CGxDevice/CGxDevice.h"
#include "Engine/Source/Gx/CGxDevice/CGxShader.h"
#include "Engine/Source/Gx/Gx.h"
#include "Engine/Source/Os/OsFile.h"
#include "Engine/Source/Services/Texture.h"
#include "Engine/Source/Tempest/CRect.h"

namespace FFX {

// A note line during setup. Kept cheap enough to call from the frame path: the file is opened once and every
// write advances the handle's own position, so there is no seek and no re-read of the log. (The first version
// read to end-of-file on every call, which cost hundreds of syscalls per frame and visibly halved the frame
// rate; anything per-frame has to be one write.)
//
// s_traceFrames gates the per-frame notes: setup notes always go out, the draw-by-draw ones only for the next
// few frames after something changed. A countdown of *frames*, not of notes: one world frame writes three or
// four notes before the interesting one, so a note budget of 3 was spent before "step1 ao drawn" and every run
// went blind exactly where the pass stops. SsaoEnd() resets it when a frame completes.
static HOSFILE s_traceFile;
static bool s_traceOpen;
static int32_t s_traceFrames;
static bool s_traceFrameUsed;

static void SsaoTraceV(const char* format, va_list args) {
    char line[512];
    _vsnprintf(line, sizeof(line), format, args);

    if (!s_traceOpen) {
        s_traceOpen = true;
        // OS_CREATE_ALWAYS truncates, so the position starts at 0 and each later write appends.
        s_traceFile = OsCreateFile("Logs\\ssao-trace.log", OS_GENERIC_WRITE, 1, OS_CREATE_ALWAYS, OS_FILE_ATTRIBUTE_NORMAL, OS_FILE_TYPE_ANY);
    }

    if (s_traceFile == OS_INVALID_FILE) {
        return;
    }

    uint32_t length = static_cast<uint32_t>(strlen(line));
    line[length] = '\r';
    line[length + 1] = '\n';

    uint32_t written = 0;
    OsWriteFile(s_traceFile, line, length + 2, &written);
}

void SsaoTrace(const char* format, ...) {
    va_list args;
    va_start(args, format);
    SsaoTraceV(format, args);
    va_end(args);
}

// Per-frame notes, for the frames right after a setup change only. The countdown drains here, as the notes are
// actually written, rather than once per SsaoBegin call: a frame that bails out early (glue screen, MSAA on)
// writes nothing, and draining on those used to eat the whole budget before a real world frame was reached, so
// the interesting frame logged nothing at all.
// Per-frame notes, for the frames right after a setup change only. The countdown drains once per frame, on the
// first note that frame writes, so a frame that bails out early costs one frame of budget rather than one note,
// and a full world frame can write every note it has. SsaoEnd() ends the frame.
static void SsaoTraceFrame(const char* format, ...) {
    if (s_traceFrames <= 0) {
        return;
    }

    if (!s_traceFrameUsed) {
        s_traceFrameUsed = true;
        s_traceFrames--;
    }

    va_list args;
    va_start(args, format);
    SsaoTraceV(format, args);
    va_end(args);
}

static CVar* s_cvarSsao;
static CVar* s_cvarSsaoRadius;
static CVar* s_cvarSsaoIntensity;
static CVar* s_cvarSsaoDebug;

static CGxShader* s_aoPs;
static CGxShader* s_combinePs;
static CGxShader* s_probePs;

static Tex s_depth;  // the scene's depth while the world renders
static Tex s_ao;     // the occlusion term
static Tex s_probe;  // occlusion replaced by the reconstructed view depth, for checking the reconstruction

static bool s_ready;
// Separate one-shot flags per reason. A single shared flag meant the startup glue screen (an orthographic frame)
// claimed it and then suppressed the "active" line for the whole session, which is exactly the line needed to
// tell a running effect from a dead one.
static bool s_loggedOrtho;
static bool s_loggedNoShader;
static bool s_loggedActive;
static bool s_loggedMultisample;
static bool s_armed;  // depth is bound, SsaoEnd() owes a pass

// The world-pass projection, captured in SsaoBegin(). FFX::Finish() runs after the UI has already
// overwritten m_projNative with its orthographic matrix, so reading it there decodes s_depth with the
// wrong matrix and the pass bails out as "not a perspective frame".
static C44Matrix s_frameProj;

// VERIFY: (new) manual
static CGxShader* CreatePixelShader(const uint8_t* code, uint32_t size, const char* name) {
    CGxShader* shader = new (__FILE__, __LINE__) CGxShader();
    shader->m_target = GxSh_Pixel;
    shader->m_code.SetCount(size);
    memcpy(shader->m_code.m_data, code, size);
    shader->m_loaded = 0;
    shader->m_valid = 0;
    shader->unk_0034 = 0;
    shader->m_refCount = 1;

    if (!shader->Valid()) {
        GxLog("SSAO: pixel shader '%s' did not compile on this device", name);
    }

    return shader;
}

// VERIFY: (new) manual
// A device reset (resolution change, fullscreen toggle) drops every D3D resource this pass owns. The sizes
// have to be cleared with the pointers: FFX::CreateTex only creates the texture when the requested size differs
// from tex.m_texSize, so leaving the old size behind makes it skip creation and hand back a null texture.
static void __fastcall OnDeviceRestored() {
    s_depth.m_texture = nullptr;
    s_depth.m_size.x = 0;
    s_depth.m_size.y = 0;
    s_depth.m_texSize.x = 0;
    s_depth.m_texSize.y = 0;
    s_ao.m_texture = nullptr;
    s_ao.m_size.x = 0;
    s_ao.m_size.y = 0;
    s_ao.m_texSize.x = 0;
    s_ao.m_texSize.y = 0;
    s_probe.m_texture = nullptr;
    s_probe.m_size.x = 0;
    s_probe.m_size.y = 0;
    s_probe.m_texSize.x = 0;
    s_probe.m_texSize.y = 0;
    s_aoPs = nullptr;
    s_combinePs = nullptr;
    s_probePs = nullptr;
    s_loggedOrtho = false;
    s_loggedNoShader = false;
    s_loggedActive = false;
    s_loggedMultisample = false;
    s_armed = false;
    s_traceFrames = 3;
}

// VERIFY: (new) manual
// Logged so a toggle from the Video Options panel leaves a line in the trace even when the effect then
// bails out further down (no depth texture, non-perspective frame).
//
// Turning the effect on also drops gxMultisample to 1. D3D9 cannot bind a single-sampled depth surface while a
// multisampled colour target is active, and a multisampled depth surface cannot be read as a texture, so the
// two cannot coexist on this API. Rather than leave the checkbox doing nothing, the conflict is resolved in
// favour of the effect and said out loud; the setting applies on the next start, which is what the panel's
// "requires restart" flag is for.
static bool SsaoCVarCallback(CVar* var, const char* oldValue, const char* value, void* arg) {
    SsaoTrace("ambientOcclusion changed '%s' -> '%s'", oldValue ? oldValue : "", value ? value : "");
    s_traceFrames = 3;

    if (SStrToInt(value) && s_cvarMultisample && s_cvarMultisample->m_intValue > 1) {
        GxLog("SSAO: on, dropping gxMultisample %d -> 1 (they cannot both be active on D3D9), restart required", s_cvarMultisample->m_intValue);
        SsaoTrace("ambientOcclusion: gxMultisample %d -> 1, restart required", s_cvarMultisample->m_intValue);
        s_cvarMultisample->Set(1, true, false, false, true);
        s_loggedMultisample = false;  // the previous "it needs gxMultisample 1" line is now settled
    }

    return true;
}


// VERIFY: (new) manual
void SsaoSetup() {
    // The CVars are registered even when the device is not up yet: the video options panel reads them, and an
    // unregistered name there is a Lua error that stops the whole panel loading. Registration and arming are
    // separate -- s_ready only guards the render path.
    if (!s_cvarSsao) {
        // "ambientOcclusion" is the name the Video Options panel uses (Interface\FrameXML\VideoOptionsPanels.xml,
        // the Advanced Effects checkbox), so the checkbox drives the effect without any Lua. Flag 1 and a
        // callback match "ffx" and "gxVSync", the two graphics CVars SetCVar already accepts. Default 0: the
        // effect waits for the checkbox, so nothing changes on a fresh install.
        s_cvarSsao = CVar::Register("ambientOcclusion", "ambient occlusion (screen-space)", 1, "0", SsaoCVarCallback, CATEGORY_GRAPHICS, false, nullptr, false);
        s_cvarSsaoRadius = CVar::Register("ambientOcclusionRadius", "ambient occlusion sampling radius, in world units (1 is about a yard)", 3, "1", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
        s_cvarSsaoIntensity = CVar::Register("ambientOcclusionIntensity", "ambient occlusion strength (0.5 subtle, 1 normal, 2 strong)", 3, "1", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
        // Diagnostic: shows the occlusion term as a band of stripes so a mis-scaled reconstruction is visible
        // at a glance instead of only as a darker screen.
        s_cvarSsaoDebug = CVar::Register("ambientOcclusionDebug", "ambient occlusion debug view: 1 raw occlusion with stripes over the frame, 2 bypass, 3 recon steps", 1, "0", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
    }

    if (s_ready || !g_theGxDevicePtr) {
        return;
    }

    s_depth.m_texture = nullptr;
    s_ao.m_texture = nullptr;
    s_probe.m_texture = nullptr;

    g_theGxDevicePtr->AddDeviceRestoredCallback(OnDeviceRestored);

    s_ready = true;
    // Setup notes always go out; the draw-by-draw ones only for the frames right after something changed.
    s_traceFrames = 3;
    SsaoTrace("SsaoSetup: ready, ambientOcclusion default %s, multisample %d", s_cvarSsao->m_stringValue.m_str, s_cvarMultisample ? s_cvarMultisample->m_intValue : -1);
}


// VERIFY: (new) manual
bool SsaoActive() {
    // Registered at FFX::Init, which runs after the video options panel is built, so look it up lazily.
    if (!s_cvarSsao) {
        s_cvarSsao = CVar::Lookup("ambientOcclusion");
    }

    return s_ready && s_cvarSsao && s_cvarSsao->m_intValue;
}

// VERIFY: (new) manual
// True once the depth and occlusion targets exist for the current window size. Logs the first reason the
// effect cannot run, so a quiet gx.log means success rather than a silent no-op.
static bool EnsureTargets() {
    CRect window = { 0.0f, 0.0f, 0.0f, 0.0f };
    g_theGxDevicePtr->CapsWindowSize(window);

    int32_t width = static_cast<int32_t>(window.maxX - window.minX);
    int32_t height = static_cast<int32_t>(window.maxY - window.minY);

    if (width < 8 || height < 8) {
        SsaoTraceFrame("EnsureTargets: window too small (%dx%d)", width, height);
        return false;
    }

    // All three, not just depth. The occlusion and probe targets are allocated through FFX::CreateTex, which
    // skips creation when the size already matches, so one failed recreation used to leave them null while depth
    // stayed valid -- then every later frame took this early-out and the combine pass multiplied the frame by a
    // null texture, i.e. a black screen. Depth being ready says nothing about the other two.
    if (s_depth.m_texture && s_ao.m_texture && s_probe.m_texture &&
        s_depth.m_size.x == width && s_depth.m_size.y == height) {
        return true;
    }

    SsaoTrace("EnsureTargets: (re)creating %dx%d targets", width, height);
    s_traceFrames = 3;

    if (s_depth.m_texture) {
        HandleClose(reinterpret_cast<HOBJECT>(s_depth.m_texture));
        s_depth.m_texture = nullptr;
        s_depth.m_size.x = 0;
        s_depth.m_size.y = 0;
        s_depth.m_texSize.x = 0;
        s_depth.m_texSize.y = 0;
    }

    if (s_ao.m_texture) {
        HandleClose(reinterpret_cast<HOBJECT>(s_ao.m_texture));
        s_ao.m_texture = nullptr;
        s_ao.m_size.x = 0;
        s_ao.m_size.y = 0;
        s_ao.m_texSize.x = 0;
        s_ao.m_texSize.y = 0;
    }

    if (s_probe.m_texture) {
        HandleClose(reinterpret_cast<HOBJECT>(s_probe.m_texture));
        s_probe.m_texture = nullptr;
        s_probe.m_size.x = 0;
        s_probe.m_size.y = 0;
        s_probe.m_texSize.x = 0;
        s_probe.m_texSize.y = 0;
    }

    C2iVector size;
    size.x = width;
    size.y = height;

    // GxTexCreate rejects a non-power-of-two size for the plain 2d target, and TextureAllocGxTex ignores that
    // failure: the CTexture handle still comes back, but it carries no CGxTex, so the depth target is silently
    // unusable at 1920x1200. Use the same rule the engine's own FFX::CreateTex uses -- the rectangle/non-pow2
    // target when the device has one, otherwise allocate the next power of two and record both sizes so the
    // quad mapping (which uses m_texSize) still lands on the used corner.
    EGxTexTarget target = GetTexTarget();
    int32_t allocWidth = width;
    int32_t allocHeight = height;

    if (target == GxTex_2d) {
        for (allocHeight = 1; height > allocHeight; allocHeight *= 2) {
        }

        for (allocWidth = 1; width > allocWidth; allocWidth *= 2) {
        }
    }

    // TextureCreate's D24X8 path passes D3DUSAGE_DEPTHSTENCIL, which is what lets the device bind this as
    // the depth-stencil target as well as sample it afterwards.
    CGxTexFlags depthFlags(GxTex_Nearest, 0, 0, 0, 0, 1, 1, 0, 0, 0);
    s_depth.m_texture = TextureCreate(target, allocWidth, allocHeight, 0, GxTex_D24X8, GxTex_D24X8, depthFlags, nullptr, GxTexNullUserFunc, "FFX::SsaoDepth", 0);
    s_depth.m_size = size;
    s_depth.m_texSize.x = allocWidth;
    s_depth.m_texSize.y = allocHeight;
    s_depth.m_texelSize.x = 1.0f / allocWidth;
    s_depth.m_texelSize.y = 1.0f / allocHeight;

    CreateTex(s_ao, size, false, true);
    CreateTex(s_probe, size, false, true);

    if (!s_depth.m_texture || !s_ao.m_texture || !s_probe.m_texture) {
        SsaoTraceFrame("EnsureTargets: a texture came back null (depth %p ao %p probe %p)",
              static_cast<void*>(s_depth.m_texture), static_cast<void*>(s_ao.m_texture), static_cast<void*>(s_probe.m_texture));
        return false;
    }

    s_loggedOrtho = false;
    s_loggedNoShader = false;
    s_loggedActive = false;

    GxLog("SSAO: targets %dx%d (alloc %dx%d, target %d)", width, height, s_depth.m_texSize.x, s_depth.m_texSize.y, static_cast<int32_t>(target));
    SsaoTrace("EnsureTargets: ok %dx%d depth %p ao %p", width, height, static_cast<void*>(s_depth.m_texture), static_cast<void*>(s_ao.m_texture));

    return true;
}

// VERIFY: (new) manual
// Called at the end of FFX::Start(), i.e. just before the world draws. Binds this pass's depth texture as
// the device's depth target; SsaoEnd() hands the window's own surface back.
void SsaoBegin() {
    s_armed = false;
    s_traceFrameUsed = false;

    if (!SsaoActive()) {
        return;
    }

    // Single-sampled depth cannot pair with a multisampled colour target, so the override is impossible
    // with MSAA on. Say it once, with the CVar's current value, then stay quiet.
    if (s_cvarMultisample && s_cvarMultisample->m_intValue > 1) {
        if (!s_loggedMultisample) {
            s_loggedMultisample = true;
            GxLog("SSAO: off, it needs gxMultisample 1 (currently %d); set gxMultisample 1 and restart",
                  s_cvarMultisample->m_intValue);
        }

        return;
    }

    if (!s_cvarSsaoRadius) {
        s_cvarSsaoRadius = CVar::Lookup("ambientOcclusionRadius");
    }

    if (!s_cvarSsaoIntensity) {
        s_cvarSsaoIntensity = CVar::Lookup("ambientOcclusionIntensity");
    }

    if (!s_cvarSsaoDebug) {
        s_cvarSsaoDebug = CVar::Lookup("ambientOcclusionDebug");
    }

    if (!EnsureTargets()) {
        return;
    }

    CGxTex* depth = TextureGetGxTex(s_depth.m_texture, 1, nullptr);

    if (!depth) {
        SsaoTraceFrame("SsaoBegin: no depth gx tex (target %d, alloc %dx%d)", static_cast<int32_t>(GetTexTarget()), s_depth.m_texSize.x, s_depth.m_texSize.y);
        return;
    }

    // DeviceSetRenderTarget creates the surface and clears m_needsCreation, so bind first and judge afterwards:
    // the CGxTex from TexCreate always starts out needing creation, and nothing else touches this one.
    g_theGxDevicePtr->DeviceSetRenderTarget(GxBuffers_Depth, depth, 0);

    if (depth->m_needsCreation) {
        SsaoTraceFrame("SsaoBegin: depth texture did not create (target %d, alloc %dx%d)", static_cast<int32_t>(GetTexTarget()), s_depth.m_texSize.x, s_depth.m_texSize.y);
        return;
    }

    s_frameProj = g_theGxDevicePtr->m_projNative;
    s_armed = true;
    SsaoTraceFrame("SsaoBegin: armed, depth target bound (c2 %g d2 %g)", s_frameProj.c2, s_frameProj.d2);
}

// VERIFY: (new) manual
// Called at the start of FFX::Finish(), with the finished frame still on the backbuffer.
void SsaoEnd() {
    if (!s_armed) {
        return;
    }

    s_armed = false;

    // Was our depth still the bound one when the world finished? The scene render binds depth targets of its own
    // (the shadow query does exactly that, then restores), so a lost binding would leave s_depth never written:
    // a flat 1.0 buffer makes every kernel tap read zs > z0, occl stays 0, ao stays 1.0, and multiplying the frame
    // by 1.0 is invisible. That is indistinguishable from a dead pass on screen, so name the surface that is
    // actually bound before handing it back.
    CGxTex* boundDepth = nullptr;
    g_theGxDevicePtr->DeviceGetRenderTarget(GxBuffers_Depth, boundDepth);
    SsaoTraceFrame("SsaoEnd: depth bound %p, ours %p", static_cast<void*>(boundDepth), static_cast<void*>(TextureGetGxTex(s_depth.m_texture, 1, nullptr)));

    // Hand the window's own depth surface back before anything else draws; the UI and the next frame's
    // world pass both expect the real one.
    g_theGxDevicePtr->DeviceSetRenderTarget(GxBuffers_Depth, nullptr, 0);

    // The projection captured at SsaoBegin, i.e. the world pass's own: D3DTS_PROJECTION is set from
    // m_projNative (CGxDeviceD3d::XformSetProjection), so that is the matrix the depth buffer in s_depth was
    // written with. Reading m_projNative here instead would pick up the UI's orthographic matrix, since the
    // interface has already drawn by the time FFX::Finish() runs. The engine-space m_projection keeps the -1..1
    // z form and would give a view depth twice the real one. a0/b1 are the view-to-clip scales the kernel needs
    // to turn the world-unit radius into a texture offset.
    const C44Matrix& proj = s_frameProj;
    float zScale = proj.c2;
    float zBias = proj.d2;
    float projX = proj.a0;
    float projY = proj.b1;

    // c2 is 0 for an orthographic projection, which has no distance to recover: on the glue screens this
    // pass has nothing to do.
    if (!(zBias < 0.0f) || !(zScale > 0.0f)) {
        if (!s_loggedOrtho) {
            s_loggedOrtho = true;
            GxLog("SSAO: not a perspective frame (c2 %g d2 %g), skipping", zScale, zBias);
        }

        return;
    }

    if (!s_aoPs) {
        SsaoTrace("SsaoEnd: creating shaders");
        s_aoPs = CreatePixelShader(SSAO::kSsaoPs, sizeof(SSAO::kSsaoPs), "SSAO");
        s_combinePs = CreatePixelShader(SSAO::kSsaoCombinePs, sizeof(SSAO::kSsaoCombinePs), "SSAOCombine");
        SsaoTrace("SsaoEnd: shaders ao %d combine %d", s_aoPs ? s_aoPs->m_valid : -1, s_combinePs ? s_combinePs->m_valid : -1);
    }

    if (!s_aoPs || !s_aoPs->m_valid || !s_combinePs || !s_combinePs->m_valid) {
        if (!s_loggedNoShader) {
            s_loggedNoShader = true;
            GxLog("SSAO: no usable pixel shader on this device, effect off");
        }

        return;
    }

    float radius = s_cvarSsaoRadius ? s_cvarSsaoRadius->m_floatValue : 1.0f;
    float intensity = s_cvarSsaoIntensity ? s_cvarSsaoIntensity->m_floatValue : 1.0f;

    if (radius < 0.1f) {
        radius = 0.1f;
    }

    if (!s_loggedActive) {
        s_loggedActive = true;
        GxLog("SSAO: active %dx%d, c2 %g d2 %g a0 %g b1 %g, radius %g intensity %g",
              s_depth.m_size.x, s_depth.m_size.y, zScale, zBias, projX, projY, radius, intensity);
    }

    float debug = s_cvarSsaoDebug ? static_cast<float>(s_cvarSsaoDebug->m_intValue) : 0.0f;

    // 1. Occlusion into s_ao. Constants are set immediately before the draw because the vertex/pixel
    //    constant blocks are shared state the quad setup does not own. Bias is 1e-3 in the depth buffer's
    //    own units, scaled by the view depth so it stays a fixed fraction of the radius.
    float ao0[4];
    // World-space bias: the kernel compares view depths, so this is in the same world units as the radius.
    ao0[0] = radius;
    ao0[1] = 0.05f * radius;
    ao0[2] = intensity;
    ao0[3] = zScale;

    // b1/a0 rather than 1/d2 and 1/d3: see the shader's note. Without these the kernel collapses to a
    // sub-pixel radius and the whole pass outputs a flat 1.0.
    float ao1[4];
    ao1[0] = zBias;
    ao1[1] = projX;
    ao1[2] = projY;
    ao1[3] = 0.0f;

    GxShaderConstantsSet(GxSh_Pixel, 0, ao0, 1);
    GxShaderConstantsSet(GxSh_Pixel, 1, ao1, 1);

    SsaoTraceFrame("SsaoEnd: step1 setup (c2 %g d2 %g radius %g)", zScale, zBias, radius);

    CImVector white;
    white.b = 0xFF;
    white.g = 0xFF;
    white.r = 0xFF;
    white.a = 0xFF;

    Tex* occlusion = &s_ao;

    // SetupVB_PCT dereferences the shade array unconditionally, so a null one faults on the first vertex. The
    // occlusion shaders do not declare the diffuse colour, but the vertex still carries it and the engine's
    // own pass always supplies a real array. 1.0 gives the full-intensity value (0xFF), the neutral one.
    static float s_shade[4] = { 1.0f, 1.0f, 1.0f, 1.0f };

    SsaoTraceFrame("SsaoEnd: step1 constants done");

    // Debug 3 swaps the occlusion for the reconstructed view depth in distinct steps, so the constants the
    // reconstruction depends on (c2, d2) can be read straight off the screen instead of guessed at.
    if (debug >= 3.0f) {
        s_probePs = CreatePixelShader(SSAO::kSsaoProbePs, sizeof(SSAO::kSsaoProbePs), "SSAOProbe");

        if (s_probePs && s_probePs->m_valid) {
            float probe[4];
            probe[0] = radius;
            probe[1] = zBias;
            probe[2] = 0.0f;
            probe[3] = zScale;

            GxShaderConstantsSet(GxSh_Pixel, 1, probe, 1);

            SetupGx(&s_probe);
            SetupGeo_Quad(s_probe.m_size, s_depth.m_size, s_depth.m_texSize, s_gridPos, s_gridTex, false);
            SetupVB_PCT(4, s_gridPos, s_gridTex, s_shade);
            // Bits over EGxVertexAttrib: Position(0) | Color0(4) | TexCoord0(6) = the s_vertexAttribsPCT layout
            // just set, and the same mask the engine's own PCT pass passes (FFXEffects.cpp).
            g_theGxDevicePtr->PrimVertexMask(0x51);
            SetupIB();
            g_theGxDevicePtr->RsSet(GxRs_Texture0, TextureGetGxTex(s_depth.m_texture, 1, nullptr));
            g_theGxDevicePtr->RsSet(GxRs_PixelShader, s_probePs);
            g_theGxDevicePtr->Draw(&s_quadBatch, 1);
            SetupReset();

            occlusion = &s_probe;
            SsaoTraceFrame("SsaoEnd: debug probe drawn");
        }
    } else {
        SetupGx(&s_ao);
        SetupGeo_Quad(s_ao.m_size, s_depth.m_size, s_depth.m_texSize, s_gridPos, s_gridTex, false);
        SetupVB_PCT(4, s_gridPos, s_gridTex, s_shade);
        // Bits over EGxVertexAttrib: Position(0) | Color0(4) | TexCoord0(6) = the s_vertexAttribsPCT layout
        // just set, and the same mask the engine's own PCT pass passes (FFXEffects.cpp).
        g_theGxDevicePtr->PrimVertexMask(0x51);
        SetupIB();
        g_theGxDevicePtr->RsSet(GxRs_Texture0, TextureGetGxTex(s_depth.m_texture, 1, nullptr));
        g_theGxDevicePtr->RsSet(GxRs_PixelShader, s_aoPs);
        g_theGxDevicePtr->Draw(&s_quadBatch, 1);
        SetupReset();
        SsaoTraceFrame("SsaoEnd: step1 ao drawn");
    }


    // 2. Blur the occlusion and multiply it into the frame. The blur taps are interpolated coordinates
    //    from SetupVB_PCT4, so all four land on the occlusion texture; SetupGx with a null texture makes
    //    the backbuffer the target, and GxBlend_Mod multiplies the result by what is already there.
    static C2Vector s_offsets[4] = {
        C2Vector(-1.5f, -1.5f),
        C2Vector(0.5f, -1.5f),
        C2Vector(0.5f, 0.5f),
        C2Vector(-1.5f, 0.5f)
    };

    float scale[4];
    scale[0] = s_ao.m_texelSize.x;
    scale[1] = s_ao.m_texelSize.y;
    scale[2] = 0.0f;
    scale[3] = 0.0f;

    GxShaderConstantsSet(GxSh_Pixel, 0, scale, 1);

    // c1 drives the combine shader's debug branches; it is only read there.
    float dbg[4];
    dbg[0] = debug;
    dbg[1] = 0.0f;
    dbg[2] = 0.0f;
    dbg[3] = 0.5f;

    GxShaderConstantsSet(GxSh_Pixel, 1, dbg, 1);

    Tex backbuffer;
    backbuffer.m_texture = nullptr;
    backbuffer.m_size = occlusion->m_size;
    // The backbuffer is the window, not the occlusion texture, so its tex size is the window size. Using the
    // texture's (possibly padded) size here would shrink the viewport to a fraction of the screen.
    backbuffer.m_texSize = occlusion->m_size;
    backbuffer.m_texelSize = occlusion->m_texelSize;

    // Refuse to composite without a texture to sample. GxBlend_Mod multiplies the frame by whatever is bound, so
    // a null occlusion would darken the whole screen to black, every frame, with no way back until a restart.
    if (!occlusion->m_texture || !TextureGetGxTex(occlusion->m_texture, 1, nullptr)) {
        SsaoTraceFrame("SsaoEnd: occlusion texture missing, skipping the combine");
        return;
    }

    SsaoTraceFrame("SsaoEnd: step2 setup");

    SetupGx(&backbuffer);
    SetupGeo_Quad(backbuffer.m_size, occlusion->m_size, occlusion->m_texSize, s_gridPos, s_gridTex, false);

    SetupVB_PCT4(s_gridPos, white, s_gridTex, s_offsets, occlusion->m_texelSize);
    g_theGxDevicePtr->PrimVertexMask(0x3D1);
    SetupIB();
    g_theGxDevicePtr->RsSet(GxRs_Texture0, TextureGetGxTex(occlusion->m_texture, 1, nullptr));
    // Debug 1 shows the occlusion unreduced; 2 multiplies by a constant; 3 by the probe.
    g_theGxDevicePtr->RsSet(GxRs_BlendingMode, debug >= 1.0f ? GxBlend_Opaque : GxBlend_Mod);
    g_theGxDevicePtr->RsSet(GxRs_PixelShader, s_combinePs);
    g_theGxDevicePtr->Draw(&s_quadBatch, 1);
    SetupReset();
    SsaoTraceFrame("SsaoEnd: done");
}

}