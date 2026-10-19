#ifndef ENGINE_SOURCE_UNITOUTLINE_UNITOUTLINE_H
#define ENGINE_SOURCE_UNITOUTLINE_UNITOUTLINE_H

#include <cstddef>
#include <cstdint>
#include "Engine/Source/Gx/Types.h"

class CGxBatch;
class CGxShader;
class CGxTex;
class CM2Model;
class CTexture;

// Not in the original 3.3.5a binary. A reaction-coloured silhouette outline on the mouseover and the
// target unit, ported from the WXL module wxl-mana-unit-outline (WarcraftXL), where the same pipeline
// was a scripted extension driven by the core's render events.
//
// One frame, in order:
//
//   1. The model scene draws the world. Every batch belonging to a recorded target re-draws itself
//      into a mask with a flat-colour pixel shader (StampBatch); every batch of the local player's own
//      model goes into a second mask the same way. The batches are the real ones, so the silhouette is
//      the game's own skinning, at whatever pose the frame draws.
//   2. At the world -> interface boundary (Composite) the mask is softened by two separable gaussian
//      passes and edge-detected into the frame with alpha blending, the player's mask subtracted so
//      the line never draws across the player's own body.
//   3. The target list is spent and emptied; the game refills it as it draws the next frame.
//
// The masks are back-buffer sized and take their depth from the scene. Terrain and world objects have
// already written the frame's depth by the time a unit is stamped, so the stamp depth tests against them
// and a wall hides only the pixels it covers -- not the whole outline. The depth is the device's own, so
// the answer does not depend on the order models happen to be drawn in.
//
// This class is only the machine: it knows nothing about units, factions or selection. The game fills
// it in once per frame through SetPlayerModel / AddTarget / AddDoodad.
class CUnitOutline {
    public:
    // Not in the original binary. Registers the device-restored callback. The unitOutline cvar itself is
    // registered by CWorldParam::Initialize, because this runs from the world frame constructor and the
    // options panel reads the cvar's default at the login screen, before any world frame exists.
    // Called once from ClientInitialize, where a console is guaranteed.
    static void Init();

    // Not in the original binary. Empties the target list and forgets the player model. The game calls
    // this once per frame before it adds this frame's selections.
    static void BeginTargets();
    static void SetPlayerModel(CM2Model* model);

    // Not in the original binary. Records a target's model chain with the reaction colour; the chain is
    // walked so a mount is outlined with the same colour as the unit riding it. Occlusion is not decided
    // here: the stamp depth tests against the scene, so a wall clips the line where it covers the unit.
    static void AddTarget(CM2Model* model, const float rgb[3], bool keepFull);

    // Not in the original binary. A world object the cursor is over: no reaction colour and no mount
    // chain, so it is recorded on its own.
    static void AddDoodad(CM2Model* model, const float rgb[3]);

    // Not in the original binary. One model batch about to be drawn. Stamps the model's silhouette when
    // it is a recorded target, or the player's when it is that, and leaves the device state as it found
    // it. Called from the model scene's batch draws, right before the caller's own draw of the batch.
    static void StampBatch(const CGxBatch& batch, CM2Model* model);

    // Not in the original binary. The world -> interface boundary: softens the mask, edge-detects it
    // into the frame, and arms the next frame. Only ever called while the world's own projection and
    // render target are the ones bound, immediately after the world geometry.
    static void Composite();

    // Not in the original binary. Whether a model -- or anything it hangs off, so a rider counts with its
    // mount -- is one of this frame's outline targets. The model scene's opaque sort asks this to keep those
    // models at the end of the pass, which is what puts the rest of the world into the depth buffer before
    // they stamp against it.
    static bool IsTarget(CM2Model* model);

    // Not in the original binary. Whether this frame recorded anything to outline at all. The model scene
    // asks this once per pass, so the default render order is left alone whenever the feature is idle.
    static bool HasTargets();

    // Not in the original binary. Binds a frame-sized depth texture for the 3D pass and detaches it again.
    // Everything between the two calls -- terrain, world objects and the units that get stamped -- shares
    // that one depth buffer, which is what lets a stamp test against the scene. Both are no-ops when the
    // device cannot pair a depth texture with the scene's colour target.
    static void BeginDepthPass();
    static void EndDepthPass();

    private:
    static bool Enabled();
    static bool EnsureResources();
    static bool EnsureDepthTexture();
    static bool OcclusionSupported();
    static void OnDeviceRestored();

    // Not in the original binary. Puts the depth the 3D pass had before this module back, safely: the device
    // derives a depth from a bound colour *texture*, and asking for a null depth while one is bound reads its
    // size without checking it (see CGxDeviceD3d::DeviceSetRenderTarget). Shared by EndDepthPass and the
    // failed-bind recovery so both take the same path.
    static void RestoreSavedDepth();

    static void AddEntry(CM2Model* model, const float color[4], bool keepFull);
    static int32_t FindTarget(CM2Model* model);
    static bool IsModel(CM2Model* model, CM2Model* want);

    static void StampSilhouette(const CGxBatch& batch, int32_t index);
    static void StampOccluder(const CGxBatch& batch);
    static void StampMask(const CGxBatch& batch, const float color[4], CGxTex* target, bool& cleared);

    static void BlurPass();
    static void EdgePass();
};

#endif