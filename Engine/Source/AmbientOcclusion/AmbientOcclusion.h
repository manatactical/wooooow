#ifndef ENGINE_SOURCE_AMBIENTOCCLUSION_AMBIENTOCCLUSION_H
#define ENGINE_SOURCE_AMBIENTOCCLUSION_AMBIENTOCCLUSION_H

#include <cstddef>
#include <cstdint>

#include "Engine/Source/FFX/FFX.h"
#include "Engine/Source/Tempest/C2iVector.h"

class CGxBatch;
class CGxShader;
class CGxTex;
class CM2Model;
struct CVar;

// Not in the original 3.3.5a binary. Screen-space ambient occlusion over the finished world frame; see
// docs/AMBIENT-OCCLUSION.md for the design.
//
// One frame, in order:
//
//   1. Occupancy passes, three of them, one per size of thing: small (props, contact shadows), medium (furniture,
//      boxes) and big (buildings). Each is a fullscreen pixel shader that reconstructs the pixel's view-space
//      position and normal from the half-size distance buffer, spins a 5-tap hemisphere kernel, and writes how
//      much of that hemisphere an occluder covers. 1.0 is open space, lower is a corner. The scales differ in
//      reach (in yards and in on-screen pixels), in strength, and in how far away they still run: the small one
//      stops first. Each writes one colour channel of the same half-size AO target (a colour write mask).
//   2. Blur passes. Two 3x3 bilateral passes over all three channels at once: taps are weighted by how close
//      their depth is to the centre's, so the term softens without bleeding across a silhouette onto the wall
//      behind it.
//   3. Composite. A fullscreen quad with multiply blending (GxBlend_Mod: dst * src) reading the blurred term,
//      upsampled depth-aware to full size, with the three scales multiplied together.
//      The frame is never read into a texture, so no copy of the back buffer is needed.
//
// It sits at the world -> interface boundary, before the outline composite and before the anti-aliasing pass:
// the outline is a selection cue and must not be dimmed by a shadow, and FXAA is left to smooth the AO edges
// along with everything else. It stands down entirely when the frame has no shared scene depth -- multisampling
// on, or a device without a non-pow2 INTZ render target -- because AO without depth is a black frame, not a
// disabled effect.
//
// The ssao cvar: 0 off, 1 on, 2 shows the occlusion term itself (an opaque grey blit), for telling "the pass
// does nothing" apart from "the pass looks wrong". 3, 4 and 5 show the small, medium and big scale alone.
class CAmbientOcclusion {
    public:
    // Not in the original binary. Registers the ssao cvar and the device-restored callback. Called once from the
    // world frame constructor, which happens again after a UI reload, so it is idempotent.
    static void Init();

    // Not in the original binary. Runs the three passes over the frame the world just finished. Only ever
    // called at the world -> interface boundary, with the world's own projection and viewport still current.
    static void Apply();

    // Not in the original binary. Whether the option is on and a device exists. Always false while the camera is
    // submerged (see SetSubmerged), which stands down every part of the module at once: the passes, the exclusion
    // stamps and the character pass.
    static bool Enabled();

    // Not in the original binary. Whether the camera is below a liquid surface. The game says so once per frame, before
    // the scene draws, because the stamps that go with the scene have to know too. The module cannot ask the world
    // itself: it knows nothing of liquids. Underwater there is no ambient light to occlude, and the underwater fog and
    // blur are drawn over the frame.
    static void SetSubmerged(bool submerged);

    // Not in the original binary. How hard it is raining, 0 to 1, from the game once per frame. Wet, overcast light has
    // little direct light to take shadow from, so the strength is reduced by up to half (kRainStrengthLoss) as the rain
    // builds. Follows the weather's own intensity ramp, so it fades in and out with the rain instead of switching.
    static void SetRain(float amount);

    // Not in the original binary. The distance, in yards, where the engine's own fog begins and where it is complete,
    // from the game once per frame. The fog (and with it the storm's grey haze, which is that fog drawn in close) is
    // already in the frame when this pass runs, so a pixel that is mostly fog has no surface left to occlude; the term
    // is faded to 1 by the same linear factor the scene used. Both zero, or an end that is not past the start, mean no
    // fog (as when the depth-fog pass has taken it over, which draws after this one).
    static void SetFog(float start, float end);

    // Not in the original binary. The engine's fog colour, each channel 0 to 1, from the game once per frame. In the rain
    // the occlusion takes its hue: a shadowed pixel is tinted toward this colour (at the brightness plain occlusion would
    // have left it) instead of being darkened neutrally, so the shading belongs to the wet, overcast light it sits in.
    // The tint is scaled by the rain (SetRain), so a clear day is unchanged.
    static void SetFogColor(float r, float g, float b);

    // Not in the original binary. Whether the world being drawn right now is feeding the pass: the option is on
    // and the shared depth is bound. Geometry that must not take part in the occlusion -- the grass -- asks this
    // while it draws, and keeps itself out of the depth.
    static bool Active();

    // Not in the original binary. Records one batch of geometry as "not to be occluded": it is re-drawn flat into
    // a frame-sized mask, depth-tested against the shared scene depth, and the occupancy pass then leaves those
    // pixels alone and does not let them occlude their neighbours. Called right before the caller's own draw of
    // the batch, with its vertex stream, shader and texture already set. A no-op while the pass is not running.
    //
    // The stamps go between BeginExcludeStamps and EndExcludeStamps, which switch the render target in and out once
    // for the whole run rather than once per batch. Begin returns false when the pass is not running, and then the
    // other two do nothing.
    static bool BeginExcludeStamps(bool ignoreAlpha = false);
    static void StampExcluded(const CGxBatch& batch);
    static void EndExcludeStamps();

    // Not in the original binary. Called right before a water surface is drawn. The surface is stamped into the same
    // exclusion mask the grass uses, whole -- its texture alpha is its transparency, not its shape -- so whatever is
    // seen through it, and the surface itself, is left unshaded.
    static void StampLiquidBatch(const CGxBatch& batch);

    // Not in the original binary. The close-up character pass: while the camera is closer to the player than the
    // ssaoCharDistance cvar (yards), the player's own model -- body, mount, gear -- gets a dedicated occlusion of its
    // own instead of the frame-wide one. Past that distance it fades out over the next few yards, the frame-wide pass
    // taking the character back as the dedicated one lets go, and once it has faded completely it is off. It runs at the frame's full size on the character's pixels only, with a
    // denser kernel and a blur that cancels the per-pixel rotation exactly, so a curved, small, moving surface comes out
    // smooth where the half-size pass is grainy. The frame-wide pass leaves those pixels alone (they are still in the
    // depth it reads, so the character shades the world around it as before).
    //
    // The game says how far the camera is, once per frame, before the scene draws (see CUnitOutlineGather::Update).
    static void SetCameraDistance(float yards);

    // Not in the original binary. One model batch about to be drawn. If it is the player's and the close-up pass is
    // on, it is re-drawn flat into the green channel of the exclusion mask, depth-tested against the scene so a wall
    // in front of the character hides the pixels it covers. Called by the model scene right before the caller's own draw
    // of the batch, with its vertex stream, shader and texture already set; a no-op for anything else.
    static void StampCharacter(const CGxBatch& batch, CM2Model* model);

    // Not in the original binary. Whether the close-up character pass is wanted this frame.
    static bool CharacterWanted();

    private:
    static bool EnsureResources();
    static bool EnsureCharacterResources();
    static bool BeginStamps(bool ignoreAlpha, int32_t channelMask);
    static void OnDeviceRestored();

    static CVar* s_cvarSsao;
    static CVar* s_cvarSpread;
    static CVar* s_cvarStrength;
    static CVar* s_cvarDistance;
    static CVar* s_cvarCharacter;
    static CVar* s_cvarCharDistance;
    static CGxShader* s_charAoPS;
    static CGxShader* s_charBlurPS;
    static CGxShader* s_charCompositePS;
    static FFX::Tex s_charAoTex;
    static FFX::Tex s_charBlurTex;
    static CGxTex* s_charAo;
    static CGxTex* s_charBlur;
    static bool s_charStamped;
    static bool s_submerged;
    static float s_rain;
    static float s_fogStart;
    static float s_fogEnd;
    static float s_fogColor[3];
    static float s_cameraDistance;
    static CGxShader* s_zPS;
    static CGxShader* s_aoPS;
    static CGxShader* s_blurPS;
    static CGxShader* s_compositePS;
    static CGxShader* s_excludePS;
    static FFX::Tex s_excludeTex;
    static CGxTex* s_exclude;
    static bool s_excludeStamped;
    static bool s_stamping;
    static FFX::Tex s_zTex;
    static CGxTex* s_z;
    static FFX::Tex s_aoTex;
    static FFX::Tex s_aoBlurTex;
    static CGxTex* s_ao;
    static CGxTex* s_aoBlur;
};

#endif
