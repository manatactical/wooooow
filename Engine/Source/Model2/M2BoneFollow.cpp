#include "Engine/Source/Model2/M2BoneFollow.h"
#include "Engine/Source/Model2/M2Data.h"
#include "Engine/Source/Model2/M2Model.h"
#include "Engine/Source/Model2/M2Shared.h"
#include "Engine/Source/Tempest/C44Matrix.h"

#include <cstring>

// Not in the original binary.

M2BoneFollow* M2BoneFollowCreate(CM2Model* model, uint32_t flags) {
    M2BoneFollowDestroy(model);

    M2BoneFollow* follow = new M2BoneFollow();
    follow->flags = flags;
    model->m_boneFollow = follow;

    return follow;
}

void M2BoneFollowDestroy(CM2Model* model) {
    if (model->m_boneFollow) {
        delete model->m_boneFollow;
        model->m_boneFollow = nullptr;
    }
}

static const uint16_t kNoBone = 0xFFFF;

// Maps every bone of `self` onto a bone of `parent`, or kNoBone.
static void BuildMap(M2BoneFollow* follow, const M2Data* self, const M2Data* parent) {
    const uint32_t selfCount = self->m_bones.m_count;
    const uint32_t parentCount = parent->m_bones.m_count;

    follow->map.assign(selfCount, kNoBone);
    follow->delta.assign(selfCount * 3, 0.0f);
    follow->fitScale = 1.0f;

    if (!selfCount || !parentCount) {
        return;
    }

    // Pass 1: name CRC. Retail names its bones; a stock 3.3.5a model keeps something else in this field, so a zero never
    // matches.
    for (uint32_t i = 0; i < selfCount; i++) {
        const uint32_t crc = self->m_bones[i].m_boneNameCRC;

        if (!crc) {
            continue;
        }

        for (uint32_t j = 0; j < parentCount; j++) {
            if (parent->m_bones[j].m_boneNameCRC == crc) {
                follow->map[i] = static_cast<uint16_t>(j);
                break;
            }
        }
    }

    // Pass 2: key bone id. Overrides the name match, like the original extension did.
    for (uint32_t i = 0; i < selfCount; i++) {
        const int32_t key = static_cast<int32_t>(self->m_bones[i].m_boneId);

        if (key < 0) {
            continue;
        }

        for (uint32_t j = 0; j < parentCount; j++) {
            if (static_cast<int32_t>(parent->m_bones[j].m_boneId) == key) {
                follow->map[i] = static_cast<uint16_t>(j);
                break;
            }
        }
    }

    // Pass 3: the parent's id lookup table, for bones still unmatched.
    for (uint32_t i = 0; i < selfCount; i++) {
        if (follow->map[i] != kNoBone) {
            continue;
        }

        const int32_t key = static_cast<int32_t>(self->m_bones[i].m_boneId);

        if (key < 0 || static_cast<uint32_t>(key) >= parent->m_boneIndicesById.m_count) {
            continue;
        }

        const uint16_t index = parent->m_boneIndicesById[key];

        if (index < parentCount) {
            follow->map[i] = index;
        }
    }

    // Remember which matches were direct, for the fit.
    std::vector<uint8_t> direct(selfCount);

    for (uint32_t i = 0; i < selfCount; i++) {
        direct[i] = follow->map[i] != kNoBone;
    }

    // Pass 4: a bone with no match takes its parent's. Accessory chains in collection models run deeper than eight bones,
    // so repeat until nothing changes.
    for (uint32_t pass = 0; pass < selfCount; pass++) {
        bool changed = false;

        for (uint32_t i = 0; i < selfCount; i++) {
            if (follow->map[i] != kNoBone) {
                continue;
            }

            const uint16_t parentIndex = self->m_bones[i].m_parentIndex;

            if (parentIndex < selfCount && follow->map[parentIndex] != kNoBone) {
                follow->map[i] = follow->map[parentIndex];
                changed = true;
            }
        }

        if (!changed) {
            break;
        }
    }

    // The fit. fitScale is the client skeleton's size over the piece's, least squares over the directly matched bones'
    // pivots. delta is, per bone, what to add to a vertex before the character's matrix.
    double selfMean[3] = {};
    double parentMean[3] = {};
    uint32_t n = 0;

    for (uint32_t i = 0; i < selfCount; i++) {
        if (!direct[i] || follow->map[i] >= parentCount) {
            continue;
        }

        const C3Vector& ps = self->m_bones[i].m_pivot;
        const C3Vector& pp = parent->m_bones[follow->map[i]].m_pivot;
        selfMean[0] += ps.x; selfMean[1] += ps.y; selfMean[2] += ps.z;
        parentMean[0] += pp.x; parentMean[1] += pp.y; parentMean[2] += pp.z;
        n++;
    }

    if (n >= 4) {
        for (int k = 0; k < 3; k++) {
            selfMean[k] /= n;
            parentMean[k] /= n;
        }

        double num = 0.0;
        double den = 0.0;

        for (uint32_t i = 0; i < selfCount; i++) {
            if (!direct[i] || follow->map[i] >= parentCount) {
                continue;
            }

            const float ps[3] = {self->m_bones[i].m_pivot.x, self->m_bones[i].m_pivot.y, self->m_bones[i].m_pivot.z};
            const C3Vector& pv = parent->m_bones[follow->map[i]].m_pivot;
            const float pp[3] = {pv.x, pv.y, pv.z};

            for (int k = 0; k < 3; k++) {
                num += (ps[k] - selfMean[k]) * (pp[k] - parentMean[k]);
                den += (ps[k] - selfMean[k]) * (ps[k] - selfMean[k]);
            }
        }

        if (den > 1e-6) {
            const double fit = num / den;

            if (fit > 0.25 && fit < 4.0) {
                follow->fitScale = static_cast<float>(fit);
            }
        }
    }

    const float scale = follow->retargetScale ? follow->fitScale : 1.0f;

    for (uint32_t i = 0; i < selfCount; i++) {
        float d[3] = {0.0f, 0.0f, 0.0f};

        if ((follow->retargetPivots || follow->retargetScale) && follow->map[i] < parentCount) {
            const C3Vector& ps = self->m_bones[i].m_pivot;
            const C3Vector& pp = parent->m_bones[follow->map[i]].m_pivot;
            d[0] = pp.x - scale * ps.x;
            d[1] = pp.y - scale * ps.y;
            d[2] = pp.z - scale * ps.z;
        }

        // v' = S (scale v + d) + t = (S scale) v + (S d + t)
        for (int k = 0; k < 3; k++) {
            follow->delta[i * 3 + k] = follow->userScale * d[k] + follow->offset[k];
        }
    }

    // Folded into one factor: the fit's, then the user's.
    follow->fitScale = scale * follow->userScale;
}

void M2BoneFollowApply(CM2Model* model) {
    M2BoneFollow* follow = model->m_boneFollow;

    if (!(follow->flags & M2BoneFollow::COPY_BONES)) {
        return;
    }

    CM2Model* parent = model->m_attachParent;

    if (!parent || !(parent->m_fFlags & 1) || !parent->m_boneMatrices || !model->m_boneMatrices) {
        return;
    }

    if (!follow->built || follow->builtParent != parent->m_shared || follow->builtSelf != model->m_shared) {
        BuildMap(follow, model->m_shared->m_data, parent->m_shared->m_data);
        follow->builtParent = parent->m_shared;
        follow->builtSelf = model->m_shared;
        follow->built = true;
    }

    const uint32_t selfCount = model->m_shared->m_data->m_bones.m_count;
    const uint32_t parentCount = parent->m_shared->m_data->m_bones.m_count;

    if (follow->map.size() != selfCount) {
        return;
    }

    const float scale = follow->fitScale;

    for (uint32_t i = 0; i < selfCount; i++) {
        const uint16_t source = follow->map[i];

        if (source >= parentCount) {
            continue;
        }

        C44Matrix& out = model->m_boneMatrices[i];
        out = parent->m_boneMatrices[source];

        const float* d = &follow->delta[i * 3];

        if (d[0] != 0.0f || d[1] != 0.0f || d[2] != 0.0f) {
            // The translation row gains d through the rotation rows, taken before they are scaled.
            out.d0 += d[0] * out.a0 + d[1] * out.b0 + d[2] * out.c0;
            out.d1 += d[0] * out.a1 + d[1] * out.b1 + d[2] * out.c1;
            out.d2 += d[0] * out.a2 + d[1] * out.b2 + d[2] * out.c2;
        }

        if (scale != 1.0f) {
            out.a0 *= scale; out.a1 *= scale; out.a2 *= scale;
            out.b0 *= scale; out.b1 *= scale; out.b2 *= scale;
            out.c0 *= scale; out.c1 *= scale; out.c2 *= scale;
        }
    }
}

void M2BoneFollowSyncSequence(CM2Model* model) {
    M2BoneFollow* follow = model->m_boneFollow;

    if (!(follow->flags & M2BoneFollow::SYNC_SEQUENCE)) {
        return;
    }

    CM2Model* parent = model->m_attachParent;

    if (!parent || !(parent->m_fFlags & 1) || !parent->m_bones) {
        return;
    }

    // Bone 0 is what the primary slot (-1) resolves to. Its start anchor is rewritten every time the engine re-arms a
    // sequence on it, including a loop restart that keeps the same id, so watching the anchor as well as the id catches a
    // long channel starting over.
    const uint32_t sequence = parent->m_bones[0].m_sequenceId;
    const uint32_t start = parent->m_bones[0].m_sequence.m_startTime;

    if (follow->primed && sequence == follow->lastSequence && start == follow->lastStart) {
        return;
    }

    follow->primed = true;
    follow->lastSequence = sequence;
    follow->lastStart = start;

    if (!model->HasSequence(sequence)) {
        return;
    }

    uint32_t variation = 0;
    parent->GetBoneSequenceAnimId(0xFFFFFFFF, &variation);
    model->SetBoneSequence(0xFFFFFFFF, sequence, variation, 0, 1.0f, 0, 1);
}
