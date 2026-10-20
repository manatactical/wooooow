#ifndef WOW_SOURCE_WORLDCLIENT_GRASSMOTION_H
#define WOW_SOURCE_WORLDCLIENT_GRASSMOTION_H

#include <cstddef>
#include <cstdint>

class CGxShader;
class CDetailDoodadData;

// Not in the original 3.3.5a binary. Grass motion: the wind that moves detail doodads, and the parting
// their blades do around nearby characters.
//
// The port follows `seyris-living-azeroth` (GrassMotion.cpp / GrassDoodads.cpp / ShaderPatch.cpp), a
// WarcraftXL extension for the same 12340 client, but where that module needed three detours this tree owns
// three seams of its own:
//
//   * the shader is rewritten once from CDetailDoodad::Initialize, after ShaderCreate has read the .bls
//     bytecode and before anything has bound one. `m_loaded` is still 0 there, so the device has not
//     compiled it yet and the patched text is what it compiles. Each permutation is recognised by identity
//     against the array ShaderCreate filled, so a permutation this module cannot handle is left alone
//     instead of mis-patched.
//   * the per-frame and per-doodad values ride vertex-constant rows uploaded right beside the engine's own
//     block in CDetailDoodad::SetupVertexShader. The engine uses c0..c13; c14..c19 and c35..c55 are ours.
//   * the per-doodad row is a register written between a batch's own sub-draws, so the shader needs no
//     indirect table, no index decode, and no colour tagging. That is the one place this port is materially
//     simpler than the reference, which had to tag each instance's colour with a table index and decode it
//     back out of v2. Here the batch is baked sorted by doodad, so the caller sets c37 before each sub-draw.
//
// Everything a player tunes is a CVar (registered in CWorldParam::Initialize and again in Init below); the
// wind direction, the wavelengths and the stiffness window are named constants beside the shader text.
class CGrassMotion {
    public:
    // The clutter density ceiling, read by the Physics tab, the density cvar and the pool sizing, so the
    // pool ceiling and the cvar ceiling cannot drift apart. 196 is 12.25x the stock default.
    static const int32_t kMaxClutterDensity = 196;
    static const int32_t kMinClutterDensity = 16;

    // The view-distance ceiling, for the same reason. The engine still clamps per map through
    // CWorld::ValidateFarClip and the chunk-side test, so a large number simply does not apply
    // everywhere.
    static const float kMaxViewDistance;

    // How many characters can part the grass at once, and how far away one still counts, in yards. The
    // shader's fixed loop is written to this count.
    static const int32_t kActorSlots = 16;
    static const float kActorRange;

    // Registered in WorldFrame.cpp at the world -> interface boundary, like CAntiAlias::Init. Idempotent,
    // because a world frame is built again after a UI reload.
    static void Init();

    // True when the vertex shader was patched and there is anything to upload. False leaves every caller on
    // the stock path with no behaviour change at all.
    static bool Active();

    // Rewrites a detail-doodad vertex shader's bytecode in place. Only ever called from
    // CDetailDoodad::Initialize, on the six permutations ShaderCreate just filled. A permutation whose
    // anchor line is missing, or whose reassembly fails, is left exactly stock and the reason is logged:
    // the failure mode otherwise is silently stock grass, which is unverifiable by eye.
    static void PatchVertexShader(CGxShader* shader, uint32_t permutation);

    // The once-per-frame half: the camera/player/time rows and this frame's actor gather. Called from
    // CDetailDoodad::InitializeShaderConstants, which runs once before the chunk loop.
    static void BeginFrame();

    // Uploads c14..c19 and c40..c55. Called per chunk, next to the engine's own block, so nothing drawn
    // between chunks can clobber either.
    static void UploadChunkConstants();

    // Fills in a doodad's root/tip UV band and height from the vertices its skin actually uses, and decides
    // whether it sways at all. Idempotent and cheap, so it is called lazily from the bake.
    static void AnalyzeDoodad(CDetailDoodadData* data);

    // One doodad's row for c37: {rootV, 1/(tipV-rootV), windScale, pushScale}. windScale and pushScale are
    // zero for a doodad that is too flat to move (pebbles, shells), which is what keeps stock decoration
    // from shivering.
    static void DoodadRow(CDetailDoodadData* data, float& rootV, float& invRange, float& windScale, float& pushScale);

    // The c37 row of one doodad, uploaded right before its sub-draw. The register number stays inside this
    // module so nothing else has to know which rows were borrowed.
    static void UploadDoodadRow(CDetailDoodadData* data);

    // The share of a doodad's plants worth drawing at this distance: 1.0 inside grassThinStart, falling to
    // grassThinMinShare at grassThinEnd and beyond. False when thinning is off or the window is degenerate,
    // which the caller reads as "draw everything".
    static bool ThinningShare(float distToCamera, float& share);
};

#endif