#ifndef ENGINE_SOURCE_SUNGLOW_SUNGLOW_H
#define ENGINE_SOURCE_SUNGLOW_SUNGLOW_H

#include <cstddef>
#include <cstdint>

#include "Engine/Source/FFX/FFX.h"
#include "Engine/Source/Tempest/C3Vector.h"

class CGxShader;
class CGxTex;
struct CVar;

// Not in the original binary. What the sky says about the sun this frame, gathered by the caller (the world frame
// knows the day/night state; this module deliberately does not, so it links against nothing but the engine).
struct SunGlowLight {
    // From the camera to the sun, in world space. Need not be normalised.
    C3Vector toSun;

    // The light's colour, each channel 0..1. The glow is tinted with it, so a sunset glows orange.
    float color[3];

    // 0..1: how much sun there is to glow -- zero at night, indoors under a skybox, underwater or under a thick cloud
    // layer. Everything about the sky and time of day, nothing about where the camera is looking.
    float visibility;
};

// Not in the original binary. A soft glow around the sun's disc, added over the finished world frame when the sun is
// in view.
//
// One fullscreen additive quad. The glow is a bright core inside a wide halo, centred on the sun's screen position
// (its direction through the world's own view and projection). The frame's shared scene depth says what the sun is
// behind: the glow is full strength on sky, only a faint bleed over anything solid, and dims as the sun itself is
// covered by a canopy or a ridge. It fades out as the sun leaves the view and is gone when it is behind the camera.
//
// It runs at the world -> interface boundary, after the ambient occlusion and before the outline and the
// anti-aliasing. Like the ambient occlusion pass it stands down when the frame has no shared scene depth: multisampling
// on, or a device without a non-pow2 INTZ render target.
//
// The sunGlow cvar: 0 off, 1 on. sunGlowStrength scales it.
class CSunGlow {
    public:
    // Not in the original binary. Registers the cvars and the device-restored callback. Called from the world frame
    // constructor, which happens again after a UI reload, so it is idempotent.
    static void Init();

    // Not in the original binary. Draws the glow over the frame the world just finished. Only ever called at the
    // world -> interface boundary, with the world's own view, projection and viewport still current.
    static void Apply(const SunGlowLight& sun);

    // Not in the original binary. Whether the option is on and a device exists.
    static bool Enabled();

    private:
    static bool EnsureResources();
    static void OnDeviceRestored();

    static CVar* s_cvarGlow;
    static CVar* s_cvarStrength;
    static CGxShader* s_glowPS;
    static FFX::Tex s_quadTex;
    static CGxTex* s_quad;
};

#endif
