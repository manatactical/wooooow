#ifndef ENGINE_SOURCE_AMBIENTOCCLUSION_AMBIENTOCCLUSION_H
#define ENGINE_SOURCE_AMBIENTOCCLUSION_AMBIENTOCCLUSION_H

#include <cstddef>
#include <cstdint>

#include "Engine/Source/FFX/FFX.h"
#include "Engine/Source/Tempest/C2iVector.h"

class CGxBatch;
class CGxShader;
class CGxTex;
struct CVar;

// Not in the original 3.3.5a binary. Screen-space ambient occlusion over the finished world frame; see
// docs/AMBIENT-OCCLUSION.md for the design.
//
// One frame, in order:
//
//   1. Occupancy pass. A fullscreen pixel shader reconstructs each pixel's view-space position and normal from
//      the frame's shared scene depth (CUnitOutline::DepthPassTexture), spins a 12-sample hemisphere kernel by
//      an interleaved-gradient-noise angle, and writes how much of that hemisphere an occluder covers. 1.0 is
//      open space, lower is a corner. Written to a frame-sized AO target.
//   2. Blur pass. One 8-tap bilateral pass: taps are weighted by how close their depth is to the centre's, so
//      the term softens without bleeding across a silhouette onto the wall behind it.
//   3. Composite. A fullscreen quad with multiply blending (GxBlend_Mod: dst * src) reading the blurred term.
//      The frame is never read into a texture, so no copy of the back buffer is needed.
//
// It sits at the world -> interface boundary, before the outline composite and before the anti-aliasing pass:
// the outline is a selection cue and must not be dimmed by a shadow, and FXAA is left to smooth the AO edges
// along with everything else. It stands down entirely when the frame has no shared scene depth -- multisampling
// on, or a device without a non-pow2 INTZ render target -- because AO without depth is a black frame, not a
// disabled effect.
//
// The ssao cvar: 0 off, 1 on, 2 shows the occlusion term itself (an opaque grey blit), for telling "the pass
// does nothing" apart from "the pass looks wrong".
class CAmbientOcclusion {
    public:
    // Not in the original binary. Registers the ssao cvar and the device-restored callback. Called once from the
    // world frame constructor, which happens again after a UI reload, so it is idempotent.
    static void Init();

    // Not in the original binary. Runs the three passes over the frame the world just finished. Only ever
    // called at the world -> interface boundary, with the world's own projection and viewport still current.
    static void Apply();

    // Not in the original binary. Whether the option is on and a device exists.
    static bool Enabled();

    // Not in the original binary. Whether the world being drawn right now is feeding the pass: the option is on
    // and the shared depth is bound. Geometry that must not take part in the occlusion -- the grass -- asks this
    // while it draws, and keeps itself out of the depth.
    static bool Active();

    // Not in the original binary. Records one batch of geometry as "not to be occluded": it is re-drawn flat into
    // a frame-sized mask, depth-tested against the shared scene depth, and the occupancy pass then leaves those
    // pixels alone and does not let them occlude their neighbours. Called right before the caller's own draw of
    // the batch, with its vertex stream, shader and texture already set. A no-op while the pass is not running.
    static void StampExcluded(const CGxBatch& batch);

    private:
    static bool EnsureResources();
    static void OnDeviceRestored();

    static CVar* s_cvarSsao;
    static CVar* s_cvarSpread;
    static CVar* s_cvarStrength;
    static CGxShader* s_aoPS;
    static CGxShader* s_blurPS;
    static CGxShader* s_compositePS;
    static CGxShader* s_excludePS;
    static FFX::Tex s_excludeTex;
    static CGxTex* s_exclude;
    static bool s_excludeStamped;
    static FFX::Tex s_aoTex;
    static FFX::Tex s_aoBlurTex;
    static CGxTex* s_ao;
    static CGxTex* s_aoBlur;
};

#endif
