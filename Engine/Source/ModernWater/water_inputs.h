#ifndef ENGINE_SOURCE_MODERNWATER_WATER_INPUTS_H
#define ENGINE_SOURCE_MODERNWATER_WATER_INPUTS_H

#include "Engine/Source/ModernWater/engine_actors.h"
#include "Engine/Source/ModernWater/water_renderer.h"

// Not in the original 3.3.5a binary. Where the shading pass gets its per-frame numbers.
//
// The WXL module read all of this out of absolute addresses in Wow.exe: the camera, the sun and moon positions,
// the fog group, the sky and water colour bands, the far clip, plus a code-signature guard on every one of them.
// This tree compiles the client, so the same values come from the structures that own them -- DayNight's current
// light bands and fog info, the gx device's view and projection matrices, the camera object.
//
// One thing is deliberately different. The module had to reconstruct the camera-relative view and the GL-style
// projection because it could only see what the d3d9 wrapper saw. Here the device already holds both, in the
// exact convention the client's own shaders use, so the water reads the same matrices the terrain did and cannot
// disagree with them.

namespace ModernWater {

// Fills the frame's shared inputs. Returns false when the frame is not in a world that can be shaded -- no
// camera, no map, the glue screens -- in which case the caller leaves the client's water alone.
bool BuildFrameInputs(WaterFrameInputs& out);

// The zone's sky and water colours, from DayNight's blended output bands.
void ReadSkyAndWaterColours(WaterFrameInputs& out);

// The unit contacts around the camera, for the ripples.
void ReadContacts(const WaterFrameInputs& in, WaterContactFrame& out);

}  // namespace ModernWater

#endif
