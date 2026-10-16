#ifndef ENGINE_SOURCE_GX_VIEWSETTINGS_H
#define ENGINE_SOURCE_GX_VIEWSETTINGS_H

// Not in the original binary. Ported from 15595 (Cataclysm). The screen-space passes need the view
// extents, the near/far planes and the far-clip fraction to turn a depth into a screen coordinate.
//
// Deliberately not ported from 15595, both would duplicate state this tree already owns:
//   SetFog/GetFog  - the fog lives in CShaderEffect::s_fogParams, fed from the active lighting.
//   Set/GetSubmerged - the camera submerge flag lives in Liquid::SetCameraSubmerged.

#include <cstddef>
#include <cstdint>
#include "Engine/Source/Tempest/CAaRect.h"
#include "Engine/Source/Tempest/C4Vector.h"

namespace ViewSettings {
    C4Vector CalculateHomToScreen(float offsetX, float offsetY);
    C4Vector CalculateParams();
    void SetViewExtents(const CAaRect& extents);
    void SetViewportFarClip(float farClip);
    void SetZNearFar(float zNear, float zFar);
}

#endif