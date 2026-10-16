// Not in the original binary. Ported from 15595 (Cataclysm) - core DepthCache only. The DepthRange debug
// dump and its cvars are omitted; see DepthCache.h for why (needs 15595's per-permutation CGxShader).

#include <cstdint>
#include "Engine/Source/ShaderEffect/DepthCache.h"
#include "Engine/Source/Gx/CGxDevice/CGxDevice.h"
#include "Engine/Source/Gx/CGxTexFlags.h"
#include "Engine/Source/Gx/Gx.h"
#include "Engine/Source/Services/Texture.h"
#include "Engine/Source/Tempest/CRect.h"

// STATIC: 0x00F803D4/0x140E78258
static CTexture* s_depthResolveTex;

// STATIC: 0x00F803D8/0x140E78260
static CTexture* s_depthTex;

// STATIC: 0x00F803DC/0x140E78268
static int32_t s_refCount;

// STATIC: 0x00F803E0/0x140E7826C
static DepthCache::EDepthMode s_depthMode;

// STATIC: 0x00F803E4/0x140E78270
static int32_t s_needsCreate;

// OFFSET: 0x00A2F800/0x1407D94E0
void __fastcall DepthCache::Reset() {
    if (s_depthResolveTex) {
        TextureRelease(s_depthResolveTex);
        s_depthResolveTex = nullptr;
    }

    if (s_depthTex) {
        TextureRelease(s_depthTex);
        s_depthTex = nullptr;
    }

    s_depthMode = DM_Unknown;
    s_needsCreate = 1;
}

// OFFSET: 0x00A2F850/0x1407D9540
bool DepthCache::IsValid() {
    return s_depthMode != DM_Unknown;
}

// OFFSET: 0x00A2F860/0x1407D9550
CTexture* DepthCache::GetDepthTex() {
    return s_depthTex;
}

// OFFSET: 0x00A2F870/0x1407D9560
CTexture* DepthCache::GetDepthSR() {
    return s_depthResolveTex ? s_depthResolveTex : s_depthTex;
}

// OFFSET: 0x00A2F880/0x1407D9580
DepthCache::EDepthMode DepthCache::GetDepthMode() {
    return s_depthMode;
}

// OFFSET: 0x00A2F8C0/0x1407D95C0
int32_t DepthCache::IsSupported(EDepthMode* mode, EGxTexFormat* format, EGxTexFormat* resolveFormat) {
    EGxTexFormat resolve = GxTex_Unknown;
    EDepthMode depthMode = DM_Unknown;
    EGxTexFormat depth = GxTex_Unknown;

    if (g_theGxDevicePtr->m_caps.m_texNonPow2 || !g_theGxDevicePtr->m_caps.unk_0300) {
        return 0;
    }

    if (g_theGxDevicePtr->DeviceApi() == GxApi_D3d11) {
        switch (g_theGxDevicePtr->DeviceFormat().m_depthFormat) {
            case CGxFormat::Fmt_Ds160:
                depth = GxTex_R16;
                break;

            case CGxFormat::Fmt_Ds24X:
            case CGxFormat::Fmt_Ds248:
                depth = GxTex_R24X8;
                break;

            case CGxFormat::Fmt_Ds320:
                depth = GxTex_R32F;
                break;

            default:
                break;
        }

        depthMode = DM_Texture;

        if (g_theGxDevicePtr->DeviceFormat().m_multisampleCount > 1) {
            switch (g_theGxDevicePtr->DeviceFormat().m_depthFormat) {
                case CGxFormat::Fmt_Ds160:
                    resolve = GxTex_R16;
                    break;

                case CGxFormat::Fmt_Ds24X:
                case CGxFormat::Fmt_Ds248:
                case CGxFormat::Fmt_Ds320:
                    resolve = GxTex_R32F;
                    break;

                default:
                    break;
            }
        }
    } else if (g_theGxDevicePtr->m_caps.m_texFmt[GxTex_Intz]) {
        depth = GxTex_Intz;
        depthMode = DM_Texture;
    } else if (g_theGxDevicePtr->m_caps.m_texFmt[GxTex_AtiD24]) {
        depth = GxTex_AtiD24;
        depthMode = DM_Texture;
    }

    if (mode) {
        *mode = depthMode;
    }

    if (format) {
        *format = depth;
    }

    if (resolveFormat) {
        *resolveFormat = resolve;
    }

    return depthMode != DM_Unknown;
}

// OFFSET: 0x00A2F9D0/0x1407D9700
static void DestroyTextures() {
    g_theGxDevicePtr->RemoveDeviceRestoredCallback(DepthCache::Reset);
    g_theGxDevicePtr->RemoveTextureRecreationCallback(DepthCache::Reset);

    if (s_depthResolveTex) {
        TextureRelease(s_depthResolveTex);
        s_depthResolveTex = nullptr;
    }

    if (s_depthTex) {
        TextureRelease(s_depthTex);
        s_depthTex = nullptr;
    }

    s_depthMode = DepthCache::DM_Unknown;
}

// OFFSET: 0x00A2FA40/0x1407D9780
void DepthCache::Release() {
    if (!--s_refCount) {
        DestroyTextures();
    }
}

// OFFSET: 0x00A2FA50/0x1407D97A0
// NOTE: the call passes the depth texture first and the resolve texture second, and 15595's D3D11
// TexResolve (the only implementation) treats the first argument as the SOURCE and the second as the dest -
// its header calls them (dest, source), which is backwards. Kept verbatim so the port stays comparable.
void DepthCache::Resolve() {
    CGxTex* depth = TextureGetGxTex(s_depthTex, 0, nullptr);

    if (depth) {
        g_theGxDevicePtr->DeviceResolveDepthBuffer(depth);
    }

    if (s_depthResolveTex) {
        GxTexResolve(TextureGetGxTex(s_depthTex, 0, nullptr), TextureGetGxTex(s_depthResolveTex, 0, nullptr));
    }
}

// OFFSET: 0x00A2FAA0/0x1407D9810
static void CreateTextures() {
    // 12340's CRect has no constructor, only the four public floats (minY, minX, maxY, maxX); all zero here.
    CRect rect = { 0.0f, 0.0f, 0.0f, 0.0f };
    g_theGxDevicePtr->CapsWindowSize(rect);

    GxLog("Preparing to create sample-able depth buffer texture:");

    EGxTexFormat format = GxTex_Unknown;
    EGxTexFormat resolveFormat = GxTex_Unknown;
    DepthCache::EDepthMode mode = DepthCache::DM_Unknown;
    s_depthMode = DepthCache::DM_Unknown;

    if (DepthCache::IsSupported(&mode, &format, &resolveFormat) && (g_theGxDevicePtr->m_caps.unk_0314 || g_theGxDevicePtr->DeviceFormat().m_multisampleCount == 1)) {
        // DIAG: the whole point of Stage B. If this says "NOT supported" the water shaders get no depth
        // input, so log the two caps that decide it (both 0 under DXVK until the D3D11 backend exists).
        GxLog("    DepthCache is supported! (api=%d unk_0300=%d unk_0314=%d msaa=%u depthFmt=%d)",
              g_theGxDevicePtr->DeviceApi(),
              g_theGxDevicePtr->m_caps.unk_0300,
              g_theGxDevicePtr->m_caps.unk_0314,
              g_theGxDevicePtr->DeviceFormat().m_multisampleCount,
              g_theGxDevicePtr->DeviceFormat().m_depthFormat);
        GxLog("    Format:        %s", CGxDevice::s_texFormatNames[format]);
        GxLog("    FormatResolve: %s", CGxDevice::s_texFormatNames[resolveFormat]);

        CGxTexFlags flags;

        if (g_theGxDevicePtr->DeviceApi() != GxApi_D3d11) {
            flags.m_renderTarget = 1;
        }

        if (format) {
            flags.m_filter = 0;
            flags.m_bit8 = resolveFormat != GxTex_Unknown;
            s_depthTex = TextureCreate(GxTex_NonPow2, static_cast<uint32_t>(rect.maxX), static_cast<uint32_t>(rect.maxY), 1, format, format, flags, nullptr, GxTexNullUserFunc, "DepthCache", 0);
        }

        if (resolveFormat) {
            flags.m_renderTarget = 1;
            flags.m_bit8 = 0;
            s_depthResolveTex = TextureCreate(GxTex_NonPow2, static_cast<uint32_t>(rect.maxX), static_cast<uint32_t>(rect.maxY), 1, resolveFormat, resolveFormat, flags, nullptr, GxTexNullUserFunc, "DepthCache Resolve", 0);
        }

        s_needsCreate = 0;
        s_depthMode = mode;
    } else {
        // DIAG: the reason Stage B is inert on DXVK today. Distinguish the four inputs rather than guessing.
        GxLog("    DepthCache is NOT supported! (api=%d unk_0300=%d unk_0314=%d texNonPow2=%d msaa=%u depthFmt=%d intz=%d atiD24=%d)",
              g_theGxDevicePtr->DeviceApi(),
              g_theGxDevicePtr->m_caps.unk_0300,
              g_theGxDevicePtr->m_caps.unk_0314,
              g_theGxDevicePtr->m_caps.m_texNonPow2,
              g_theGxDevicePtr->DeviceFormat().m_multisampleCount,
              g_theGxDevicePtr->DeviceFormat().m_depthFormat,
              g_theGxDevicePtr->m_caps.m_texFmt[GxTex_Intz],
              g_theGxDevicePtr->m_caps.m_texFmt[GxTex_AtiD24]);
        s_needsCreate = 0;
    }
}

// OFFSET: 0x00A2FCA0/0x1407D9A60
void DepthCache::AddRef() {
    if (s_refCount++) {
        return;
    }

    CreateTextures();
    g_theGxDevicePtr->AddDeviceRestoredCallback(DepthCache::Reset);
    g_theGxDevicePtr->AddTextureRecreationCallback(DepthCache::Reset);
}

// OFFSET: 0x00A2FCE0/0x1407D9AB0
void DepthCache::Validate() {
    if (s_needsCreate && s_refCount) {
        CreateTextures();
    }
}