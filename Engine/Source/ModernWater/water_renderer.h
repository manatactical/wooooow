#ifndef ENGINE_SOURCE_MODERNWATER_WATER_RENDERER_H
#define ENGINE_SOURCE_MODERNWATER_WATER_RENDERER_H

#include "Engine/Source/ModernWater/water_contacts.h"
#include "Engine/Source/ModernWater/water_data.h"
#include "Engine/Source/ModernWater/water_fft.h"
#include "Engine/Source/ModernWater/water_ripples.h"
#include "Engine/Source/ModernWater/water_types.h"

#include <d3d9.h>

class CGxTex;
class CTexture;

#include <cstdint>
#include <vector>

// Not in the original 3.3.5a binary. Modern water's shading pass, ported from the WXL extension module
// wxl-modern-water (ex CoAVolFog).
//
// The module had to run as a frame-wide second pass: it could not recompile the client, so it caught each water draw
// through a D3D9 device wrapper, tagged the pixels it covered with a stencil reference, and shaded every tagged pixel
// afterwards from a captured frame and depth. That is what forced the wrapper and the driver-specific multisampled
// depth copy.
//
// This tree compiles the client, so the same picture is reached differently, with no stencil and no second pass:
//
//   Begin       Called from Liquid::CRenderer::Flush before the transparent liquid draws. Captures the frame and
//               linearises the frame's depth into m_sceneDepthBefore.
//   water draw  The client's liquid material draws into the frame exactly as before, except that it now writes depth
//               (see MaterialProcWater::Render and ModernWater::WaterPassArmed).
//   ShadeRun    Called by the liquid renderer when a class's contiguous run of draws ends. Re-linearises the depth
//               into m_waterDepth and shades once; the shader finds the water by comparing the two depth copies, and
//               pixels the water never covered are passed through untouched.
//   End         Closes the pass and restores the device.
//
// Coverage without a stencil is a depth comparison: where the water wrote, the frame depth became the water surface,
// which is nearer than the scene behind it. Shading per class run, rather than all classes at the end, is what keeps
// one class from repainting another's pixels -- the liquid instances are sorted by material, so a class's draws are
// contiguous and each is resolved before the next begins.
//
// Two things this port needs from the rest of the tree, both of which are compile-time changes rather than patches:
// the frame's shared depth is INTZ (the only D3D9 depth format that can also be sampled), and the water materials
// write depth while the pass is armed.
//
// The shading itself is unchanged: same waterdata.bin, same FFT, same constants, same shader.
// The frame inputs the shading needs. Gathered from the client's own structures, not from absolute addresses.
struct WaterFrameInputs {
    float cameraRelativeView[16] = {};
    float glProjection[16] = {};
    float camPos[3] = {};
    float camTarget[3] = {};
    D3DVIEWPORT9 viewport = {};
    float toLight[3] = {};
    bool lightIsMoon = false;
    uint32_t sunColor = 0;
    uint32_t directColor = 0;
    uint32_t ambientColor = 0;
    uint32_t fogColor = 0;
    float fogStart = 0.0f;
    float fogEnd = 0.0f;
    bool inLiquid = false;
    int mapId = 0;
    uint32_t skyColors[6] = {};
    uint32_t riverColors[2] = {};
    bool stockFogApplies = false;
};

// Everything the module read from its ini. Hardcoded here to the module's defaults, except that the pass is
// simply always on at full quality: this client has no overlay panel and no second ini to keep in sync.
struct WaterTimeInputs {
    float clarity = 0.7f;
    float waves = 0.7f;
    float wind = 3.0f;
    float zoneColors = 0.7f;
    float foam = 1.0f;
    float reflections = 0.75f;
    float specular = 1.0f;
    float ripples = 0.5f;
    int quality = 2;
};

class WaterRenderer {
    public:
    ~WaterRenderer();

    void ReleaseDefaultPool();
    void ReleaseAll();

    bool Ready() const { return m_prepared; }
    bool Prepare(IDirect3DDevice9* device, const WaterTimeInputs& tuning);

    // Not in the original binary. Phase one. Returns false when the pass cannot run this frame, in which case
    // the client's liquid draws happen untagged and End does nothing.
    bool Begin(IDirect3DDevice9* device, const void* liquidRenderer, const WaterFrameInputs& inputs,
               const WaterTimeInputs& tuning);

    // Not in the original binary. Tags one liquid draw with its class, called from the liquid renderer just before
    // the instance draws. A liquid this pass does not shade is left untagged and keeps the client's material.
    void Tag(IDirect3DDevice9* device, WaterClass waterClass);

    // Not in the original binary. Shades the water drawn since the last shade -- one class's contiguous run of draws --
    // and moves the baseline forward. Called from the liquid renderer when a class's run ends, so each class is resolved
    // by its own preset without a per-pixel class mask.
    void ShadeRun(IDirect3DDevice9* device, WaterClass waterClass);

    // Not in the original binary. Phase two: closes the pass and puts the device back. The shading itself already
    // happened per class run.
    void End(IDirect3DDevice9* device);

    void Abort(IDirect3DDevice9* device);

    // Not in the original binary. False once the option, the data or the device has ruled the pass out, which is
    // what lets the liquid material stop tagging and stop paying for the classification.
    bool Active() const { return m_active; }
    void SetActive(bool active) { m_active = active; }

    // Not in the original binary. Refreshed once per frame with the units standing in liquid.
    void SetContacts(const WaterContactFrame& contacts) { m_contactFrame = contacts; }

    bool Armed() const { return m_armed; }
    unsigned ShadedClassMask() const { return m_shadedClassMask; }
    bool WavesSimulated() const { return m_wavesSimulated; }
    const char* LastSkipReason() const { return m_skip; }

    private:
    struct Float4 {
        float x, y, z, w;
    };

    // One class's pixel constants, in the register order vw_constants.hlsli declares. Uploaded as one block.
    struct ShadingConstants {
        Float4 light;
        Float4 sunColour;
        Float4 ambient;
        Float4 isotropicLight;
        Float4 sky[5];
        Float4 stockFogColour;
        Float4 stockFog;
        Float4 absorption;
        Float4 scatteringIntensities;
        Float4 scatteringTop;
        Float4 scatteringBottom;
        Float4 depthFadeFoam;
        Float4 shoreFoam;
        Float4 waveFoam;
        Float4 waveFoamScaling;
        Float4 surfaceResponse;
        Float4 inverseTileSizes;
        Float4 waveControl;
        Float4 foamScroll;
        Float4 depthFoamScroll;
        Float4 depthDecode;
        Float4 maskTints[10];
        Float4 surfaceOptics;
        // The 20 reflection-fog layer registers. Unused by this pass -- a stock-fog frame takes the branch that
        // ignores them -- but they must be present and zeroed, because everything after this point is addressed
        // by register number and skipping them would shift the ripple registers by twenty slots.
        Float4 reflectionFogLayers[20];
        Float4 reflectionFogRange;
        Float4 rippleWindow;
        Float4 rippleShape;
        Float4 rippleFade;
    };

    // 60 registers, uploaded from c9 and ending at c68 -- see vw_constants.hlsli, where cRippleFade is the last
    // one the shading pass sets. Everything before it is addressed by register number, so the field order here is
    // not a convenience: it is the register map, and padding or reordering it feeds the shader the wrong values.
    static_assert(sizeof(ShadingConstants) / sizeof(Float4) == 60,
                  "the shading block is registers c9..c68, ending at cRippleFade");

    struct FoamMaskTexture {
        int32_t sources[3] = {};
        bool packed = false;
        bool present[3] = {};
        IDirect3DTexture9* texture = nullptr;
    };

    struct SavedTargets {
        IDirect3DSurface9* colour[4] = {};
        IDirect3DSurface9* depth = nullptr;
        IDirect3DVertexBuffer9* stream = nullptr;
        UINT streamOffset = 0;
        UINT streamStride = 0;
    };

    bool Skip(const char* reason);
    bool EnsureShaders(IDirect3DDevice9* device);
    bool EnsureStateBlock(IDirect3DDevice9* device);
    bool EnsureFlatTexture(IDirect3DDevice9* device);
    bool EnsureCopies(IDirect3DDevice9* device, IDirect3DSurface9* target, UINT width, UINT height);
    bool EnsureDepthSource(IDirect3DDevice9* device, IDirect3DSurface9* bound);
    bool UsableTargets(IDirect3DSurface9* depthSurface, const D3DVIEWPORT9& viewport, D3DSURFACE_DESC& desc);

    void SaveTargets(IDirect3DDevice9* device);
    void RestoreTargets(IDirect3DDevice9* device);
    void ReleaseTargets();
    void CaptureClientState();
    // Puts the hardware back on the render states the client had, so the gx cache and the device agree again.
    void RestoreClientState(IDirect3DDevice9* device);
    void SetPassState(IDirect3DDevice9* device);

    bool CopySceneColour(IDirect3DDevice9* device, IDirect3DSurface9* target, const D3DVIEWPORT9& viewport,
                         IDirect3DTexture9* destination);
    void CopyDepthRaw(IDirect3DDevice9* device);
    void RestoreSceneDepth(IDirect3DDevice9* device);
    bool CopyFrameDepth(IDirect3DDevice9* device, const D3DVIEWPORT9& viewport);
    void CopyDepthTargetLinear(IDirect3DDevice9* device, IDirect3DTexture9* source, IDirect3DTexture9* copy);
    bool BuildCommonConstants(const D3DSURFACE_DESC& depthDesc);

    void PlanFoamMasks(const WaterData& data);
    void EnsureMasks(IDirect3DDevice9* device);
    void ReleaseMasks();
    const FoamMaskTexture* WaveFoamMasks(const WaterPreset& preset) const;
    IDirect3DTexture9* MaskTexture(int32_t index) const;
    bool MaskPresent(const WaterPreset& preset, int slot) const;

    bool PrepareWaves(IDirect3DDevice9* device);
    bool SimulateWaves(IDirect3DDevice9* device, double seconds);
    bool SimulateRipples(IDirect3DDevice9* device, double seconds);
    void FillRippleConstants(double seconds);
    uint32_t DrawnTileMask() const;

    void FillClassConstants(ShadingConstants& constants, const WaterPreset& preset, WaterClass waterClass,
                            double seconds) const;
    void BindClassTextures(IDirect3DDevice9* device, const WaterPreset& preset);
    // Shades one class over the whole viewport, masked by the depth-difference coverage the shader computes.
    void ShadeClass(IDirect3DDevice9* device, WaterClass waterClass, double seconds);

    bool m_prepared = false;
    bool m_active = true;
    bool m_armed = false;
    const char* m_skip = "";
    // The last skip reason written to the log, so each distinct stand-down is reported once rather than every frame.
    char m_loggedSkip[96] = {};
    bool m_loggedFirstShade = false;
    bool m_loggedDepthMapping = false;
    IDirect3DTexture9* m_depthRaw = nullptr;
    IDirect3DPixelShader9* m_depthRawShader = nullptr;
    IDirect3DPixelShader9* m_depthRestoreShader = nullptr;
    bool m_loggedEndState = false;
    bool m_loggedDepthFormats = false;

    IDirect3DVertexShader9* m_vs = nullptr;
    IDirect3DVertexDeclaration9* m_decl = nullptr;
    IDirect3DPixelShader9* m_depthCopyShader = nullptr;
    IDirect3DPixelShader9* m_packedDepthCopyShader = nullptr;
    IDirect3DPixelShader9* m_shade[3] = {};
    IDirect3DStateBlock9* m_state = nullptr;
    IDirect3DTexture9* m_flat = nullptr;
    std::vector<FoamMaskTexture> m_foamMasks;
    bool m_masksPlanned = false;
    bool m_masksUploaded = false;
    uint32_t m_maskRevision = 0;

    SavedTargets m_saved;
    bool m_stateCaptured = false;

    IDirect3DTexture9* m_sceneColour = nullptr;
    // The scene depth, borrowed from the frame's shared INTZ depth target (see CUnitOutline::DepthPassTexture).
    // Borrowed, not owned: nothing here releases it, and it is only valid while the depth pass is bound.
    IDirect3DTexture9* m_sceneDepthTexture = nullptr;
    // Two linearised copies of that depth: one taken before the liquid draws and one after them. The water is exactly
    // where the two differ, which is how this pass finds its pixels without a stencil.
    IDirect3DTexture9* m_sceneDepthBefore = nullptr;
    // The depth surface the water draws into (the shared INTZ depth), kept so the mid-loop restores in ShadeRun can
    // rebind exactly what Begin chose instead of consulting the device.
    IDirect3DSurface9* m_waterDepthSurface = nullptr;
    IDirect3DTexture9* m_waterDepth = nullptr;
    UINT m_depthW = 0;
    UINT m_depthH = 0;
    UINT m_copyW = 0;
    UINT m_copyH = 0;
    bool m_packedDepth = false;
    bool m_copyFailed = false;
    bool m_depthFailed = false;

    float m_common[9][4] = {};
    D3DSURFACE_DESC m_depthDesc = {};

    WaterFrameInputs m_inputs = {};
    WaterTimeInputs m_tuning = {};
    unsigned m_draws[kWaterClassCount] = {};
    int m_shadingVariant = -1;
    unsigned m_shadedClassMask = 0;
    int m_shadedClasses = 0;

    double m_lastSeconds = -1.0;
    double m_lastWaveSeconds = -1.0;
    bool m_wavesValid = false;
    bool m_wavesSimulated = false;
    WaterFftSettings m_waveSettings = {};
    uint32_t m_waveTiles = 0;
    WaterFft m_fft;

    WaterRipples m_ripples;
    WaterContactTracker m_contacts;
    WaterContactFrame m_contactFrame = {};
    int m_rippleMapId = 0;
    double m_lastRippleSeconds = -1.0;
    bool m_ripplesShaded = false;
    bool m_loggedRippleFailureShown = false;
    Float4 m_rippleWindow = {};
    Float4 m_rippleShape = {};
    Float4 m_rippleFade = {};

    ShadingConstants m_classConstants[kWaterClassCount] = {};
};

// Not in the original binary. The single renderer every liquid material and the liquid renderer share.
WaterRenderer& GlobalWaterRenderer();

#endif
