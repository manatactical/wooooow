p = 'WoW/Source/Object/ObjectClient/Unit_C.cpp'
s = open(p, encoding='utf-8').read()

old = '''                    mountAnim = this->m_flightRoleIds[role];
'''

new = '''                    mountAnim = this->m_flightRoleIds[role];

                    // TEMPORARY PROBE: the role actually chosen, once per distinct input.
                    {
                        static uint32_t lastRole = 0xFFFFFFFF;
                        static uint32_t lastFlags = 0xFFFFFFFF;

                        if (moveFlags != lastFlags || role != lastRole) {
                            lastFlags = moveFlags;
                            lastRole = role;

                            char pb[240];
                            SStrPrintf(pb, sizeof(pb), "mountrole: flags=0x%x role=%u anim=%u (hover=%u fwd=%u back=%u L=%u R=%u)",
                                       moveFlags, role, mountAnim,
                                       this->m_flightRoleIds[FLYROLE_HOVER],
                                       this->m_flightRoleIds[FLYROLE_FORWARD],
                                       this->m_flightRoleIds[FLYROLE_BACKWARD],
                                       this->m_flightRoleIds[FLYROLE_LEFT],
                                       this->m_flightRoleIds[FLYROLE_RIGHT]);
                            extern void ProbeLogMount(const char*);
                            ProbeLogMount(pb);
                        }
                    }
'''

assert old in s
s = s.replace(old, new, 1)
open(p, 'w', encoding='utf-8').write(s)
print('role probe added')
