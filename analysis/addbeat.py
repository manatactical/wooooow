p = 'WoW/Source/Object/ObjectClient/Unit_C.cpp'
s = open(p, encoding='utf-8').read()

anchor = '// VERIFY: 71fe5a98 manual\n// OFFSET: 0x00717600\nint32_t CGUnit_C::FindDefaultAnimForTier(int32_t tier, uint32_t* anim) const {'
assert anchor in s

block = '''// NOT IN BINARY. Plays the one-shot flap over the glide at an irregular interval, then hands the slot back.
//
// A retail flier's forward clip is AdvFlyForwardGlide -- a glide. Played alone the mount never beats its
// wings, which reads as dead however cleanly it loops. Same shape as the wxl-mana-speedflying module's
// LevelBeat: hold the glide, play the flap through once for its own duration, return to the glide. The gap is
// randomised so the beat does not become a metronome.
uint32_t CGUnit_C::ChooseMountWingBeat(const M2Data* data, uint32_t nowMs, uint32_t glideAnim)
{
    if (!this->m_flightFlapId) {
        return glideAnim;
    }

    if (this->m_flightBeatUntilMs) {
        // Still inside the beat. Signed comparison so it survives the millisecond counter wrapping.
        if (static_cast<int32_t>(nowMs - this->m_flightBeatUntilMs) < 0) {
            return this->m_flightFlapId;
        }

        this->m_flightBeatUntilMs = 0;

        if (!this->m_flightBeatRng) {
            this->m_flightBeatRng = nowMs | 1u;
        }

        // xorshift: no seeding to get wrong, and the interval only has to look irregular.
        this->m_flightBeatRng ^= this->m_flightBeatRng << 13;
        this->m_flightBeatRng ^= this->m_flightBeatRng >> 17;
        this->m_flightBeatRng ^= this->m_flightBeatRng << 5;

        this->m_flightBeatNextMs = nowMs + 4000 + this->m_flightBeatRng % 4001;
        return glideAnim;
    }

    if (!this->m_flightBeatNextMs) {
        this->m_flightBeatNextMs = nowMs + 4000;
        return glideAnim;
    }

    if (static_cast<int32_t>(nowMs - this->m_flightBeatNextMs) < 0) {
        return glideAnim;
    }

    // Hold the beat for its own duration so the clip is never cut off part-way.
    uint32_t duration = 0;

    if (data && data->m_sequences.Data() && data->m_sequences.m_count && data->m_sequences.m_count <= 4096)
    {
        for (uint32_t i = 0; i < data->m_sequences.m_count; i++)
        {
            if (data->m_sequences[i].m_id == this->m_flightFlapId)
            {
                duration = data->m_sequences[i].m_duration;
                break;
            }
        }
    }

    this->m_flightBeatUntilMs = nowMs + (duration ? duration : 900);
    return this->m_flightFlapId;
}

'''

s = s.replace(anchor, block + anchor, 1)
open(p, 'w', encoding='utf-8').write(s)
print('added ChooseMountWingBeat')
