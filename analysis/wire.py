import io

p = 'WoW/Source/Object/ObjectClient/Unit_C.cpp'
s = open(p, encoding='utf-8').read()

old_start = '''    if (this->m_mountModel) {
        if (IsFullBodyAnim(anim)) {
            mountSlot = newSlot;

            // NOT IN BINARY. The mount's animation was always derived from the *rider's* choice: anim is
            // whatever ChooseMovementAnim picked for the player's own model, and it was then translated
            // through the animation table for the mount. A player model has no flight cycle, so while
            // flying it picks ANIM_SWIM (42) -- and the mount was told to swim too. That is why flying
            // mounts paddle through the air.
            //
            // A mount that authors its own air sequence should play that sequence. The ids are read from
            // the mount's own file, so whichever air cycle it actually carries is the one used, and a
            // mount with none keeps the translated behaviour.
            uint32_t mountAnim = 0xFFFFFFFF;

            if (this->m_movementData.m_flags & MOVEMENTFLAG_FLYING) {
                const M2Data* mountData = this->m_mountModel->m_shared ? this->m_mountModel->m_shared->m_data : nullptr;

                if (mountData) {
                    const uint32_t airIds[] = { ANIM_FLY_RUN, ANIM_FLY_WALK, ANIM_FLY_STAND };

                    for (uint32_t airId : airIds) {
                        if (this->m_mountModel->HasSequence(mountData, airId)) {
                            mountAnim = airId;
                            break;
                        }
                    }
                }
            }

            mountSlot.m_sequenceId = mountAnim != 0xFFFFFFFF
                                   ? mountAnim
                                   : this->ResolveAnimForModel(anim, this->m_mountModel);
        }
'''

new = '''    if (this->m_mountModel) {
        if (IsFullBodyAnim(anim)) {
            mountSlot = newSlot;

            // NOT IN BINARY. The mount's animation used to be derived from the *rider's* choice: anim is
            // whatever ChooseMovementAnim picked for the player's own model, then translated through the
            // animation table for the mount. A mounted player model has no flight cycle, so in the air it
            // picks ANIM_SWIM and the mount was told to swim too -- which is why flying mounts paddled.
            //
            // The mount now answers for itself, from its own sequences, per flight movement.
            uint32_t mountAnim = 0xFFFFFFFF;

            if (this->m_movementData.m_flags & MOVEMENTFLAG_FLYING) {
                const M2Data* mountData = this->m_mountModel->m_shared ? this->m_mountModel->m_shared->m_data : nullptr;

                if (mountData) {
                    this->ResolveFlightRoles(mountData);

                    const uint32_t moveFlags = this->m_movementData.m_flags;
                    uint32_t role = FLYROLE_FORWARD;

                    // Strafe and reverse are deliberate short moves and each has its own authored clip;
                    // with no forward input at all the mount is hovering, not travelling.
                    if (moveFlags & MOVEMENTFLAG_STRAFE_LEFT) {
                        role = FLYROLE_LEFT;
                    } else if (moveFlags & MOVEMENTFLAG_STRAFE_RIGHT) {
                        role = FLYROLE_RIGHT;
                    } else if (moveFlags & MOVEMENTFLAG_BACKWARD) {
                        role = FLYROLE_BACKWARD;
                    } else if (!(moveFlags & MOVEMENTFLAG_FORWARD)) {
                        role = FLYROLE_HOVER;
                    }

                    mountAnim = this->m_flightRoleIds[role];

                    // A model that has no clip for the requested role keeps travelling rather than freezing:
                    // fall back through forward, then hover.
                    if (!mountAnim) {
                        mountAnim = this->m_flightRoleIds[FLYROLE_FORWARD];
                    }

                    if (!mountAnim) {
                        mountAnim = this->m_flightRoleIds[FLYROLE_HOVER];
                    }

                    // A glide that never beats its wings reads as dead. Only forward travel is punctuated:
                    // hover already loops a standing clip, and a strafe or reverse is a short move whose own
                    // clip should not be interrupted.
                    if (role == FLYROLE_FORWARD && this->m_flightFlapId && mountAnim) {
                        mountAnim = this->ChooseMountWingBeat(mountData, OsGetAsyncTimeMs(), mountAnim);
                    } else {
                        this->m_flightBeatUntilMs = 0;
                    }
                }
            }

            mountSlot.m_sequenceId = mountAnim != 0xFFFFFFFF
                                   ? mountAnim
                                   : this->ResolveAnimForModel(anim, this->m_mountModel);
        }
'''

assert old_start in s
s = s.replace(old_start, new, 1)
open(p, 'w', encoding='utf-8').write(s)
print('wired')
