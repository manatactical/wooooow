#ifndef ENGINE_SOURCE_UNDERWATERFX_UNDERWATERFX_H
#define ENGINE_SOURCE_UNDERWATERFX_UNDERWATERFX_H

#include <cstddef>
#include <cstdint>

#include "Engine/Source/FFX/FFX.h"

class CGxShader;
class CGxTex;
struct CVar;

// Not in the original binary. What the caller knows about the liquid the camera is in, read from the day/night and
// liquid state (this module deliberately knows neither, so it links against nothing but the engine).
struct UnderwaterMedium {
    // True while the camera is below a liquid surface.
    bool submerged;

    // The colour things fade into with distance, each channel 0..1: the engine's own fog colour for the liquid.
    float color[3];

    // The distance, in yards, at which the engine's own fog is complete. The added fog is scaled to it, so a murky
    // swamp is murkier than a clear lagoon.
    float fogEnd;

    // How far the surface is above the camera, in yards. What lies above that height is the world outside the water.
    float surfaceDepth;

    // The z the world's viewport maps the far plane to, as CDepthFogAtmosphere::depthMaxZ.
    float depthMaxZ;
};

// Not in the original binary. The look of being underwater: the finished world frame is softened more the farther
// away it is, and fades into the water's colour with distance (red first, as it does in real water). It is the
// counterpart of what the water pass does to the seabed seen from above, which stops at the surface.
//
// One frame, in order: the frame is copied whole into a scratch texture; one fullscreen pass reads the copy and the
// frame's shared scene depth and writes the result back over the frame. A short fade ramps the effect in and out as
// the camera crosses the surface, so it does not pop.
//
// It runs at the world -> interface boundary with the other world passes, and stands down when the frame has no shared
// scene depth (multisampling on, or a device without a non-pow2 INTZ render target), when the world drew into an
// off-screen target, and on any device but D3D9.
//
// The underwaterFx cvar: 0 off, 1 on. underwaterFxStrength scales the blur and the fog.
class CUnderwaterFx {
    public:
    // Not in the original binary. Registers the cvars and the device-restored callback; idempotent.
    static void Init();

    // Not in the original binary. Draws the effect over the frame the world just finished.
    static void Apply(const UnderwaterMedium& medium);

    // Not in the original binary. Whether the option is on and a device exists.
    static bool Enabled();

    private:
    static bool EnsureResources();
    static void OnDeviceRestored();

    static CVar* s_cvarEnabled;
    static CVar* s_cvarStrength;
    static CGxShader* s_ps;
    static FFX::Tex s_sceneTex;
    static CGxTex* s_scene;
};

#endif
