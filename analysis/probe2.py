p = 'WoW/Source/Object/ObjectClient/Unit_C.cpp'
s = open(p, encoding='utf-8').read()

start = s.index('    if (this->m_mountModel && (this->m_movementData.m_flags & MOVEMENTFLAG_FLYING)) {\n        // TEMPORARY PROBE')
end = s.index('    if (updateMount && this->m_mountModel && mountSlot.m_sequenceId != 0xFFFFFFFF', start)

new = '''    if (this->m_mountModel && (this->m_movementData.m_flags & MOVEMENTFLAG_FLYING)) {
        // TEMPORARY PROBE: log once per distinct input combination, so every direction is seen and the log
        // stays readable. up = the movement bits only, which is what the role selection keys off.
        const uint32_t up = this->m_movementData.m_flags & 0xF;
        static uint32_t seenInputs = 0;
        static uint32_t lastUp = 0xFFFFFFFF;

        if (up != lastUp) {
            lastUp = up;

            if (up < 32) {
                seenInputs |= (1u << up);
            }

            char pb[240];
            SStrPrintf(pb, sizeof(pb), "mountanim: up=0x%x (F=%d B=%d L=%d R=%d) want=%u cur=%u upd=%u",
                       up, (up & 1) ? 1 : 0, (up & 2) ? 1 : 0, (up & 4) ? 1 : 0, (up & 8) ? 1 : 0,
                       mountSlot.m_sequenceId, mount.m_sequenceId, updateMount ? 1u : 0u);
            extern void ProbeLogMount(const char*);
            ProbeLogMount(pb);
        }
    }

'''

s = s[:start] + new + s[end:]
open(p, 'w', encoding='utf-8').write(s)
print('probe replaced')
