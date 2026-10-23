#include "Engine/Source/ModernWater/water_renderer.h"

#include "Engine/Source/Gx/CGxDevice/CGxDevice.h"
#include "Engine/Source/Gx/CGxDevice/CGxShader.h"
#include "Engine/Source/Gx/Gx.h"
#include "Engine/Source/Gx/CGxTex.h"
#include "Engine/Source/Services/Texture.h"
#include "Engine/Source/Base/Handle.h"
#include "Engine/Source/ModernWater/fullscreen_triangle.h"
#include "Engine/Source/ModernWater/log.h"
#include "Engine/Source/ModernWater/water_classify.h"
#include "Engine/Source/UnitOutline/UnitOutline.h"

#include "modernwater-shaders/ps_vw_depth.h"
#include "modernwater-shaders/ps_vw_depth_packed.h"
#include "modernwater-shaders/ps_vw_depth_raw.h"
#include "modernwater-shaders/ps_vw_depth_restore.h"
#include "modernwater-shaders/ps_vw_shade_high.h"
#include "modernwater-shaders/ps_vw_shade_low.h"
#include "modernwater-shaders/ps_vw_shade_mid.h"
#include "modernwater-shaders/vs_fullscreen.h"

#include <algorithm>
#include <vector>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

// TEMPORARY: set the environment variable WOWWATER_DEBUG_VIEW before launching to paint the water with a debug view
// (1 normal, 2 foam, 3 transmittance, 4 reflection, 5 class: lake blue, river green, ocean red, interior yellow).
float TemporaryDebugView() {
    static const float view = [] {
        const char* value = std::getenv("WOWWATER_DEBUG_VIEW");
        return value ? static_cast<float>(std::atoi(value)) : 0.0f;
    }();
    return view;
}

}  // namespace

namespace {

// Register and sampler plan of the ported shader. These are not free choices: vw_constants.hlsli names every
// one of them, so a change here is a change to the shader.
constexpr UINT kWaterPixelConstants = 72;
constexpr DWORD kWaterSamplerStages = 16;
constexpr UINT kCommonConstants = 9;
constexpr UINT kShadingFirstConstant = 9;
constexpr DWORD kMaxRenderTargets = 4;
constexpr DWORD kSceneColourStage = 0;
constexpr DWORD kSceneDepthStage = 1;
constexpr DWORD kWaterDepthStage = 2;
constexpr DWORD kFirstSurfaceStage = 3;
constexpr DWORD kFirstFoamStateStage = 7;
constexpr DWORD kWaveFoamMaskStage = 11;
constexpr DWORD kRippleStage = 12;
constexpr DWORD kShoreFoamMaskStage = 14;
constexpr DWORD kDepthFoamMaskStage = 15;
constexpr int kShoreFoamSlot = static_cast<int>(WaterMaskSlot::ShoreFoam);
constexpr int kDepthFoamSlot = static_cast<int>(WaterMaskSlot::DepthFoam);

constexpr DWORD kColourWriteRgb = D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE;
constexpr DWORD kColourWriteAll = kColourWriteRgb | D3DCOLORWRITEENABLE_ALPHA;

constexpr float kWaterMaxViewDepth = 65536.0f;
constexpr float kMinViewportDepthExtent = 0.01f;
constexpr float kDeepestWorldDepthInFullRangeViewport = 0.9999995f;
constexpr float kWorldDepthMargin = 2.0e-6f;
constexpr UINT kMinWorldViewportSide = 16;
constexpr float kPackedDepthLevels = 16777215.0f;
constexpr float kByteMax = 255.0f;
constexpr float kHighByteWeight = 65536.0f;
constexpr float kMidByteWeight = 256.0f;
constexpr float kDisplayGamma = 2.2f;
constexpr float kSunVisibilityHalfWidth = 0.1f;
constexpr float kWaterF0 = 0.02f;
// Multiplies how often every foam mask repeats across the water; below 1 the foam patterns are larger and repeat less.
constexpr float kFoamTilingScale = 1.0f;
constexpr int kSchlickExponent = 5;
constexpr float kMinClarity = 0.01f;
constexpr float kShoreDistancePerDepth = 4.0f;
constexpr float kMinStockFogRange = 1e-3f;
constexpr double kMaxFoamStepSeconds = 0.1;
constexpr double kWaveUpdateSeconds = 1.0 / 30.0;
constexpr int kLowQuality = 1;
constexpr int kHighQuality = 3;
constexpr int kSkyReflectionsOnlyVariant = 0;
constexpr int kMaskUploadRetryPasses = 60;
constexpr float kReflectionFogActive = 1.0f;
constexpr int kFftResolutionLow = 128;
constexpr int kFftResolution = 256;
constexpr int kFftReferenceResolution = 256;
constexpr float kWindDirection[2] = {0.8f, 0.6f};
constexpr int kSkyTop = 0;
constexpr int kSkyMiddle = 1;
constexpr int kSkyUpperBand = 2;
constexpr int kSkyLowerBand = 3;
constexpr int kSkyHorizon = 4;
constexpr int kSkyColourOfBand[5] = {kSkyTop, kSkyMiddle, kSkyUpperBand, kSkyLowerBand, kSkyHorizon};
constexpr int kCloseWaterColour = 0;
constexpr int kFarWaterColour = 1;
constexpr double kRippleGapSeconds = 1.0;
constexpr float kRippleEdgeRampPerExtent = 1.0f / 16.0f;
constexpr float kRippleFadeYards = 4.0f;

// The render states and sampler states the pass owns. Captured into a state block so every value the shading
// pass touches is put back exactly as the client left it, including the ones this file never sets.
const D3DRENDERSTATETYPE kPassRenderStates[] = {
    D3DRS_ZENABLE,           D3DRS_ZWRITEENABLE,     D3DRS_ZFUNC,
    D3DRS_ALPHATESTENABLE,   D3DRS_ALPHABLENDENABLE, D3DRS_SRCBLEND,
    D3DRS_DESTBLEND,         D3DRS_BLENDOP,          D3DRS_SEPARATEALPHABLENDENABLE,
    D3DRS_CULLMODE,          D3DRS_STENCILENABLE,    D3DRS_STENCILFUNC,
    D3DRS_STENCILPASS,       D3DRS_STENCILFAIL,      D3DRS_STENCILZFAIL,
    D3DRS_STENCILREF,        D3DRS_STENCILMASK,      D3DRS_STENCILWRITEMASK,
    D3DRS_TWOSIDEDSTENCILMODE, D3DRS_CCW_STENCILFUNC, D3DRS_CCW_STENCILPASS,
    D3DRS_CCW_STENCILFAIL,   D3DRS_CCW_STENCILZFAIL, D3DRS_SCISSORTESTENABLE,
    D3DRS_COLORWRITEENABLE,  D3DRS_SRGBWRITEENABLE,  D3DRS_FOGENABLE,
    D3DRS_CLIPPLANEENABLE,   D3DRS_FILLMODE,
};

const D3DSAMPLERSTATETYPE kPassSamplerStates[] = {
    D3DSAMP_ADDRESSU,    D3DSAMP_ADDRESSV,       D3DSAMP_ADDRESSW,     D3DSAMP_BORDERCOLOR,
    D3DSAMP_MAGFILTER,   D3DSAMP_MINFILTER,      D3DSAMP_MIPFILTER,    D3DSAMP_MIPMAPLODBIAS,
    D3DSAMP_MAXMIPLEVEL, D3DSAMP_MAXANISOTROPY,  D3DSAMP_SRGBTEXTURE,
};

// gx keeps its own copy of the shader constants and uploads only the range it has marked dirty, and it marks a register
// dirty only when its value changes. This pass writes constants straight to the device and restores a state block that
// holds whatever the hardware had at Begin -- which, with gx's uploads still pending, is not what gx believes it holds.
// Afterwards gx and the hardware disagree and gx never resends the difference, so the client's later draws (the
// splash rings among them) run with stale constants. Marking every register dirty makes gx resend its own values.
void ForceShaderConstantUpload() {
    for (ShadowConstants& shadow : CGxDevice::s_shadowConstants) {
        shadow.dirtyMin = 0;
        shadow.dirtyMax = 255;
    }
}

template <typename T>
void SafeRelease(T*& pointer) {
    if (pointer) {
        pointer->Release();
        pointer = nullptr;
    }
}

double QpcSeconds() {
    static const double frequency = [] {
        LARGE_INTEGER value;
        QueryPerformanceFrequency(&value);
        return static_cast<double>(value.QuadPart);
    }();

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return static_cast<double>(now.QuadPart) / frequency;
}

double WaterSeconds() {
    return QpcSeconds();
}

bool CreateTarget(IDirect3DDevice9* device, UINT width, UINT height, D3DFORMAT format, IDirect3DTexture9** out) {
    return SUCCEEDED(device->CreateTexture(width, height, 1, D3DUSAGE_RENDERTARGET, format, D3DPOOL_DEFAULT, out,
                                           nullptr));
}

void SetTarget(IDirect3DDevice9* device, IDirect3DTexture9* texture) {
    IDirect3DSurface9* surface = nullptr;

    if (SUCCEEDED(texture->GetSurfaceLevel(0, &surface))) {
        device->SetRenderTarget(0, surface);
        surface->Release();
    }
}

void BindSampler(IDirect3DDevice9* device, DWORD stage, IDirect3DBaseTexture9* texture, D3DTEXTUREADDRESS address,
                 D3DTEXTUREFILTERTYPE filter, D3DTEXTUREFILTERTYPE mipFilter) {
    device->SetTexture(stage, texture);
    device->SetSamplerState(stage, D3DSAMP_ADDRESSU, address);
    device->SetSamplerState(stage, D3DSAMP_ADDRESSV, address);
    device->SetSamplerState(stage, D3DSAMP_ADDRESSW, address);
    device->SetSamplerState(stage, D3DSAMP_MAGFILTER, filter);
    device->SetSamplerState(stage, D3DSAMP_MINFILTER, filter);
    device->SetSamplerState(stage, D3DSAMP_MIPFILTER, mipFilter);
    device->SetSamplerState(stage, D3DSAMP_MIPMAPLODBIAS, 0);
    device->SetSamplerState(stage, D3DSAMP_MAXMIPLEVEL, 0);
    device->SetSamplerState(stage, D3DSAMP_SRGBTEXTURE, FALSE);
}

void BindPointSampler(IDirect3DDevice9* device, DWORD stage, IDirect3DBaseTexture9* texture,
                      D3DTEXTUREADDRESS address) {
    BindSampler(device, stage, texture, address, D3DTEXF_POINT, D3DTEXF_NONE);
}

RECT ViewportRect(const D3DVIEWPORT9& viewport) {
    return {static_cast<LONG>(viewport.X), static_cast<LONG>(viewport.Y),
            static_cast<LONG>(viewport.X + viewport.Width), static_cast<LONG>(viewport.Y + viewport.Height)};
}

struct WorldDepthMapping {
    float atInfinity;
    float perInverseViewDepth;
    float deepest;
};

WorldDepthMapping MapWorldDepth(const float* projection, const D3DVIEWPORT9& viewport) {
    // The depth this decodes is D3D's: the shared INTZ surface holds a [0,1] value written by the device's native
    // projection, not a GL [-1,1] one. So the mapping is derived from the native matrix directly and not by
    // half-and-adding a GL convention into place.
    //
    // D3D projects as depth = (m22 * z + m32) / -z, which inverts to a view distance of
    //     viewZ = m32 / (depth + m22)
    // with z measured forward from the eye. The shader computes perInverseViewDepth / (depth - atInfinity), so those
    // two are m32 and -m22. For the client's own projection that puts atInfinity just past 1.0, which is why the far
    // plane is reachable and why BeyondFarClip still works.
    //
    // An earlier version half-and-added the GL form: (1 + m22) * 0.5 for atInfinity and m32 * 0.5 for the reciprocal,
    // which is the [-1,1] to [0,1] remap. Applied to an already-[0,1] depth it inverted the sign of every result, so
    // terrain linearised negative and drew black while only the sky -- which escapes through the far-clip branch --
    // came out lit.
    const bool usableRange = viewport.MaxZ - viewport.MinZ > kMinViewportDepthExtent && viewport.MinZ >= 0.0f &&
                             viewport.MaxZ <= 1.0f;
    const float worldMinZ = usableRange ? viewport.MinZ : 0.0f;
    const float worldExtent = usableRange ? viewport.MaxZ - viewport.MinZ : 1.0f;
    const float worldMaxZ = worldMinZ + worldExtent;

    // The viewport's own depth range is folded in before the two terms are formed, so a partial depth range decodes
    // the same way a full one does.
    //
    // atInfinity is +m22, not -m22. The client's projection is left-handed, so it stores depth as
    //     depth = m22 + m32 / z
    // -- both terms entering with the sign the matrix holds. The inverse is therefore
    //     z = m32 / (depth - m22)
    // which is exactly the shader's perInverseViewDepth / (depth - atInfinity) with atInfinity = +m22. Negating it
    // instead made the denominator about 2.0 on every real pixel rather than a few thousandths, so terrain decoded to
    // a small negative number and drew black while only the sky -- which exits through the far-clip branch -- lit up.
    // Sanity: with this client's numbers (m22 1.00025, m32 -0.20005) a surface at 50 yards has depth 0.9962517 and
    // decodes back to exactly 50.
    //
    // The viewport maps the native [0,1] depth into [minZ, maxZ], so what the buffer holds is
    //     stored = minZ + extent * (m22 + m32 / z)
    // and the inverse is z = extent * m32 / (stored - minZ - extent * m22). That makes atInfinity = minZ + extent * m22
    // and the reciprocal term extent * m32: the extent multiplies, it does not divide. Dividing (the earlier form) is only
    // right for extent 1, and this client draws its world into 0..0.94, which decoded every terrain pixel to a few yards.
    return {worldMinZ + worldExtent * projection[10], worldExtent * projection[14],
            worldMaxZ >= kDeepestWorldDepthInFullRangeViewport ? kDeepestWorldDepthInFullRangeViewport
                                                               : worldMaxZ + kWorldDepthMargin};
}

// The camera-relative view matrix is affine, so its inverse is the transposed rotation with the negated
// translation. Written out rather than using a general 4x4 inverse, which would only add rounding.
bool InvertAffineView(const float* matrix, float* out) {
    out[0] = matrix[0];
    out[1] = matrix[4];
    out[2] = matrix[8];
    out[3] = 0.0f;
    out[4] = matrix[1];
    out[5] = matrix[5];
    out[6] = matrix[9];
    out[7] = 0.0f;
    out[8] = matrix[2];
    out[9] = matrix[6];
    out[10] = matrix[10];
    out[11] = 0.0f;
    out[12] = -(out[0] * matrix[12] + out[4] * matrix[13] + out[8] * matrix[14]);
    out[13] = -(out[1] * matrix[12] + out[5] * matrix[13] + out[9] * matrix[14]);
    out[14] = -(out[2] * matrix[12] + out[6] * matrix[13] + out[10] * matrix[14]);
    out[15] = 1.0f;

    for (int index = 0; index < 16; ++index) {
        if (!std::isfinite(out[index])) {
            return false;
        }
    }

    return true;
}

float Saturate(float value) {
    return std::clamp(value, 0.0f, 1.0f);
}

float SmoothStep(float edge0, float edge1, float value) {
    // A zero-width edge would divide by zero and push a NaN into the water's colours, where it spreads through the
    // whole shading of every pixel. Degenerating to a hard step is what the function means at zero width.
    const float width = edge1 - edge0;

    if (std::abs(width) < 1e-6f) {
        return value >= edge1 ? 1.0f : 0.0f;
    }

    const float t = Saturate((value - edge0) / width);
    return t * t * (3.0f - 2.0f * t);
}

float FresnelSchlick(float cosine) {
    return kWaterF0 +
           (1.0f - kWaterF0) * std::pow(1.0f - Saturate(cosine), static_cast<float>(kSchlickExponent));
}

void UnpackColour(uint32_t argb, float* rgb) {
    rgb[0] = static_cast<float>((argb >> 16) & 0xFF) / kByteMax;
    rgb[1] = static_cast<float>((argb >> 8) & 0xFF) / kByteMax;
    rgb[2] = static_cast<float>(argb & 0xFF) / kByteMax;
}

void LinearColour(uint32_t argb, float* rgb) {
    UnpackColour(argb, rgb);

    for (int channel = 0; channel < 3; ++channel) {
        rgb[channel] = std::pow(std::max(rgb[channel], 0.0f), kDisplayGamma);
    }
}

void ClampedTint(const float* linearTint, float* rgb) {
    for (int channel = 0; channel < 3; ++channel) {
        rgb[channel] = Saturate(linearTint[channel]);
    }
}

float ScrollOffset(double seconds, int axis, float multiplier) {
    const double travelled = seconds * kWindDirection[axis] * multiplier;
    return static_cast<float>(travelled - std::floor(travelled));
}

uint32_t MipSide(uint32_t size, uint32_t level) {
    const uint32_t side = size >> level;
    return side ? side : 1;
}

bool FillMaskLevel(IDirect3DTexture9* texture, UINT level, const std::vector<uint8_t>& texels, uint32_t side,
                   bool luminance) {
    D3DLOCKED_RECT locked = {};

    if (FAILED(texture->LockRect(level, &locked, nullptr, 0))) {
        return false;
    }

    for (uint32_t y = 0; y < side; ++y) {
        const uint8_t* source = texels.data() + static_cast<size_t>(y) * side;
        auto* row = static_cast<uint8_t*>(locked.pBits) + static_cast<size_t>(y) * locked.Pitch;

        if (luminance) {
            std::memcpy(row, source, side);
            continue;
        }

        auto* argb = reinterpret_cast<DWORD*>(row);

        for (uint32_t x = 0; x < side; ++x) {
            argb[x] = 0xFF000000u | source[x] * 0x00010101u;
        }
    }

    return SUCCEEDED(texture->UnlockRect(level));
}

IDirect3DTexture9* CreateMaskStaging(IDirect3DDevice9* device, const WaterMaskLevels& mask, D3DFORMAT format) {
    const UINT levels = static_cast<UINT>(mask.levels.size());
    const UINT size = mask.info.size;
    IDirect3DTexture9* staging = nullptr;

    if (FAILED(device->CreateTexture(size, size, levels, 0, format, D3DPOOL_SYSTEMMEM, &staging, nullptr))) {
        return nullptr;
    }

    for (UINT level = 0; level < levels; ++level) {
        if (!FillMaskLevel(staging, level, mask.levels[level], MipSide(size, level), format == D3DFMT_L8)) {
            staging->Release();
            return nullptr;
        }
    }

    return staging;
}

bool FillPackedLevel(IDirect3DTexture9* texture, UINT level, const std::vector<uint32_t>& texels, uint32_t side) {
    D3DLOCKED_RECT locked = {};

    if (FAILED(texture->LockRect(level, &locked, nullptr, 0))) {
        return false;
    }

    for (uint32_t y = 0; y < side; ++y) {
        std::memcpy(static_cast<uint8_t*>(locked.pBits) + static_cast<size_t>(y) * locked.Pitch,
                    texels.data() + static_cast<size_t>(y) * side, side * sizeof(uint32_t));
    }

    return SUCCEEDED(texture->UnlockRect(level));
}

IDirect3DTexture9* CreatePackedMaskTexture(IDirect3DDevice9* device, const WaterPackedMask& packed) {
    if (packed.levels.empty()) {
        return nullptr;
    }

    const UINT levels = static_cast<UINT>(packed.levels.size());
    IDirect3DTexture9* staging = nullptr;
    IDirect3DTexture9* texture = nullptr;
    bool uploaded = SUCCEEDED(device->CreateTexture(packed.size, packed.size, levels, 0, D3DFMT_A8R8G8B8,
                                                    D3DPOOL_SYSTEMMEM, &staging, nullptr));

    for (UINT level = 0; uploaded && level < levels; ++level) {
        uploaded = FillPackedLevel(staging, level, packed.levels[level], MipSide(packed.size, level));
    }

    uploaded = uploaded && SUCCEEDED(device->CreateTexture(packed.size, packed.size, levels, 0, D3DFMT_A8R8G8B8,
                                                           D3DPOOL_DEFAULT, &texture, nullptr)) &&
               SUCCEEDED(device->UpdateTexture(staging, texture));

    if (staging) {
        staging->Release();
    }

    if (!uploaded && texture) {
        texture->Release();
        texture = nullptr;
    }

    return texture;
}

IDirect3DTexture9* CreateMaskTexture(IDirect3DDevice9* device, const WaterMaskLevels& mask) {
    const UINT levels = static_cast<UINT>(mask.levels.size());
    const UINT size = mask.info.size;

    for (D3DFORMAT format : {D3DFMT_L8, D3DFMT_A8R8G8B8}) {
        IDirect3DTexture9* texture = nullptr;

        if (FAILED(device->CreateTexture(size, size, levels, 0, format, D3DPOOL_DEFAULT, &texture, nullptr))) {
            continue;
        }

        IDirect3DTexture9* staging = CreateMaskStaging(device, mask, format);
        const bool uploaded = staging && SUCCEEDED(device->UpdateTexture(staging, texture));

        if (staging) {
            staging->Release();
        }

        if (uploaded) {
            return texture;
        }

        texture->Release();
    }

    return nullptr;
}

int QualityIndex(const WaterTimeInputs& tuning) {
    return std::clamp(tuning.quality, 1, 3) - 1;
}

int ShadingVariant(const WaterTimeInputs& tuning) {
    return tuning.reflections > 0.0f ? QualityIndex(tuning) : kSkyReflectionsOnlyVariant;
}

// The magma preset. The water data file has no lava entry, so this is the one class whose numbers live in code.
// Lava is self-lit and opaque: it absorbs everything within a few inches, scatters almost nothing, and its colour
// comes from the client's own lava texture through the foam path, which is the only per-pixel surface detail the
// shader has. The values are deliberately blunt -- a physically-based lava is a different project.
WaterPreset MagmaPreset() {
    WaterPreset preset = {};
    preset.foreverLiquidId = 0;
    preset.absorption[0] = 24.0f;
    preset.absorption[1] = 48.0f;
    preset.absorption[2] = 96.0f;
    preset.absorption[3] = 1.0f;
    preset.scatteringIntensities[0] = 0.0f;
    preset.scatteringIntensities[1] = 0.0f;
    preset.scatteringIntensities[2] = 0.0f;
    preset.scatteringIntensities[3] = 0.0f;
    preset.scatteringTop[0] = 1.0f;
    preset.scatteringTop[1] = 0.35f;
    preset.scatteringTop[2] = 0.06f;
    preset.scatteringTop[3] = 1.0f;
    preset.scatteringBottom[0] = 0.85f;
    preset.scatteringBottom[1] = 0.16f;
    preset.scatteringBottom[2] = 0.02f;
    preset.scatteringBottom[3] = 1.0f;
    preset.depthFadeFoam[0] = 0.0f;
    preset.depthFadeFoam[1] = 0.0f;
    preset.depthFadeFoam[2] = 1.0f;
    preset.depthFadeFoam[3] = 1.0f;
    preset.shoreFoam[0] = 1.0f;
    preset.shoreFoam[1] = 1.0f;
    preset.shoreFoam[2] = 1.0f;
    preset.shoreFoam[3] = 1.0f;
    preset.waveFoam[0] = 1.0f;
    preset.waveFoam[1] = 0.05f;
    preset.waveFoam[2] = 0.0f;
    preset.waveFoam[3] = 0.0f;
    preset.waveFoamScaling[0] = 1.0f;
    preset.waveFoamScaling[1] = 1.0f;
    preset.waveFoamScaling[2] = 1.0f;
    preset.flow[0] = 0.0f;
    preset.flow[1] = 0.0f;
    preset.flow[2] = 0.0f;
    preset.flow[3] = 0.0f;
    preset.roughness[0] = 0.8f;
    preset.roughness[1] = 0.6f;
    preset.roughness[2] = 0.0f;
    preset.roughness[3] = 1.0f;

    for (int32_t& tile : preset.tiles) {
        tile = kWaterNoIndex;
    }

    for (int32_t& mask : preset.masks) {
        mask = kWaterNoIndex;
    }

    return preset;
}

}  // namespace

WaterRenderer& GlobalWaterRenderer() {
    static WaterRenderer renderer;
    return renderer;
}

WaterRenderer::~WaterRenderer() {
    ReleaseAll();
}

void WaterRenderer::ReleaseDefaultPool() {
    ReleaseTargets();
    ReleaseMasks();
    // The scene and water copies are default-pool render targets and do not survive a device reset.
    SafeRelease(m_sceneColour);
    SafeRelease(m_sceneDepthBefore);
    SafeRelease(m_waterDepthSurface);
    SafeRelease(m_waterDepth);
    m_copyW = m_copyH = 0;
    m_copyFailed = false;
    m_fft.ReleaseDefaultPool();
    m_ripples.ReleaseDefaultPool();
    SafeRelease(m_state);
    SafeRelease(m_vs);
    SafeRelease(m_decl);
    SafeRelease(m_depthCopyShader);
    SafeRelease(m_packedDepthCopyShader);
    SafeRelease(m_depthRawShader);
    SafeRelease(m_depthRestoreShader);
    SafeRelease(m_depthRaw);

    for (IDirect3DPixelShader9*& shader : m_shade) {
        SafeRelease(shader);
    }

    m_prepared = false;
}

void WaterRenderer::ReleaseAll() {
    ReleaseDefaultPool();
    m_fft.ReleaseAll();
    m_ripples.ReleaseAll();
    SafeRelease(m_flat);
}

bool WaterRenderer::Skip(const char* reason) {
    m_skip = reason;

    // A skip used to be silent, which makes "no water" impossible to diagnose: the pass can stand down at a dozen
    // points and the log said nothing. Each distinct reason is reported once, so the first run on a machine explains
    // itself and a repeating frame does not flood the log.
    if (reason && std::strcmp(reason, m_loggedSkip) != 0) {
        if (m_loggedSkip[0] == '\0') {
            VF_LOG_INFO("modern water pass inactive: %s", reason);
        } else {
            VF_LOG_INFO("modern water pass inactive: %s (was: %s)", reason, m_loggedSkip);
        }

        std::snprintf(m_loggedSkip, sizeof(m_loggedSkip), "%s", reason);
    }

    return false;
}

bool WaterRenderer::Prepare(IDirect3DDevice9* device, const WaterTimeInputs& tuning) {
    m_tuning = tuning;

    if (m_prepared) {
        return true;
    }

    if (!device) {
        return Skip("no device");
    }

    if (!GlobalWaterData().Loaded()) {
        return Skip("no water data");
    }

    if (!EnsureShaders(device) || !EnsureStateBlock(device) || !EnsureFlatTexture(device)) {
        return false;
    }

    PlanFoamMasks(GlobalWaterData());
    m_prepared = true;
    return true;
}

bool WaterRenderer::EnsureShaders(IDirect3DDevice9* device) {
    if (m_vs) {
        return true;
    }

    struct PixelShaderRequest {
        const char* name;
        const BYTE* code;
        IDirect3DPixelShader9** output;
    };

    const PixelShaderRequest pixels[] = {
        {"ps_vw_depth", g_ps_vw_depth, &m_depthCopyShader},
        {"ps_vw_depth_packed", g_ps_vw_depth_packed, &m_packedDepthCopyShader},
        {"ps_vw_depth_raw", g_ps_vw_depth_raw, &m_depthRawShader},
        {"ps_vw_depth_restore", g_ps_vw_depth_restore, &m_depthRestoreShader},
        {"ps_vw_shade_low", g_ps_vw_shade_low, &m_shade[0]},
        {"ps_vw_shade_mid", g_ps_vw_shade_mid, &m_shade[1]},
        {"ps_vw_shade_high", g_ps_vw_shade_high, &m_shade[2]},
    };

    static const D3DVERTEXELEMENT9 kElements[] = {
        {0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
        D3DDECL_END(),
    };

    const char* failedName = "vs_fullscreen";
    HRESULT result = device->CreateVertexShader(reinterpret_cast<const DWORD*>(g_vs_fullscreen), &m_vs);

    if (SUCCEEDED(result)) {
        for (const PixelShaderRequest& shader : pixels) {
            failedName = shader.name;
            result = device->CreatePixelShader(reinterpret_cast<const DWORD*>(shader.code), shader.output);

            if (FAILED(result)) {
                break;
            }
        }
    }

    if (SUCCEEDED(result)) {
        failedName = "water vertex declaration";
        result = device->CreateVertexDeclaration(kElements, &m_decl);
    }

    if (SUCCEEDED(result)) {
        return true;
    }

    VF_LOG_ERROR("modern water: shader initialization failed at %s (HRESULT 0x%08lX)", failedName,
                 static_cast<unsigned long>(result));

    // The shader bytecode is ps_3_0. A device that cannot take it -- shader model 2 hardware, or a driver that
    // has lost the device -- leaves the pass off for good and the client's own water stays.
    ReleaseDefaultPool();
    m_active = false;
    return Skip("water shader unsupported");
}

bool WaterRenderer::EnsureStateBlock(IDirect3DDevice9* device) {
    if (m_state) {
        return true;
    }

    if (FAILED(device->BeginStateBlock())) {
        return Skip("water state block recording failed");
    }

    for (D3DRENDERSTATETYPE state : kPassRenderStates) {
        device->SetRenderState(state, 0);
    }

    for (DWORD stage = 0; stage < kWaterSamplerStages; ++stage) {
        device->SetTexture(stage, nullptr);

        for (D3DSAMPLERSTATETYPE state : kPassSamplerStates) {
            device->SetSamplerState(stage, state, 0);
        }
    }

    float zeros[kWaterPixelConstants * 4] = {};
    device->SetVertexShader(nullptr);
    device->SetPixelShader(nullptr);
    device->SetPixelShaderConstantF(0, zeros, kWaterPixelConstants);
    device->SetVertexDeclaration(m_decl);
    device->SetStreamSource(0, nullptr, 0, 0);
    device->SetStreamSourceFreq(0, 1);

    D3DVIEWPORT9 viewport = {0, 0, 1, 1, 0.0f, 1.0f};
    device->SetViewport(&viewport);

    RECT scissor = {0, 0, 1, 1};
    device->SetScissorRect(&scissor);

    if (FAILED(device->EndStateBlock(&m_state)) || !m_state) {
        m_state = nullptr;
        return Skip("water state block recording failed");
    }

    return true;
}

bool WaterRenderer::EnsureFlatTexture(IDirect3DDevice9* device) {
    if (m_flat) {
        return true;
    }

    if (FAILED(device->CreateTexture(1, 1, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &m_flat, nullptr))) {
        m_flat = nullptr;
        return Skip("flat water texture creation failed");
    }

    D3DLOCKED_RECT locked = {};

    if (FAILED(m_flat->LockRect(0, &locked, nullptr, 0))) {
        SafeRelease(m_flat);
        return Skip("flat water texture creation failed");
    }

    *static_cast<DWORD*>(locked.pBits) = 0;
    m_flat->UnlockRect(0);
    return true;
}

void WaterRenderer::PlanFoamMasks(const WaterData& data) {
    ReleaseMasks();

    auto plan = [this](const int32_t (&sources)[kWaveFoamMaskSlots], bool packed) {
        for (const FoamMaskTexture& mask : m_foamMasks) {
            if (mask.packed == packed && std::equal(std::begin(sources), std::end(sources), mask.sources)) {
                return;
            }
        }

        FoamMaskTexture mask = {{sources[0], sources[1], sources[2]}, packed, {}, nullptr};
        m_foamMasks.push_back(mask);
    };

    for (const WaterPreset& preset : data.Presets()) {
        const int32_t wave[kWaveFoamMaskSlots] = {preset.masks[static_cast<int>(WaterMaskSlot::HighFoam)],
                                                  preset.masks[static_cast<int>(WaterMaskSlot::MidFoam)],
                                                  preset.masks[static_cast<int>(WaterMaskSlot::LowFoam)]};

        if (std::any_of(std::begin(wave), std::end(wave), [](int32_t index) { return index >= 0; })) {
            plan(wave, true);
        }

        for (int slot : {kShoreFoamSlot, kDepthFoamSlot}) {
            const int32_t single[kWaveFoamMaskSlots] = {preset.masks[slot], kWaterNoIndex, kWaterNoIndex};

            if (single[0] >= 0) {
                plan(single, false);
            }
        }
    }

    m_masksPlanned = true;
}

void WaterRenderer::EnsureMasks(IDirect3DDevice9* device) {
    const WaterData& data = GlobalWaterData();

    if (m_maskRevision != data.Revision() || !m_masksPlanned) {
        PlanFoamMasks(data);
        m_maskRevision = data.Revision();
    }

    if (m_masksUploaded) {
        return;
    }

    int uploaded = 0;

    for (FoamMaskTexture& mask : m_foamMasks) {
        if (!mask.texture && mask.packed) {
            const WaterPackedMask packed = PackWaveFoamMasks(data.Masks(), mask.sources);
            mask.texture = CreatePackedMaskTexture(device, packed);
            std::copy(std::begin(packed.present), std::end(packed.present), mask.present);
        } else if (!mask.texture) {
            mask.texture = CreateMaskTexture(device, data.Masks()[mask.sources[0]]);
        }

        uploaded += mask.texture ? 1 : 0;
    }

    m_masksUploaded = uploaded == static_cast<int>(m_foamMasks.size());

    if (!m_masksUploaded) {
        VF_LOG_INFO("modern water: %d of %d foam masks uploaded", uploaded,
                    static_cast<int>(m_foamMasks.size()));
    }
}

void WaterRenderer::ReleaseMasks() {
    for (FoamMaskTexture& mask : m_foamMasks) {
        SafeRelease(mask.texture);
    }

    m_foamMasks.clear();
    m_masksPlanned = false;
    m_masksUploaded = false;
}

const WaterRenderer::FoamMaskTexture* WaterRenderer::WaveFoamMasks(const WaterPreset& preset) const {
    // The three wave-foam masks are the High/Mid/Low slots, in that order. PlanFoamMasks builds the packed masks from
    // exactly those three, so the comparison has to use them too: using preset.masks[0..2] directly only matches when
    // the enum happens to number them 0,1,2, and silently loses all wave foam when it does not.
    const int32_t sources[kWaveFoamMaskSlots] = {preset.masks[static_cast<int>(WaterMaskSlot::HighFoam)],
                                                 preset.masks[static_cast<int>(WaterMaskSlot::MidFoam)],
                                                 preset.masks[static_cast<int>(WaterMaskSlot::LowFoam)]};

    for (const FoamMaskTexture& mask : m_foamMasks) {
        if (mask.packed && mask.texture &&
            std::equal(mask.sources, mask.sources + kWaveFoamMaskSlots, sources)) {
            return &mask;
        }
    }

    return nullptr;
}

IDirect3DTexture9* WaterRenderer::MaskTexture(int32_t index) const {
    for (const FoamMaskTexture& mask : m_foamMasks) {
        if (!mask.packed && mask.sources[0] == index && mask.texture) {
            return mask.texture;
        }
    }

    return nullptr;
}

bool WaterRenderer::MaskPresent(const WaterPreset& preset, int slot) const {
    if (slot >= kWaveFoamMaskSlots) {
        return MaskTexture(preset.masks[slot]) != nullptr;
    }

    const FoamMaskTexture* wave = WaveFoamMasks(preset);
    return wave && wave->present[slot];
}

void WaterRenderer::SaveTargets(IDirect3DDevice9* device) {
    ReleaseTargets();

    for (DWORD index = 0; index < kMaxRenderTargets; ++index) {
        device->GetRenderTarget(index, &m_saved.colour[index]);
    }

    device->GetDepthStencilSurface(&m_saved.depth);
    device->GetStreamSource(0, &m_saved.stream, &m_saved.streamOffset, &m_saved.streamStride);
}

void WaterRenderer::ReleaseTargets() {
    m_stateCaptured = false;

    for (auto*& surface : m_saved.colour) {
        SafeRelease(surface);
    }

    SafeRelease(m_saved.depth);
    SafeRelease(m_saved.stream);
}

void WaterRenderer::CaptureClientState() {
    if (!m_state) {
        return;
    }

    // A failed capture would leave the recording block's contents (a 1x1 viewport, no shaders, every state zeroed)
    // to be "restored" over the client, so it is only taken as captured when the device actually captured.
    m_stateCaptured = SUCCEEDED(m_state->Capture());
}

void WaterRenderer::RestoreClientState(IDirect3DDevice9* device) {
    // The pass talks to the device directly, so its copies and shading leave ZENABLE, ZWRITE, blending, shaders and
    // textures set the way *it* wants them while gx's cache still holds the client's values.
    //
    // That desync is silent and it is the reason this pass looked like it did nothing at all. gx early-outs whenever a
    // requested state matches its cache, so once the two disagree the client's draw asks for a depth write, gx sees a
    // match, skips the call, and the hardware keeps whatever the pass last set -- ZWRITEENABLE FALSE. The water then
    // writes no depth, the before and after copies are identical, coverage is zero everywhere and no water is shaded.
    // Every individual call succeeds, so nothing logs.
    //
    // Applying the captured block puts the hardware back. Marking the affected states dirty afterwards makes gx agree:
    // it forces the next RsSet for each of them to reach the driver even though the cached value looks unchanged.
    if (m_stateCaptured && m_state) {
        m_state->Apply();
    }

    g_theGxDevicePtr->IRsForceUpdate();
    ForceShaderConstantUpload();
}

void WaterRenderer::RestoreTargets(IDirect3DDevice9* device) {
    const bool captured = m_stateCaptured;
    m_stateCaptured = false;
    device->SetRenderTarget(0, m_saved.colour[0]);

    for (DWORD index = 1; index < kMaxRenderTargets; ++index) {
        device->SetRenderTarget(index, m_saved.colour[index]);
    }

    if (captured && m_state) {
        m_state->Apply();
    }

    ForceShaderConstantUpload();
    device->SetDepthStencilSurface(m_saved.depth);
    device->SetStreamSource(0, m_saved.stream, m_saved.streamOffset, m_saved.streamStride);
    ReleaseTargets();
}

void WaterRenderer::SetPassState(IDirect3DDevice9* device) {
    device->SetVertexShader(m_vs);
    device->SetVertexDeclaration(m_decl);
    device->SetStreamSourceFreq(0, 1);
    device->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
    device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    device->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
    device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    device->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    device->SetRenderState(D3DRS_TWOSIDEDSTENCILMODE, FALSE);
    device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    device->SetRenderState(D3DRS_COLORWRITEENABLE, kColourWriteAll);
    device->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
    device->SetRenderState(D3DRS_FOGENABLE, FALSE);
    device->SetRenderState(D3DRS_CLIPPLANEENABLE, 0);
    device->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
}

bool WaterRenderer::EnsureCopies(IDirect3DDevice9* device, IDirect3DSurface9* target, UINT width, UINT height) {
    // Probed once: whether this device can give a depth-stencil texture that also carries stencil bits, and whether
    // INTZ can be both a depth-stencil texture and a render target. Those two answers decide which of the two ways of
    // finding the water pixels is available at all, so they are measured rather than assumed.
    if (!m_loggedDepthFormats) {
        m_loggedDepthFormats = true;

        auto probe = [device](D3DFORMAT format, DWORD usage, D3DRESOURCETYPE type) {
            return SUCCEEDED(device->CreateTexture(64, 64, 1, usage, format, D3DPOOL_DEFAULT, nullptr, nullptr))
                       ? 1
                       : 0;
        };

        IDirect3DTexture9* intzDepth = nullptr;
        IDirect3DTexture9* intzTarget = nullptr;
        IDirect3DTexture9* d24s8 = nullptr;
        const bool intzAsDepth = SUCCEEDED(device->CreateTexture(
            64, 64, 1, D3DUSAGE_DEPTHSTENCIL, static_cast<D3DFORMAT>(MAKEFOURCC('I', 'N', 'T', 'Z')),
            D3DPOOL_DEFAULT, &intzDepth, nullptr));
        const bool intzAsTarget = SUCCEEDED(device->CreateTexture(
            64, 64, 1, D3DUSAGE_RENDERTARGET, static_cast<D3DFORMAT>(MAKEFOURCC('I', 'N', 'T', 'Z')),
            D3DPOOL_DEFAULT, &intzTarget, nullptr));
        const bool stencilDepth = SUCCEEDED(device->CreateTexture(64, 64, 1, D3DUSAGE_DEPTHSTENCIL, D3DFMT_D24S8,
                                                                 D3DPOOL_DEFAULT, &d24s8, nullptr));
        SafeRelease(intzDepth);
        SafeRelease(intzTarget);
        SafeRelease(d24s8);

        VF_LOG_INFO("modern water: depth formats intz-as-depth %d, intz-as-target %d, d24s8-depth %d, "
                    "rttFormat intz %d d24x8 %d",
                    intzAsDepth ? 1 : 0, intzAsTarget ? 1 : 0, stencilDepth ? 1 : 0,
                    g_theGxDevicePtr->Caps().m_rttFormat[GxTex_INTZ] ? 1 : 0,
                    g_theGxDevicePtr->Caps().m_rttFormat[GxTex_D24X8] ? 1 : 0);
        (void)probe;
    }

    if (m_sceneColour && m_copyW == width && m_copyH == height) {
        return true;
    }

    SafeRelease(m_sceneColour);
    SafeRelease(m_sceneDepthBefore);
    SafeRelease(m_waterDepthSurface);
    SafeRelease(m_waterDepth);
    SafeRelease(m_depthRaw);
    m_copyW = m_copyH = 0;

    if (m_copyFailed) {
        return Skip("water copies unavailable");
    }

    D3DSURFACE_DESC desc = {};
    target->GetDesc(&desc);

    const bool colour = CreateTarget(device, width, height, desc.Format, &m_sceneColour) ||
                        CreateTarget(device, width, height, D3DFMT_A8R8G8B8, &m_sceneColour);

    // Both depth copies are attempted unconditionally: evaluating them with || short-circuits, so a failed R32F
    // target would leave m_waterDepth null while m_packedDepth was already set, and every later bind of it would
    // silently target nothing.
    const bool r32fDepth = CreateTarget(device, width, height, D3DFMT_R32F, &m_sceneDepthBefore) &&
                           CreateTarget(device, width, height, D3DFMT_R32F, &m_waterDepth);
    m_packedDepth = !r32fDepth;
    bool depth = r32fDepth;

    if (!r32fDepth) {
        SafeRelease(m_sceneDepthBefore);
        SafeRelease(m_waterDepth);
        const bool packed = CreateTarget(device, width, height, D3DFMT_A8R8G8B8, &m_sceneDepthBefore);
        depth = CreateTarget(device, width, height, D3DFMT_A8R8G8B8, &m_waterDepth) && packed;
    }

    if (!colour || !depth) {
        SafeRelease(m_sceneColour);
        SafeRelease(m_sceneDepthBefore);
        SafeRelease(m_waterDepth);
        m_copyFailed = true;
        VF_LOG_ERROR("modern water: scene copies could not be created (%ux%u); the client's water stays", width,
                     height);
        return Skip("water copy creation failed");
    }

    // Optional: without it the water's depth simply stays in the shared buffer after the pass, as before.
    if (!m_packedDepth) {
        CreateTarget(device, width, height, D3DFMT_R32F, &m_depthRaw);
    }

    m_copyW = width;
    m_copyH = height;
    VF_LOG_INFO("modern water: scene copies %ux%u, depth %s", width, height,
                m_packedDepth ? "packed rgba8" : "r32f");
    return true;
}

bool WaterRenderer::CopySceneColour(IDirect3DDevice9* device, IDirect3DSurface9* target,
                                    const D3DVIEWPORT9& viewport, IDirect3DTexture9* destination) {
    const RECT world = ViewportRect(viewport);
    IDirect3DSurface9* copy = nullptr;
    const bool copied = SUCCEEDED(destination->GetSurfaceLevel(0, &copy)) &&
                        SUCCEEDED(device->StretchRect(target, &world, copy, nullptr, D3DTEXF_POINT));
    SafeRelease(copy);
    return copied;
}

// Linearises a depth texture into a sampleable R32F (or packed) copy. The depth shader reads the source as a
// texture, so the source has to be the pass's own INTZ depth target -- the client's D24X8 surface cannot be sampled
// by D3D9 at all.
void WaterRenderer::CopyDepthTargetLinear(IDirect3DDevice9* device, IDirect3DTexture9* source,
                                          IDirect3DTexture9* copy) {
    // A full-screen depth copy through the linearise shader. It sets everything it depends on, because it is called
    // from two places with different device state -- the before copy runs on the client's state, the after copy on the
    // pass's -- so it cannot inherit anything.
    //
    // Depth-stencil is unbound and depth test/write are off: the copy samples the very depth buffer that is bound, and
    // a full-screen triangle at z=0 would otherwise write 0 over the depth it is reading, destroying both the scene
    // depth and the image it is producing.
    SetTarget(device, copy);
    device->SetDepthStencilSurface(nullptr);
    SetPassState(device);
    device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    device->SetVertexDeclaration(m_decl);
    device->SetVertexShader(m_vs);
    device->SetPixelShader(m_packedDepth ? m_packedDepthCopyShader : m_depthCopyShader);
    device->SetPixelShaderConstantF(0, &m_common[0][0], kCommonConstants);
    BindPointSampler(device, 0, source, D3DTADDRESS_CLAMP);
    DrawFullscreenTriangle(device);
}

// The shared depth as stored, copied unchanged into an R32F target before the water draws.
void WaterRenderer::CopyDepthRaw(IDirect3DDevice9* device) {
    if (!m_depthRaw || !m_depthRawShader || !m_sceneDepthTexture) {
        return;
    }

    SetTarget(device, m_depthRaw);
    device->SetDepthStencilSurface(nullptr);
    SetPassState(device);
    device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    device->SetVertexDeclaration(m_decl);
    device->SetVertexShader(m_vs);
    device->SetPixelShader(m_depthRawShader);
    device->SetPixelShaderConstantF(0, &m_common[0][0], kCommonConstants);
    BindPointSampler(device, 0, m_sceneDepthTexture, D3DTADDRESS_CLAMP);
    DrawFullscreenTriangle(device);
}

// Writes the pre-water depth back into the shared depth. The pass makes the liquid draws write depth so it can find the
// water; left there, everything the client draws afterwards tests against the water's surface instead of the terrain
// under it. The splash and wake rings are decals on that terrain, so they were hidden wherever water lay over them.
// Depth only: colour writes are masked, and the viewport's depth range is the identity because a shader depth output is
// stored as is.
void WaterRenderer::RestoreSceneDepth(IDirect3DDevice9* device) {
    if (!m_depthRaw || !m_depthRestoreShader || !m_waterDepthSurface || !m_saved.colour[0]) {
        return;
    }

    DWORD zFunc = D3DCMP_LESSEQUAL;
    device->GetRenderState(D3DRS_ZFUNC, &zFunc);

    for (DWORD stage = 0; stage < kWaterSamplerStages; ++stage) {
        device->SetTexture(stage, nullptr);
    }

    device->SetRenderTarget(0, m_saved.colour[0]);
    device->SetDepthStencilSurface(m_waterDepthSurface);
    D3DVIEWPORT9 viewport = m_inputs.viewport;
    viewport.MinZ = 0.0f;
    viewport.MaxZ = 1.0f;
    device->SetViewport(&viewport);
    SetPassState(device);
    device->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
    device->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
    device->SetRenderState(D3DRS_ZFUNC, D3DCMP_ALWAYS);
    device->SetRenderState(D3DRS_COLORWRITEENABLE, 0);
    device->SetPixelShader(m_depthRestoreShader);
    device->SetPixelShaderConstantF(0, &m_common[0][0], kCommonConstants);
    BindPointSampler(device, 0, m_depthRaw, D3DTADDRESS_CLAMP);
    DrawFullscreenTriangle(device);

    device->SetTexture(0, nullptr);
    device->SetRenderState(D3DRS_ZFUNC, zFunc);
    device->SetRenderState(D3DRS_COLORWRITEENABLE, kColourWriteAll);
}

bool WaterRenderer::CopyFrameDepth(IDirect3DDevice9* device, const D3DVIEWPORT9& viewport) {
    // The scene's depth has to be known per pixel, and D3D9 cannot sample a D24X8 depth surface -- which is what the
    // client's default depth is. The frame's shared depth is INTZ instead (CUnitOutline::BeginDepthPass), and that
    // one is bound across the whole world draw, the liquid pass included, so it is already a texture here.
    //
    // This makes the pass work rather than stand down. The alternative -- the WXL module's -- was to wrap the device
    // and re-create the client's depth buffer in INTZ from device creation, because it could not change the client.
    // This tree does not need that: it only has to make sure the one shared depth is INTZ, which is a one-line change
    // in CUnitOutline and a new format in gx.
    (void)device;
    (void)viewport;

    CGxTex* sceneDepth = CUnitOutline::DepthPassTexture();

    if (!sceneDepth || !sceneDepth->m_apiSpecificData) {
        return false;
    }

    m_sceneDepthTexture = static_cast<IDirect3DTexture9*>(sceneDepth->m_apiSpecificData);
    return true;
}

bool WaterRenderer::BuildCommonConstants(const D3DSURFACE_DESC& depthDesc) {
    // The bars the shader reconstructs the pixel's world position from: the viewport, the projection's own terms,
    // and the inverse of the camera-relative view, which is what turns a view ray back into world space.
    float inverse[16] = {};

    if (!InvertAffineView(m_inputs.cameraRelativeView, inverse)) {
        return false;
    }

    // The camera position is added back here rather than in the shader, because the view matrix is
    // camera-relative and the reconstruction needs absolute world coordinates.
    inverse[12] += m_inputs.camPos[0];
    inverse[13] += m_inputs.camPos[1];
    inverse[14] += m_inputs.camPos[2];

    const D3DVIEWPORT9& viewport = m_inputs.viewport;
    const float* projection = m_inputs.glProjection;
    const WorldDepthMapping depth = MapWorldDepth(projection, viewport);
    const float width = static_cast<float>(viewport.Width);
    const float height = static_cast<float>(viewport.Height);
    const float rows[9][4] = {
        {static_cast<float>(viewport.X), static_cast<float>(viewport.Y), width, height},
        {1.0f, 0.0f, 1.0f / depthDesc.Width, 1.0f / depthDesc.Height},
        {projection[0], projection[5], projection[8], projection[9]},
        {depth.atInfinity, depth.perInverseViewDepth, kWaterMaxViewDepth, depth.deepest},
        {},
        {},
        {},
        {},
        {width, height, 1.0f / width, 1.0f / height},
    };

    std::memcpy(m_common, rows, sizeof(rows));
    std::memcpy(m_common[4], inverse, sizeof(inverse));

    // Reported once: these four numbers decide whether any pixel can be read as nearer than the far plane. A zero
    // perInverseViewDepth or a swapped atInfinity makes the shader's LinearDepth return 0 for real geometry, which
    // shows up as "the whole world is at the far plane" and cannot be told apart from an empty depth copy by eye.
    if (!m_loggedDepthMapping) {
        m_loggedDepthMapping = true;
        VF_LOG_INFO("modern water: depth mapping atInfinity %.9f, perInverseViewDepth %.9f, deepest %.9f, "
                    "proj10 %.9f, proj14 %.9f, minZ %.3f, maxZ %.3f",
                    depth.atInfinity, depth.perInverseViewDepth, depth.deepest, projection[10], projection[14],
                    viewport.MinZ, viewport.MaxZ);
    }
    return true;
}

bool WaterRenderer::UsableTargets(IDirect3DSurface9* depthSurface, const D3DVIEWPORT9& viewport,
                                  D3DSURFACE_DESC& depthDesc) {
    D3DSURFACE_DESC targetDesc = {};

    // The depth this pass reads is the frame's shared INTZ depth, which the outline binds across the world draw. It is
    // not the same surface as the client's own depth, so what has to be checked is that the shared one exists and
    // matches the colour target -- not that it is the one the device happens to have bound.
    CGxTex* sharedDepth = CUnitOutline::DepthPassTexture();

    if (!depthSurface || !sharedDepth || !CUnitOutline::IsDepthPassBound()) {
        return Skip("the frame is not sharing a depth");
    }

    if (!m_saved.colour[0]) {
        return Skip("no render target");
    }

    if (FAILED(m_saved.colour[0]->GetDesc(&targetDesc)) || FAILED(depthSurface->GetDesc(&depthDesc))) {
        return Skip("surface description failed");
    }

    if (targetDesc.Width != depthDesc.Width || targetDesc.Height != depthDesc.Height) {
        return Skip("render target and depth sizes differ");
    }

    if (viewport.Width < kMinWorldViewportSide || viewport.Height < kMinWorldViewportSide ||
        viewport.X + viewport.Width > depthDesc.Width || viewport.Y + viewport.Height > depthDesc.Height) {
        return Skip("world viewport outside the render target");
    }

    return true;
}

uint32_t WaterRenderer::DrawnTileMask() const {
    const WaterData& data = GlobalWaterData();
    uint32_t mask = 0;

    for (int index = 1; index < kWaterClassCount; ++index) {
        const WaterPreset* preset = m_draws[index] ? data.Preset(static_cast<WaterClass>(index)) : nullptr;

        if (!preset) {
            continue;
        }

        for (int32_t tile : preset->tiles) {
            if (tile >= 0 && tile < kWaterMaxTiles && static_cast<size_t>(tile) < data.Tiles().size()) {
                mask |= 1u << tile;
            }
        }
    }

    return mask;
}

bool WaterRenderer::PrepareWaves(IDirect3DDevice9* device) {
    m_waveTiles = DrawnTileMask();
    m_waveSettings = {};
    m_waveSettings.resolution = m_tuning.quality >= kHighQuality ? kFftResolution : kFftResolutionLow;
    m_waveSettings.referenceResolution = kFftReferenceResolution;
    m_waveSettings.windSpeed = m_tuning.wind;
    m_waveSettings.windDirection[0] = kWindDirection[0];
    m_waveSettings.windDirection[1] = kWindDirection[1];
    m_waveSettings.amplitudeScale = m_tuning.waves * m_tuning.waves;
    const bool attempted = m_waveTiles != 0 && m_tuning.waves > 0.0f;
    return attempted && m_fft.Prepare(device, m_waveSettings, GlobalWaterData().Tiles(), m_waveTiles);
}

bool WaterRenderer::SimulateWaves(IDirect3DDevice9* device, double seconds) {
    const double step =
        m_lastSeconds < 0.0 ? 0.0 : std::clamp(seconds - m_lastSeconds, 0.0, kMaxFoamStepSeconds);
    return m_fft.Run(device, m_waveSettings, GlobalWaterData().Tiles(), seconds, static_cast<float>(step));
}

bool WaterRenderer::SimulateRipples(IDirect3DDevice9* device, double seconds) {
    if (m_tuning.ripples <= 0.0f) {
        m_ripples.ReleaseAll();
        m_contacts.Reset();
        return false;
    }

    // A map change, a clock that went backwards and a long stall all mean the previous state is stale rather
    // than continuous, so the simulation restarts instead of integrating a discontinuity.
    if (m_lastRippleSeconds >= 0.0 &&
        (seconds < m_lastRippleSeconds || seconds - m_lastRippleSeconds > kRippleGapSeconds ||
         m_inputs.mapId != m_rippleMapId)) {
        m_ripples.Restart();
        m_contacts.Reset();
    }

    m_lastRippleSeconds = seconds;
    m_rippleMapId = m_inputs.mapId;
    m_contacts.Update(m_contactFrame, seconds);

    if (!m_ripples.Running() && !m_contacts.Emitting()) {
        return false;
    }

    if (!m_ripples.Prepare(device, m_tuning.quality == kLowQuality ? kWaterRippleTexelsLow : kWaterRippleTexels)) {
        if (!m_loggedRippleFailureShown) {
            VF_LOG_INFO("modern water: ripples skipped: %s", m_ripples.LastFailure());
            m_loggedRippleFailureShown = true;
        }

        return false;
    }

    m_loggedRippleFailureShown = false;

    if (!m_ripples.Running()) {
        m_contacts.PlaceFootprints(seconds);
    }

    const WaterRippleSchedule schedule = m_ripples.Schedule(seconds);
    const float centre[2] = {m_inputs.camTarget[0], m_inputs.camTarget[1]};

    for (int step = 0; step < schedule.steps; ++step) {
        WaterRippleDisturbance disturbances[kMaxWaterRippleDisturbances];
        const uint32_t count =
            m_contacts.TakeDisturbances(schedule.stepSeconds[step], disturbances, kMaxWaterRippleDisturbances);
        m_ripples.Step(device, centre, disturbances, count);
    }

    return m_ripples.Visible();
}

void WaterRenderer::FillRippleConstants(double seconds) {
    const WaterRippleWindow window = m_ripples.Window(seconds);
    const float extent = window.texels > 0 ? window.extent : 1.0f;
    const float texelUv = window.texels > 0 ? 1.0f / window.texels : 0.0f;
    m_rippleWindow = {window.origin[0], window.origin[1], 1.0f / extent, texelUv};
    m_rippleShape = {window.weight, m_ripplesShaded ? m_tuning.ripples : 0.0f, extent, 0.0f};
    m_rippleFade = {extent * (0.5f - kRippleEdgeRampPerExtent), 1.0f / kRippleFadeYards,
                    kWaterRippleTexelYards, 0.0f};
}

void WaterRenderer::FillClassConstants(ShadingConstants& constants, const WaterPreset& preset,
                                       WaterClass waterClass, double seconds) const {
    constants = {};
    const WaterData& data = GlobalWaterData();
    float toLight[3] = {m_inputs.toLight[0], m_inputs.toLight[1], m_inputs.toLight[2]};
    const float length = std::sqrt(toLight[0] * toLight[0] + toLight[1] * toLight[1] + toLight[2] * toLight[2]);

    for (float& axis : toLight) {
        axis = length > 1e-6f ? axis / length : 0.0f;
    }

    const float sunVisibility = SmoothStep(-kSunVisibilityHalfWidth, kSunVisibilityHalfWidth, toLight[2]);
    const float sunTransmission = 1.0f - FresnelSchlick(Saturate(toLight[2]));
    float sun[3];
    LinearColour(m_inputs.directColor, sun);
    const float moonScale = m_inputs.lightIsMoon ? 0.35f : 1.0f;

    for (float& channel : sun) {
        channel *= moonScale;
    }

    float sky[6][3];
    float ambient[3] = {};

    for (int band = 0; band < 6; ++band) {
        LinearColour(m_inputs.skyColors[band], sky[band]);

        for (int channel = 0; channel < 3; ++channel) {
            ambient[channel] += sky[band][channel] / 6.0f;
        }
    }

    constants.light = {toLight[0], toLight[1], toLight[2], sunVisibility};
    constants.sunColour = {sun[0], sun[1], sun[2], sunTransmission};
    constants.ambient = {ambient[0], ambient[1], ambient[2], 0.0f};
    constants.isotropicLight = {ambient[0] + sunVisibility * sun[0], ambient[1] + sunVisibility * sun[1],
                                ambient[2] + sunVisibility * sun[2], 0.0f};

    for (int band = 0; band < 5; ++band) {
        const float* colour = sky[kSkyColourOfBand[band]];
        constants.sky[band] = {colour[0], colour[1], colour[2], 0.0f};
    }

    if (m_inputs.stockFogApplies) {
        float fog[3];
        UnpackColour(m_inputs.fogColor, fog);
        constants.stockFogColour = {fog[0], fog[1], fog[2], 1.0f};
        constants.stockFog = {m_inputs.fogEnd,
                              1.0f / std::max(m_inputs.fogEnd - m_inputs.fogStart, kMinStockFogRange), 0.0f,
                              0.0f};
    }

    const float absorptionScale = preset.absorption[3] / std::max(m_tuning.clarity, kMinClarity);
    constants.absorption = {preset.absorption[0] * absorptionScale, preset.absorption[1] * absorptionScale,
                            preset.absorption[2] * absorptionScale, 0.0f};
    constants.scatteringIntensities = {preset.scatteringIntensities[0], preset.scatteringIntensities[1],
                                       preset.scatteringIntensities[2], preset.scatteringIntensities[3]};

    // The zone's own water colours pull the physical scattering towards what the map's light bands say the water
    // should look like. Magma has no zone colour: lava is the same underground as it is anywhere else.
    float closeWater[3] = {1.0f, 1.0f, 1.0f};
    float farWater[3] = {1.0f, 1.0f, 1.0f};

    if (waterClass != WaterClass::Magma) {
        LinearColour(m_inputs.riverColors[kCloseWaterColour], closeWater);
        LinearColour(m_inputs.riverColors[kFarWaterColour], farWater);
    }

    const float zoneWeight = waterClass == WaterClass::Magma ? 0.0f : Saturate(m_tuning.zoneColors);
    auto towardZone = [zoneWeight](const float* authored, const float* zoneColour, float weight) {
        return Float4{authored[0] + (zoneColour[0] - authored[0]) * zoneWeight,
                      authored[1] + (zoneColour[1] - authored[1]) * zoneWeight,
                      authored[2] + (zoneColour[2] - authored[2]) * zoneWeight, weight};
    };
    constants.scatteringTop = towardZone(preset.scatteringTop, closeWater, preset.scatteringTop[3]);
    constants.scatteringBottom = towardZone(preset.scatteringBottom, farWater, preset.scatteringBottom[3]);

    const float foam = std::max(m_tuning.foam, 0.0f);
    constants.depthFadeFoam = {std::max(preset.depthFadeFoam[0], 0.0f) * foam,
                               preset.depthFadeFoam[1] * kFoamTilingScale, preset.depthFadeFoam[2], 0.0f};
    constants.shoreFoam = {std::max(preset.shoreFoam[0], 0.0f) * foam, preset.shoreFoam[1] * kFoamTilingScale,
                           preset.shoreFoam[2], kShoreDistancePerDepth};
    constants.waveFoam = {std::max(preset.waveFoam[0], 0.0f) * foam, 0.0f, 0.0f, 0.0f};
    constants.waveFoamScaling = {preset.waveFoamScaling[0] * kFoamTilingScale,
                                 preset.waveFoamScaling[1] * kFoamTilingScale,
                                 preset.waveFoamScaling[2] * kFoamTilingScale, 0.0f};
    constants.surfaceResponse = {preset.roughness[0], preset.roughness[1],
                                 preset.roughness[2] * m_tuning.reflections, m_tuning.specular};

    // Surface optics. Water keeps the physical Fresnel of 0.02 and has no self-lit term, so its shading is byte
    // for byte what the module drew. Lava reflects essentially nothing: its F0 is zero, which removes both the
    // environment term and the sun's highlight at every angle, including grazing -- that is the difference
    // between "no reflections" and "dim reflections". Its emissive term then supplies the glow instead.
    if (waterClass == WaterClass::Magma) {
        constants.surfaceOptics = {0.0f, 1.0f, 0.0f, 0.0f};
    } else {
        constants.surfaceOptics = {kWaterF0, 0.0f, 0.0f, 0.0f};
    }

    float inverse[kWaterPresetTiles] = {};
    int tileCount = 0;

    for (int index = 0; index < kWaterPresetTiles; ++index) {
        const int32_t tile = preset.tiles[index];

        if (tile < 0 || static_cast<size_t>(tile) >= data.Tiles().size() ||
            !(data.Tiles()[tile].size > 0.0f)) {
            continue;
        }

        inverse[index] = 1.0f / data.Tiles()[tile].size;
        ++tileCount;
    }

    constants.inverseTileSizes = {inverse[0], inverse[1], inverse[2], inverse[3]};
    constants.waveControl = {1.0f / static_cast<float>(std::max(tileCount, 1)), 0.0f,
                             static_cast<float>(waterClass), TemporaryDebugView()};
    constants.foamScroll = {ScrollOffset(seconds, 0, preset.waveFoam[1]),
                            ScrollOffset(seconds, 1, preset.waveFoam[1]),
                            ScrollOffset(seconds, 0, preset.shoreFoam[3]),
                            ScrollOffset(seconds, 1, preset.shoreFoam[3])};
    constants.depthFoamScroll = {ScrollOffset(seconds, 0, preset.depthFadeFoam[3]),
                                 ScrollOffset(seconds, 1, preset.depthFadeFoam[3]), 0.0f, 0.0f};
    constants.reflectionFogRange = {0.0f, 0.0f, 0.0f, 0.0f};

    const float packedUnit = kWaterMaxViewDepth / kPackedDepthLevels * kByteMax;
    constants.depthDecode =
        m_packedDepth ? Float4{packedUnit * kHighByteWeight, packedUnit * kMidByteWeight, packedUnit, 0.0f}
                      : Float4{1.0f, 0.0f, 0.0f, 0.0f};
    constants.rippleWindow = m_rippleWindow;
    constants.rippleShape = m_rippleShape;
    constants.rippleFade = m_rippleFade;

    for (int slot = 0; slot < 5; ++slot) {
        const int32_t mask = preset.masks[slot];

        if (!MaskPresent(preset, slot)) {
            continue;
        }

        // The presence check above only proves a texture was uploaded; the index still has to be in range before it
        // is used, because a preset from a different data build could name a mask this file does not have.
        if (mask < 0 || static_cast<size_t>(mask) >= data.Masks().size()) {
            continue;
        }

        const WaterMask& info = data.Masks()[mask].info;
        float low[3];
        float high[3];
        ClampedTint(info.tintLow, low);
        ClampedTint(info.tintHigh, high);
        constants.maskTints[slot * 2] = {low[0], low[1], low[2], 1.0f};
        constants.maskTints[slot * 2 + 1] = {high[0], high[1], high[2], 0.0f};
    }
}

void WaterRenderer::BindClassTextures(IDirect3DDevice9* device, const WaterPreset& preset) {
    for (int index = 0; index < kWaterPresetTiles; ++index) {
        const int32_t tile = preset.tiles[index];
        IDirect3DTexture9* surface = m_wavesSimulated && tile >= 0 ? m_fft.Surface(tile) : nullptr;
        IDirect3DTexture9* foam = m_wavesSimulated && tile >= 0 ? m_fft.Foam(tile) : nullptr;
        BindSampler(device, kFirstSurfaceStage + index, surface ? surface : m_flat, D3DTADDRESS_WRAP,
                    D3DTEXF_LINEAR, D3DTEXF_LINEAR);
        BindSampler(device, kFirstFoamStateStage + index, foam ? foam : m_flat, D3DTADDRESS_WRAP,
                    D3DTEXF_LINEAR, D3DTEXF_NONE);
    }

    const FoamMaskTexture* wave = WaveFoamMasks(preset);
    IDirect3DTexture9* shore = MaskTexture(preset.masks[kShoreFoamSlot]);
    IDirect3DTexture9* depth = MaskTexture(preset.masks[kDepthFoamSlot]);
    BindSampler(device, kWaveFoamMaskStage, wave ? wave->texture : m_flat, D3DTADDRESS_WRAP, D3DTEXF_LINEAR,
                D3DTEXF_LINEAR);
    BindSampler(device, kShoreFoamMaskStage, shore ? shore : m_flat, D3DTADDRESS_WRAP, D3DTEXF_LINEAR,
                D3DTEXF_LINEAR);
    BindSampler(device, kDepthFoamMaskStage, depth ? depth : m_flat, D3DTADDRESS_WRAP, D3DTEXF_LINEAR,
                D3DTEXF_LINEAR);
}

void WaterRenderer::ShadeClass(IDirect3DDevice9* device, WaterClass waterClass, double seconds) {
    const D3DVIEWPORT9& viewport = m_inputs.viewport;
    const RECT world = ViewportRect(viewport);

    // Nothing here reads or writes depth: the coverage test is a depth comparison in the pixel shader. So no
    // depth-stencil surface is bound and no stencil state is set -- there is nothing to leak into the client's passes.
    device->SetRenderTarget(0, m_saved.colour[0]);
    device->SetDepthStencilSurface(nullptr);
    device->SetViewport(&viewport);
    device->SetScissorRect(&world);
    device->SetRenderState(D3DRS_SCISSORTESTENABLE, TRUE);
    device->SetRenderState(D3DRS_COLORWRITEENABLE, kColourWriteRgb);
    device->SetRenderState(D3DRS_STENCILENABLE, FALSE);

    // The shading triangle must not be depth tested at all: it is a fullscreen pass and would otherwise reject every
    // pixel behind a nearer water plane.
    device->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
    device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    m_shadingVariant = ShadingVariant(m_tuning);

    // ShadingVariant returns 0..2 by construction, but it comes from a config value, so the index is clamped rather
    // than trusted: an out-of-range index here would read a shader pointer from outside the array.
    m_shadingVariant = std::clamp(m_shadingVariant, 0, 2);
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    device->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
    device->SetPixelShader(m_shade[m_shadingVariant]);
    device->SetPixelShaderConstantF(0, &m_common[0][0], kCommonConstants);
    BindPointSampler(device, kSceneColourStage, m_sceneColour, D3DTADDRESS_MIRROR);
    // s1 is the scene as it was before the liquid pass -- the terrain or object behind the water, which is what the
    // shader refracts and measures the water column against. s2 is the same buffer after the water drew, so on a
    // water pixel it holds the water's own surface; comparing the two is what finds the water.
    BindPointSampler(device, kSceneDepthStage, m_sceneDepthBefore, D3DTADDRESS_MIRROR);
    BindPointSampler(device, kWaterDepthStage, m_waterDepth, D3DTADDRESS_CLAMP);
    BindSampler(device, kRippleStage, m_ripplesShaded ? m_ripples.Map() : m_flat, D3DTADDRESS_CLAMP,
                D3DTEXF_LINEAR, D3DTEXF_NONE);

    const WaterData& data = GlobalWaterData();
    const WaterPreset magma = MagmaPreset();
    static_assert(sizeof(ShadingConstants) % sizeof(Float4) == 0, "water constants are whole registers");

    // One class only. The caller shades each class straight after its own draws, so "the water in the frame since the
    // baseline" is this class and nothing else -- which is why no per-pixel class mask is needed.
    const WaterPreset* preset = waterClass == WaterClass::Magma ? &magma : data.Preset(waterClass);

    if (!preset) {
        return;
    }

    ShadingConstants constants;
    FillClassConstants(constants, *preset, waterClass, seconds);
    device->SetPixelShaderConstantF(kShadingFirstConstant, &constants.light.x,
                                    sizeof(constants) / sizeof(Float4));
    BindClassTextures(device, *preset);
    DrawFullscreenTriangle(device);
    ++m_shadedClasses;
    m_shadedClassMask |= 1u << static_cast<int>(waterClass);
}

void WaterRenderer::ShadeRun(IDirect3DDevice9* device, WaterClass waterClass) {
    if (!device || !m_armed || device->TestCooperativeLevel() != D3D_OK || !m_sceneColour) {
        return;
    }

    const int classIndex = static_cast<int>(waterClass);

    if (classIndex <= 0 || classIndex >= kWaterClassCount || !m_draws[classIndex]) {
        return;
    }

    const double seconds = WaterSeconds();

    // Reported once per class, which is the smallest thing that answers "is the water being shaded at all". If nothing
    // appears here the pass never reached shading and the reason is the last "pass inactive" line.
    if (!m_loggedFirstShade) {
        m_loggedFirstShade = true;
        VF_LOG_INFO("modern water: shading %s", WaterClassLabel(waterClass));
    }

    // The waves and ripples advance once per frame, not once per class: the first run of the frame pays for them and
    // the rest just shade.
    if (!m_wavesSimulated) {
        // The wave and ripple passes set no render state of their own: they draw fullscreen triangles into float
        // textures and rely on the caller having put the device in the pass state first. This point is still in the
        // middle of the client's liquid draws, so the device holds the client's state -- depth test against the frame's
        // depth, alpha blending, its colour mask. Left like that the passes depth-test and blend their output into the
        // wave textures, which comes out as garbage the shader reads as saturated normals.
        device->SetDepthStencilSurface(nullptr);
        SetPassState(device);

        const double frameStep = m_lastSeconds < 0.0 ? 0.0 : seconds - m_lastSeconds;
        const bool waveDue = !m_wavesValid || m_lastWaveSeconds < 0.0 ||
                             seconds - m_lastWaveSeconds + frameStep >= kWaveUpdateSeconds;

        if (waveDue && PrepareWaves(device) && SimulateWaves(device, seconds)) {
            m_lastSeconds = seconds;
            m_lastWaveSeconds = seconds;
            m_wavesValid = true;
        }

        m_wavesSimulated = m_wavesValid;
        m_ripplesShaded = SimulateRipples(device, seconds);
        FillRippleConstants(seconds);
    }

    // The frame's depth as it stands now: the water of this run has already drawn into it, so on those pixels it holds
    // the water surface, and everywhere else it holds whatever was there before. Comparing it with the baseline is what
    // finds this run's pixels.
    {
        D3DVIEWPORT9 copyViewport = {0, 0, m_inputs.viewport.Width, m_inputs.viewport.Height, 0.0f, 1.0f};
        SetTarget(device, m_waterDepth);
        device->SetViewport(&copyViewport);
        device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
        CopyDepthTargetLinear(device, m_sceneDepthTexture, m_waterDepth);
    }

    ShadeClass(device, waterClass, seconds);

    // This run is resolved, so its pixels become the baseline: the next run's water is measured against a depth that
    // already contains this run's surface, and cannot re-find them.
    {
        D3DVIEWPORT9 copyViewport = {0, 0, m_inputs.viewport.Width, m_inputs.viewport.Height, 0.0f, 1.0f};
        SetTarget(device, m_sceneDepthBefore);
        device->SetViewport(&copyViewport);
        device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
        CopyDepthTargetLinear(device, m_sceneDepthTexture, m_sceneDepthBefore);
    }

    for (int index = 1; index < kWaterClassCount; ++index) {
        m_draws[index] = 0;
    }

    // Hand the frame back. This runs in the middle of the client's liquid draw loop, so the next instance's draw has to
    // find the frame's colour target, the shared depth and -- critically -- the client's own render states, not the
    // ones this pass left behind. Restoring the depth write is what lets the next water draw actually land in the
    // depth the coverage test compares.
    RestoreClientState(device);
    device->SetRenderTarget(0, m_saved.colour[0]);
    device->SetDepthStencilSurface(m_waterDepthSurface);
    device->SetViewport(&m_inputs.viewport);
}

bool WaterRenderer::Begin(IDirect3DDevice9* device, const void* liquidRenderer, const WaterFrameInputs& clientInputs,
                          const WaterTimeInputs& tuning) {
    (void)liquidRenderer;
    m_skip = "";

    // The depth range the client's viewport maps the world into. The world is not drawn into [0,1]: the depth this pass
    // decodes was written through the device's real viewport, so decoding it against an assumed 0..1 range compresses
    // every terrain distance to a few yards and puts the water in the wrong place. Read from the device, not assumed.
    WaterFrameInputs inputs = clientInputs;
    D3DVIEWPORT9 liveViewport = {};

    if (device && SUCCEEDED(device->GetViewport(&liveViewport))) {
        inputs.viewport.MinZ = liveViewport.MinZ;
        inputs.viewport.MaxZ = liveViewport.MaxZ;
    }

    if (m_armed) {
        Abort(device);
    }

    m_tuning = tuning;

    if (!m_active) {
        return Skip("water disabled");
    }

    if (inputs.inLiquid) {
        return Skip("camera under water");
    }

    if (!Prepare(device, tuning)) {
        return false;
    }

    // Captured before a single state is touched. The block records the device exactly as the client left it, so End and
    // Abort can put it back; capturing later would record the pass's own copy-pass state and hand that back instead,
    // which also desynchronises gx's render-state cache from the hardware.
    SaveTargets(device);
    CaptureClientState();

    // The description has to come from the depth the pass actually samples; the client's own depth is a different
    // surface and, on a driving device, a different format.
    IDirect3DSurface9* sharedDepthSurface = nullptr;
    CGxTex* sharedDepthTex = CUnitOutline::DepthPassTexture();

    if (sharedDepthTex && sharedDepthTex->m_apiSpecificData) {
        static_cast<IDirect3DTexture9*>(sharedDepthTex->m_apiSpecificData)->GetSurfaceLevel(0, &sharedDepthSurface);
    }

    const bool targetsUsable = UsableTargets(sharedDepthSurface, inputs.viewport, m_depthDesc);

    if (sharedDepthSurface) {
        sharedDepthSurface->Release();
    }

    if (!targetsUsable) {
        ReleaseTargets();
        return false;
    }

    if (!EnsureCopies(device, m_saved.colour[0], inputs.viewport.Width, inputs.viewport.Height)) {
        ReleaseTargets();
        return false;
    }

    m_inputs = inputs;

    if (!BuildCommonConstants(m_depthDesc)) {
        ReleaseTargets();
        return Skip("view matrix not invertible");
    }

    EnsureMasks(device);

    // The frame as it stands -- the sky and every opaque thing, but not the water, which has not drawn yet -- is
    // copied whole. This is the image the shader refracts through, and because the water is not in it the sample is
    // exactly "what is behind this water pixel".
    const bool colourCopied = CopySceneColour(device, m_saved.colour[0], inputs.viewport, m_sceneColour);

    // The frame's own depth, for the shader's refraction and for coverage: how far each pixel's scene is. The frame's
    // shared depth is INTZ (see CUnitOutline::BeginDepthPass), which is bound across the whole world draw including
    // the liquid pass, so it can be sampled here directly -- no copy, no second depth buffer.
    const bool depthCopied = colourCopied && CopyFrameDepth(device, inputs.viewport);

    if (!colourCopied || !depthCopied) {
        RestoreTargets(device);
        return Skip(colourCopied ? "scene depth copy failed" : "scene colour copy failed");
    }

    // The scene depth *before* the water draws, linearised. End copies it again afterwards; the difference between
    // the two is the water's coverage, which is what lets the shading pass exist without a stencil.
    {
        D3DVIEWPORT9 copyViewport = {0, 0, inputs.viewport.Width, inputs.viewport.Height, 0.0f, 1.0f};
        device->SetViewport(&copyViewport);
        device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
        CopyDepthTargetLinear(device, m_sceneDepthTexture, m_sceneDepthBefore);
        CopyDepthRaw(device);
    }

    // The water draws now, into the frame's colour and the frame's *shared INTZ* depth -- not the client's own depth.
    //
    // This is the one binding the whole coverage scheme depends on. The shading pass finds the water by comparing two
    // copies of the shared INTZ depth, so the water's draws have to write into exactly that surface. Binding the
    // client's own depth instead (D24X8) writes the water's depth somewhere the pass never reads, the two copies come
    // out identical, coverage is zero on every pixel, and the water silently disappears.
    // Bound fresh each frame: the shared depth is a gx texture that a device reset re-creates, so the surface the
    // last frame held may be stale.
    SafeRelease(m_waterDepthSurface);

    if (!m_sceneDepthTexture || FAILED(m_sceneDepthTexture->GetSurfaceLevel(0, &m_waterDepthSurface)) ||
        !m_waterDepthSurface) {
        RestoreTargets(device);
        return Skip("the shared INTZ depth has no surface to draw into");
    }

    // The shared depth is about to become the depth-stencil target, but the depth-copy pass just left it bound as a
    // pixel-shader texture. D3D9 silently discards depth writes to a surface that is also bound for sampling -- no
    // error, no warning -- so the water would write nothing, the after copy would equal the before copy, coverage
    // would be zero on every pixel and the water would never appear. Unbind it from every sampler stage first.
    for (DWORD stage = 0; stage < kWaterSamplerStages; ++stage) {
        device->SetTexture(stage, nullptr);
    }

    // The client's render states go back before the water draws, so the liquid material's own RsSet calls land on a
    // device that matches gx's cache. Without this the material asks gx for a depth write, gx sees its cache already
    // agreeing, skips the call, and the hardware stays on this pass's ZWRITEENABLE FALSE -- so the water writes no
    // depth and coverage stays zero.
    RestoreClientState(device);

    device->SetRenderTarget(0, m_saved.colour[0]);
    device->SetDepthStencilSurface(m_waterDepthSurface);
    device->SetViewport(&inputs.viewport);

    // The stencil is not usable for this: the client's depth surface is D24X8 and the sampleable INTZ copy carries no
    // stencil bits either, so a tag written here would land in a surface that has none. Coverage is found by comparing
    // depth instead -- see ShadeClass.
    device->SetRenderState(D3DRS_STENCILENABLE, FALSE);

    m_armed = true;
    m_shadedClasses = 0;
    m_shadedClassMask = 0;
    std::fill(std::begin(m_draws), std::end(m_draws), 0u);
    return true;
}

void WaterRenderer::Tag(IDirect3DDevice9* device, WaterClass waterClass) {
    (void)device;

    // Only a tally: the class is recorded so the shading pass does not bother with a preset nothing drew. The pixels
    // themselves are found by the depth difference, not by anything written during the draw -- the client's depth
    // surface is D24X8 and carries no stencil to tag with.
    const int index = static_cast<int>(waterClass);

    if (!m_armed || index <= 0 || index >= kWaterClassCount) {
        return;
    }

    ++m_draws[index];
}

void WaterRenderer::End(IDirect3DDevice9* device) {
    m_wavesSimulated = false;
    m_ripplesShaded = false;

    if (!m_armed) {
        return;
    }
    // Nothing to unwrite: the water drew into the frame's own colour and depth, so there is no tag to take back off and
    // no stencil state to restore. Each class was already shaded as its own run of draws finished (ShadeRun), so there
    // is nothing left to shade here.
    m_armed = false;

    // NOT IN BINARY behaviour change: the shared depth is no longer put back as it was before the water wrote into it. On
    // water pixels it keeps the water's surface, which is what the depth fog needs to fog the surface at its own distance
    // (with the sea bed's depth there, the fog was worked out for the ground under the water, and read as fog beneath it).
    // The restore existed for the client's splash and wake rings, which this pass already suppresses, and for the models
    // cut by the liquid plane, whose parts drawn after the pass are above the surface and so still pass its depth test.
    // RestoreSceneDepth(device);

    // Everything the client had before Begin is handed back: the frame's colour targets, its depth surface and every
    // render state the shading pass touched.
    //
    // The expected target is kept before the restore, because RestoreTargets releases the saved surfaces and would
    // otherwise leave nothing to compare against -- which made the diagnostic below always report a mismatch.
    IDirect3DSurface9* expected = m_saved.colour[0];

    if (expected) {
        expected->AddRef();
    }

    if (m_stateCaptured) {
        RestoreTargets(device);
    } else {
        ReleaseTargets();
    }

    // Reported once. A viewport or target left small by one of the simulation passes is what puts a blocky rectangle in
    // the corner of the frame, and only the device knows whether that happened -- the code looks correct either way.
    if (!m_loggedEndState) {
        m_loggedEndState = true;
        D3DVIEWPORT9 viewport = {};
        IDirect3DSurface9* target = nullptr;
        DWORD scissor = 0;
        DWORD zEnable = 0;
        DWORD colourWrite = 0;
        device->GetViewport(&viewport);
        device->GetRenderTarget(0, &target);
        device->GetRenderState(D3DRS_SCISSORTESTENABLE, &scissor);
        device->GetRenderState(D3DRS_ZENABLE, &zEnable);
        device->GetRenderState(D3DRS_COLORWRITEENABLE, &colourWrite);
        VF_LOG_INFO("modern water: at end viewport %ux%u at %u,%u, scissor %u, zenable %u, colourwrite 0x%X, "
                    "target matches frame %d",
                    static_cast<unsigned>(viewport.Width), static_cast<unsigned>(viewport.Height),
                    static_cast<unsigned>(viewport.X), static_cast<unsigned>(viewport.Y),
                    static_cast<unsigned>(scissor), static_cast<unsigned>(zEnable),
                    static_cast<unsigned>(colourWrite), target && target == expected ? 1 : 0);

        if (target) {
            target->Release();
        }
    }

    if (expected) {
        expected->Release();
    }
}

void WaterRenderer::Abort(IDirect3DDevice9* device) {
    if (device && m_stateCaptured) {
        RestoreTargets(device);
    } else {
        ReleaseTargets();
    }

    m_armed = false;
}
