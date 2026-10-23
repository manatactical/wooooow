#ifndef ENGINE_SOURCE_SUNSHAFTS_SUNSHAFTS_H
#define ENGINE_SOURCE_SUNSHAFTS_SUNSHAFTS_H

#include <cstddef>
#include <cstdint>

#include "Engine/Source/FFX/FFX.h"
#include "Engine/Source/Tempest/C3Vector.h"

class CGxShader;
class CGxTex;
struct CVar;

// Not in the original binary. What the sky says about the sun this frame, gathered by the caller (the world frame
// knows the day/night state; this module deliberately does not, so it links against nothing but the engine).
struct SunShaftsLight {
    // From the camera to the sun, in world space. Need not be normalised.
    C3Vector toSun;

    // The light's colour, each channel 0..1. The shafts are tinted with it, so a sunset throws orange ones.
    float color[3];

    // 0..1: how much sun there is to make shafts from -- zero at night, indoors under a skybox, underwater or under a
    // thick cloud layer. Everything about the sky and time of day, nothing about where the camera is looking.
    float visibility;
};

// Not in the original binary. Crepuscular rays ("sun shafts", "god rays") over the finished world frame.
//
// A screen-space radial march that needs nothing but the frame's shared scene depth, which is why it fits where the
// ambient occlusion pass does and costs no copy of the frame. The idea: air scatters sunlight toward the eye wherever
// the sun can see that air, and the sun can see it wherever nothing stands between the two. On screen that is "from
// this pixel, walk toward the sun's screen position; the more of the walk is open sky, the brighter the shaft". Sky
// is recognised by its depth (the same test the ambient occlusion pass uses), so a tree canopy, a building edge or a
// mountain ridge cuts the light and leaves shafts through the gaps.
//
// One frame, in order:
//
//   1. March. A half-size pass: for every pixel, 48 depth taps along the way to the sun, each counting as lit when it
//      lands on sky, weighted so the nearby part of the walk matters most. The walk is cut at the edge of the world
//      view -- nothing is known beyond it -- and capped in length. A pixel's own depth sets how much air lies in front
//      of it, so a wall at arm's length does not glow and a distant hillside does; sky itself is dimmed so it does not
//      white out.
//   2. Blur. Two 3x3 box passes. The march rotates its starting offset over a 3x3 tile of nine evenly spaced values and
//      the box covers each exactly once, so the banding of a fixed step count cancels (the ambient occlusion pass does
//      the same with its kernel rotation).
//   3. Composite. A fullscreen quad with additive blending, tinted by the light's colour. The frame is never read.
//
// It runs at the world -> interface boundary, after the ambient occlusion (shafts are light, and must not be darkened
// by a shadow term) and before the outline and the anti-aliasing. Like the ambient occlusion pass it stands down when
// the frame has no shared scene depth: multisampling on, or a device without a non-pow2 INTZ render target.
//
// The sunShafts cvar: 0 off, 1 on, 2 shows the shaft term itself (an opaque grey blit).
class CSunShafts {
    public:
    // Not in the original binary. Registers the cvars and the device-restored callback. Called from the world frame
    // constructor, which happens again after a UI reload, so it is idempotent.
    static void Init();

    // Not in the original binary. Runs the passes over the frame the world just finished. Only ever called at the
    // world -> interface boundary, with the world's own view, projection and viewport still current.
    static void Apply(const SunShaftsLight& sun);

    // Not in the original binary. Whether the option is on and a device exists.
    static bool Enabled();

    private:
    static bool EnsureResources();
    static void OnDeviceRestored();

    static CVar* s_cvarShafts;
    static CVar* s_cvarStrength;
    static CGxShader* s_marchPS;
    static CGxShader* s_blurPS;
    static CGxShader* s_compositePS;
    static FFX::Tex s_shaftTex;
    static FFX::Tex s_shaftBlurTex;
    static CGxTex* s_shaft;
    static CGxTex* s_shaftBlur;
};

#endif
