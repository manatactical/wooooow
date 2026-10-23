#ifndef ENGINE_SOURCE_DEPTHFOG_DEPTHFOG_H
#define ENGINE_SOURCE_DEPTHFOG_DEPTHFOG_H

#include <cstddef>
#include <cstdint>

#include "Engine/Source/FFX/FFX.h"
#include "Engine/Source/Tempest/C3Vector.h"

class CGxShader;
class CGxTex;
struct CVar;

// Not in the original binary. What the sky says about the air this frame, gathered by the caller (the world frame knows
// the day/night state; this module deliberately does not, so it links against nothing but the engine).
struct DepthFogAtmosphere {
    // The sky's colour gradient, 0 = the horizon (the fog colour the world fades toward) up to 5 = the zenith. Each
    // channel 0..1. A distant ridge is fogged toward the gradient at the height it stands against the sky, so its
    // silhouette dissolves into the sky behind it instead of cutting it out.
    float sky[6][3];

    // From the camera to the sun, in world space. Need not be normalised.
    C3Vector toSun;

    // The sun's light colour, each channel 0..1. The in-scatter toward the sun is tinted with it.
    float sunColor[3];

    // 0..1: how much sun there is to scatter. Zero at night.
    float sunVisibility;

    // The camera's height in world space, and the far clip distance the engine culls at.
    float cameraZ;
    float farClip;

    // 0..1: how much fog there should be at all. Zero indoors and underwater, where the engine's own fog rules.
    float amount;

    // The far end of the depth range the world was drawn into (its viewport's maximum z). The world is drawn into only part
    // of the depth range, the rest being kept for the horizon and the sky, and the viewport is back to the full range by the
    // time the pass runs, so the caller says what it was. Zero: use the device's current one.
    float depthMaxZ;
};

// Not in the original binary. Distance and height fog as a depth-based post pass over the finished world frame.
//
// Two terms, combined into a single opacity per pixel:
//
//   * Exponential height fog. Air is densest low and thins with altitude, density = d0 * exp(-height / scale), and the
//     pass integrates it analytically along the eye-to-surface ray, so a valley fills with haze while a ridge above the
//     camera's own level stays clear. Heights are relative to the camera (a fixed drop below it), so the look is the same
//     at sea level and on a plateau.
//   * Far fade. Over the last stretch before the far clip everything fades fully into the sky, so geometry is never
//     seen to appear or vanish at the cull distance and a mountain range's silhouette is the sky's colour. Sky pixels
//     (nothing drawn there) are left alone: the fade only needs to bring the world up to the sky, not repaint it.
//
// The colour is the sky's own gradient, looked up by the elevation of the pixel's view ray: at the horizon the fog is
// the horizon colour the engine's fog and the sky dome already blend to, and a peak that rises into the sky is fogged
// toward the sky colour at that height. Looking toward the sun, the colour brightens toward the sun's light colour
// (in-scatter), so a sunset throws a warm haze across the valley instead of a flat grey.
//
// Two full-screen quads: the fog is worked out from the shared scene depth into a target of its own, then alpha-blended
// over the frame. The frame is never read, only the depth, which is why
// this fits where the ambient occlusion and sun glow passes do. It runs inside the world render, after everything opaque
// and the first model pass but before the water, so the water is drawn over the fogged world and takes no fog, and the
// post passes (glow, outline, anti-aliasing) all see the fogged frame. Like them it stands down when the frame has no shared scene depth: multisampling on,
// or a device without a non-pow2 INTZ render target.
//
// Whatever draws after it (water, weather, the second model pass) is not fogged at all. Whatever draws before it without
// writing depth (particles) takes the fog of the opaque surface behind it.
//
// The depthFog cvar: 0 off, 1 on, 2 shows the fog opacity itself (an opaque grey blit).
class CDepthFog {
    public:
    // Not in the original binary. Registers the cvars and the device-restored callback. Called from the world frame
    // constructor, which happens again after a UI reload, so it is idempotent.
    static void Init();

    // Not in the original binary. Runs the pass over the frame the world just finished. Only ever called at the
    // point inside the world render, with the world's own view, projection and viewport current.
    static void Apply(const DepthFogAtmosphere& air);

    // Not in the original binary. Whether the option is on and a device exists.
    static bool Enabled();

    private:
    static bool EnsureResources();
    static void OnDeviceRestored();

    static CVar* s_cvarFog;
    static CVar* s_cvarStrength;
    static CVar* s_cvarLayers;
    static CGxShader* s_fogPS;
    static CGxShader* s_compositePS;
    static FFX::Tex s_fogTex;
    static CGxTex* s_fog;
};

#endif
