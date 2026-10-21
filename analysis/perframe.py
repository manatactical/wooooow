p = 'WoW/Source/Object/ObjectClient/Unit_C.cpp'
s = open(p, encoding='utf-8').read()

anchor = '''    this->CGObject_C::PreAnimate(frame);
'''

new = '''    // NOT IN BINARY. Per-frame mount animation for a flying unit.
    //
    // The mount clip used to be chosen only from PlayBaseAnimation, which runs on discrete animation
    // changes. A mounted player model has no flight cycle, so while flying it picks ANIM_SWIM and that
    // predicate rejected the frame -- the mount branch therefore almost never ran, and the mount kept
    // whatever clip it started with. That is why a flying mount only ever glided, whatever the input.
    //
    // PreAnimate runs every frame, so the mount is driven from here instead: the rider's movement decides
    // the role, the role decides the clip, and the clip is set only when it actually changes.
    this->UpdateMountFlightAnimation(time);

    this->CGObject_C::PreAnimate(frame);
'''

assert anchor in s
s = s.replace(anchor, new, 1)

# Add the method next to the other flight helpers.
anchor2 = '''// NOT IN BINARY. Plays the one-shot flap over the glide at an irregular interval, then hands the slot back.'''

impl = '''// NOT IN BINARY. Drives a flying mount's animation every frame from the rider's movement.
//
// See the call site in PreAnimate for why this cannot be left to PlayBaseAnimation.
void CGUnit_C::UpdateMountFlightAnimation(uint32_t time)
{
    if (!this->m_mountModel) {
        return;
    }

    const uint32_t moveFlags = this->m_movementData.m_flags;

    if (!(moveFlags & MOVEMENTFLAG_FLYING)) {
        return;
    }

    if (!this->m_mountModel->IsLoaded(0, 0)) {
        return;
    }

    const M2Data* mountData = this->m_mountModel->m_shared ? this->m_mountModel->m_shared->m_data : nullptr;

    if (!mountData) {
        return;
    }

    this->ResolveFlightRoles(mountData);

    uint32_t role = FLYROLE_FORWARD;

    if (moveFlags & MOVEMENTFLAG_STRAFE_LEFT) {
        role = FLYROLE_LEFT;
    } else if (moveFlags & MOVEMENTFLAG_STRAFE_RIGHT) {
        role = FLYROLE_RIGHT;
    } else if (moveFlags & MOVEMENTFLAG_BACKWARD) {
        role = FLYROLE_BACKWARD;
    } else if (!(moveFlags & MOVEMENTFLAG_FORWARD)) {
        role = FLYROLE_HOVER;
    }

    uint32_t anim = this->m_flightRoleIds[role];

    // No clip for this role: keep travelling rather than freezing.
    if (!anim) {
        anim = this->m_flightRoleIds[FLYROLE_FORWARD];
    }

    if (!anim) {
        anim = this->m_flightRoleIds[FLYROLE_HOVER];
    }

    if (!anim) {
        return;
    }

    // Beat only over forward travel: hover already loops a standing clip, and a strafe or reverse is a short
    // move whose own clip should not be interrupted.
    if (role == FLYROLE_FORWARD && this->m_flightFlapId) {
        anim = this->ChooseMountWingBeat(mountData, time, anim);
    } else {
        this->m_flightBeatUntilMs = 0;
    }

    // Only touch the bone sequence when the clip actually changes: re-issuing the same id restarts the clip
    // every frame, which would freeze it at its first frame.
    if (this->m_mountFlightApplied == anim) {
        return;
    }

    this->m_mountFlightApplied = anim;
    this->SetKeyBoneSequence(this->m_mountModel, 0xFFFFFFFF, anim, 0xFFFFFFFF, 0, 1.0f, 1, 1, 0);

    char pb[200];
    SStrPrintf(pb, sizeof(pb), "mountfly: flags=0x%x role=%u anim=%u", moveFlags, role, anim);
    extern void ProbeLogMount(const char*);
    ProbeLogMount(pb);
}

'''

assert anchor2 in s
s = s.replace(anchor2, impl + anchor2, 1)

open(p, 'w', encoding='utf-8').write(s)
print('per-frame driver added')
