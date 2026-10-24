#include "WoW/Source/WorldClient/SkyCapture.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "Engine/Source/Base/Handle.h"
#include "Engine/Source/Gx/CGxDevice/CGxDevice.h"
#include "Engine/Source/Gx/CGxMatrixStack.h"
#include "Engine/Source/Gx/CGxTex.h"
#include "Engine/Source/Gx/CGxTexFlags.h"
#include "Engine/Source/Gx/Gx.h"
#include "Engine/Source/Gxu/Gxu.h"
#include "Engine/Source/Os/TimeManager.h"
#include "Engine/Source/Services/Texture.h"
#include "Engine/Source/Tempest/C3Vector.h"
#include "Engine/Source/Tempest/C44Matrix.h"
#include "Engine/Source/Tempest/CImVector.h"
#include "WoW/Source/UIUtil/Camera.h"
#include "WoW/Source/Ui/WorldFrame.h"
#include "WoW/Source/WorldClient/CPortalView.h"
#include "WoW/Source/WorldClient/DayNight.h"
#include "WoW/Source/WorldClient/DayNight__DNInfo.h"

namespace SkyCapture {

namespace {

constexpr int kTargetWidth = kTileCount * kTileWidth;
constexpr int kTargetHeight = kTileHeight;
// The sky drifts over minutes, so a redraw every dozen frames is invisible and costs four small sky draws.
constexpr uint32_t kRefreshFrames = 12;
constexpr float kPitchRadians = 0.78539816f;
constexpr float kFovRadians = 1.57079633f;
constexpr float kPi = 3.14159265f;

CTexture* s_texture = nullptr;
bool s_failed = false;
bool s_valid = false;
uint32_t s_frame = 0;
uint32_t s_lastCallMs = 0;
// The capture is only trusted while Update keeps being called, i.e. while the world is drawing its sky. Indoors the
// call stops, and the last picture must not outlive it.
constexpr uint32_t kStaleMs = 250;

CGxTex* GxTexture() {
    return s_texture ? TextureGetGxTex(s_texture, 1, nullptr) : nullptr;
}

bool EnsureTexture() {
    if (s_texture) {
        return GxTexture() != nullptr;
    }

    if (s_failed) {
        return false;
    }

    EGxTexFormat format;

    if (g_theGxDevicePtr->Caps().m_rttFormat[GxTex_Argb8888]) {
        format = GxTex_Argb8888;
    } else if (g_theGxDevicePtr->Caps().m_rttFormat[GxTex_Abgr8888]) {
        format = GxTex_Abgr8888;
    } else {
        s_failed = true;
        return false;
    }

    CGxTexFlags flags(GxTex_Linear, 0, 0, 0, 0, 1, 1, 0, 0, 0);
    s_texture = TextureCreate(GxTex_2d, kTargetWidth, kTargetHeight, 0, format, format, flags, nullptr, GxTexNullUserFunc, "SkyCapture::s_texture", 0);

    if (!s_texture || !GxTexture()) {
        s_failed = true;
        return false;
    }

    return true;
}

// Puts the colour and depth targets back the way Update found them. The colour is released first because a null depth
// asked for while a colour texture is bound dereferences that texture; with the back buffer current the depth can be
// named safely, and the colour is bound again last so the device derives the depth that pairs with it.
void RestoreTargets(CGxTex* colour, CGxTex* depth) {
    GxRenderTargetSet(GxBuffers_Color, nullptr, 0);
    GxRenderTargetSet(GxBuffers_Depth, depth, 0);

    if (colour) {
        GxRenderTargetSet(GxBuffers_Color, colour, 0);
    }
}

}  // namespace

CGxTex* Texture() {
    return s_valid && OsGetAsyncTimeMs() - s_lastCallMs < kStaleMs ? GxTexture() : nullptr;
}

void Update(CPortalView* view) {
    (void)view;

    if (!g_theGxDevicePtr) {
        s_valid = false;
        return;
    }

    DayNight::DNInfo& info = DayNight::g_dnInfo;
    s_lastCallMs = OsGetAsyncTimeMs();

    if (!info.m_showSky) {
        s_valid = false;
        return;
    }

    if (s_valid && (++s_frame % kRefreshFrames) != 0) {
        return;
    }

    if (!EnsureTexture()) {
        s_valid = false;
        return;
    }

    CGCamera* camera = CGWorldFrame::GetActiveCamera();

    if (!camera) {
        return;
    }

    CGxTex* target = GxTexture();

    float oldMinX, oldMaxX, oldMinY, oldMaxY, oldMinZ, oldMaxZ;
    GxXformViewport(oldMinX, oldMaxX, oldMinY, oldMaxY, oldMinZ, oldMaxZ);

    C44Matrix oldView;
    GxXformView(oldView);
    C44Matrix oldProjection = g_theGxDevicePtr->m_projection;

    CGxTex* oldColour = nullptr;
    CGxTex* oldDepth = nullptr;
    g_theGxDevicePtr->DeviceGetRenderTarget(GxBuffers_Color, oldColour);
    g_theGxDevicePtr->DeviceGetRenderTarget(GxBuffers_Depth, oldDepth);

    // Free the depth so the device builds one that pairs with this target's size, then bind the target itself.
    GxRenderTargetSet(GxBuffers_Color, nullptr, 0);
    GxRenderTargetSet(GxBuffers_Depth, nullptr, 0);
    GxRenderTargetSet(GxBuffers_Color, target, 0);

    g_theGxDevicePtr->RsPush();
    GxXformSetViewport(0.0f, 1.0f, 0.0f, 1.0f, oldMinZ, oldMaxZ);
    g_theGxDevicePtr->SceneClear(GxBufferBit_Color | GxBufferBit_Depth, CImVector(0, 0, 0, 0));

    // Each strip is a plain perspective view: vertical field of view 90 degrees, as wide as its pixel aspect makes it.
    C44Matrix projection;
    const float aspect = static_cast<float>(kTileWidth) / static_cast<float>(kTileHeight);
    GxuXformCreateProjection_Exact(kFovRadians, aspect, camera->m_nearZ, camera->m_farZ, projection);
    g_theGxDevicePtr->XformSetProjection(projection);

    CPortalView full(CRect{0.0f, 0.0f, 1.0f, 1.0f}, 0.0f);

    for (int tile = 0; tile < kTileCount; ++tile) {
        const float azimuth = static_cast<float>(tile) * (kPi * 0.5f);
        const C3Vector eye(0.0f, 0.0f, 0.0f);
        const C3Vector forward(std::cos(azimuth) * std::cos(kPitchRadians), std::sin(azimuth) * std::cos(kPitchRadians), std::sin(kPitchRadians));
        const C3Vector up(0.0f, 0.0f, 1.0f);
        C44Matrix view;
        GxuXformCreateLookAtSgCompat(eye, forward, up, view);
        g_theGxDevicePtr->XformSetView(view);

        const float minX = static_cast<float>(tile) / static_cast<float>(kTileCount);
        const float maxX = static_cast<float>(tile + 1) / static_cast<float>(kTileCount);
        GxXformSetViewport(minX, maxX, 0.0f, 1.0f, oldMinZ, oldMaxZ);
        DayNight::RenderSky(&full);
    }

    g_theGxDevicePtr->RsPop();
    RestoreTargets(oldColour, oldDepth);
    g_theGxDevicePtr->XformSetProjection(oldProjection);
    g_theGxDevicePtr->XformSetView(oldView);
    GxXformSetViewport(oldMinX, oldMaxX, oldMinY, oldMaxY, oldMinZ, oldMaxZ);

    s_valid = true;
}

}  // namespace SkyCapture
