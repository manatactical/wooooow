#ifndef ENGINE_SOURCE_MODEL2_M2BONEFOLLOW_H
#define ENGINE_SOURCE_MODEL2_M2BONEFOLLOW_H

#include <cstdint>
#include <vector>

class CM2Model;
class CM2Shared;

// Not in the original binary. Part of the Equip Plus port (see docs/EQUIP-PLUS.md).
//
// A retail "collection" armour piece is one skinned model authored against a retail skeleton, attached to the character
// so it can stand in for a chest, a pair of boots or a cloak. The client draws an attached model rigidly at one
// attachment point, which is right for a pauldron and wrong for a body piece: the piece has to move with the character's
// own bones. This makes a model do that. Each frame, after the model has animated, its bone matrices are overwritten
// with the matching bone matrices of the model it is attached to.
//
// Bones are matched in four passes, in order: bone name CRC, key bone id (the later pass overrides the earlier), the
// parent's id lookup table for the rest, and finally a bone with no match of its own takes its parent's. A bone the
// pass leaves unmatched keeps its own animation.
//
// A model can also be set to follow the attach parent's animation: whenever the parent starts a sequence, the model starts
// the same one (if it has it). That is for accessories that carry their own animation set and should move with the
// wearer without being skinned to it.
struct M2BoneFollow {
    enum {
        COPY_BONES = 0x1,
        SYNC_SEQUENCE = 0x2,
    };

    uint32_t flags = 0;

    // Whoever attached the model can stamp it, to recognise it later without trusting a pointer that may have been
    // freed and reused.
    uint64_t tag = 0;

    // Fit options. With pivots, each piece bone moves by the gap between the character bone's pivot and its own; with
    // scale, the piece is also sized by the ratio of the two skeletons, measured over the bones matched by name or id.
    bool retargetPivots = false;
    bool retargetScale = false;

    // A model-space nudge and size applied before the character's matrix (WXLHelmOffsets.csv). Scale 1 and a zero
    // offset do nothing.
    float offset[3] = {0.0f, 0.0f, 0.0f};
    float userScale = 1.0f;

    // The bone map, built once both models are loaded and again if either is replaced.
    const CM2Shared* builtParent = nullptr;
    const CM2Shared* builtSelf = nullptr;
    bool built = false;
    std::vector<uint16_t> map;
    std::vector<float> delta;
    float fitScale = 1.0f;

    // Sequence sync: the last sequence and start anchor seen on the parent.
    bool primed = false;
    uint32_t lastSequence = 0xFFFFFFFF;
    uint32_t lastStart = 0;
};

// Gives the model a bone follow, replacing any it has. Returns it so the caller can set the options.
M2BoneFollow* M2BoneFollowCreate(CM2Model* model, uint32_t flags);

// Called from CM2Model::AnimateMT after the model's own bones are done.
void M2BoneFollowApply(CM2Model* model);

// Called from CM2Model::AnimateST. Single-threaded because it starts a bone sequence.
void M2BoneFollowSyncSequence(CM2Model* model);

void M2BoneFollowDestroy(CM2Model* model);

#endif
