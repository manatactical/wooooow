// Not in the original binary. Ported from 15595 (Cataclysm); see ViewSettings.h for what was left out.

#include <cstdint>
#include "Engine/Source/Gx/ViewSettings.h"
#include "Engine/Source/Gx/CGxDevice/CGxDevice.h"
#include "Engine/Source/Gx/Gx.h"

// Values carried over from 15595.
static float s_zFar = 946.0f;
static float s_zNear = 0.2f;
static float s_viewportFarClip = 0.94f;

static CAaRect s_viewExtents;

void ViewSettings::SetZNearFar(float zNear, float zFar) {
    // DIAG: the screen-space passes derive every depth-to-screen constant from these two values, and a
    // degenerate near/far silently zeroes them rather than failing. Logged on change only, to Logs/gx.log.
    if (zNear != s_zNear || zFar != s_zFar) {
        g_theGxDevicePtr->Log("ViewSettings::SetZNearFar: zNear=%.4f zFar=%.4f\n", static_cast<double>(zNear), static_cast<double>(zFar));
    }

    s_zNear = zNear;
    s_zFar = zFar;
}

void ViewSettings::SetViewportFarClip(float farClip) {
    s_viewportFarClip = farClip;
}

void ViewSettings::SetViewExtents(const CAaRect& extents) {
    // DIAG: same reason as SetZNearFar - the extents are the other half of CalculateHomToScreen, and a
    // viewport of zero size would make the result constant. Logged on change only, to Logs/gx.log.
    if (extents.minX != s_viewExtents.minX || extents.minY != s_viewExtents.minY || extents.maxX != s_viewExtents.maxX || extents.maxY != s_viewExtents.maxY) {
        g_theGxDevicePtr->Log("ViewSettings::SetViewExtents: [%.4f..%.4f]x[%.4f..%.4f] farClip=%.4f\n", static_cast<double>(extents.minX), static_cast<double>(extents.maxX), static_cast<double>(extents.minY), static_cast<double>(extents.maxY), static_cast<double>(s_viewportFarClip));
    }

    s_viewExtents = extents;
}

C4Vector ViewSettings::CalculateParams() {
    C4Vector params;
    params.x = s_zNear;
    params.y = s_zFar;
    params.z = (1.0f / s_zFar - 1.0f / s_zNear) / s_viewportFarClip;
    params.w = 1.0f / s_zNear;

    return params;
}

C4Vector ViewSettings::CalculateHomToScreen(float offsetX, float offsetY) {
    float x = ((s_viewExtents.minY + 1.0f) - (1.0f - s_viewExtents.maxY)) * 0.5f;
    float scaleY;
    float y;

    if (g_theGxDevicePtr->m_caps.m_texelCenterOnEdge) {
        scaleY = (s_viewExtents.maxX - s_viewExtents.minX) * 0.5f;
        y = (s_viewExtents.minX + 1.0f) - (1.0f - s_viewExtents.maxY);
    } else {
        scaleY = (s_viewExtents.maxX - s_viewExtents.minX) * -0.5f;
        y = ((1.0f - s_viewExtents.maxX) + 1.0f) - s_viewExtents.minX;
    }

    y = y * 0.5f;

    if (g_theGxDevicePtr->m_caps.m_pixelCenterOnEdge) {
        x = x + offsetX;
        y = y + offsetY;
    }

    C4Vector result;
    result.x = (s_viewExtents.maxY - s_viewExtents.minY) * 0.5f;
    result.y = scaleY;
    result.z = x;
    result.w = y;

    return result;
}