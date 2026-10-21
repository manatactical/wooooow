p = 'WoW/Source/Object/ObjectClient/Unit_C.cpp'
s = open(p, encoding='utf-8').read()

a = s.index('uint32_t CGUnit_C::ResolveFlightRole(const M2Data* data, EFlgithRole role)')
b = s.index('void CGUnit_C::ResolveFlightRoles(const M2Data* data)')

new = '''namespace
{
    /**
     * @brief Position of a name in the role ladder, or -1.
     *
     * The ladder is ordered best-first, so a lower index is a better clip for that role. Swim names are
     * absent from the table entirely, which is what keeps them out of every selection.
     */
    int FlightNameRank(const char* name, int role)
    {
        if (!name) return -1;

        int rank = 0;

        for (const FlightNameRole& entry : s_flightNameRoles)
        {
            if (entry.role != role) continue;
            if (std::strcmp(entry.name, name) == 0) return rank;
            rank++;
        }

        return -1;
    }

    /// True when the official name belongs to the swim family, which must never be used in the air.
    bool IsSwimName(const char* name)
    {
        return name && std::strstr(name, "Swim") != nullptr;
    }
}

uint32_t CGUnit_C::ResolveFlightRole(const M2Data* data, EFlgithRole role)
{
    if (!data || !data->m_sequences.Data() || g_animationDataDB.m_numRecords <= 0)
    {
        return 0;
    }

    const uint32_t count = data->m_sequences.m_count;

    if (!count || count > 4096)
    {
        return 0;
    }

    const uint32_t limit = static_cast<uint32_t>(g_animationDataDB.m_numRecords);

    uint32_t best = 0;
    int bestRank = 0x7FFFFFFF;
    uint32_t bestSteps = 0xFFFFFFFF;

    // Two passes over the same model ids.
    //
    // Pass 1 trusts a clip that carries its own official name: that name IS its identity, so it needs no
    // chain. Pass 2 is for modern ids with no name of their own -- those are identified by where their
    // fallback chain lands. Doing the name pass first is what stops a clip like 1542 WACurious (whose chain
    // ends on Stand) from denying a model the FlyStand it also carries.
    for (int pass = 0; pass < 2; pass++)
    {
        for (uint32_t i = 0; i < count; i++)
        {
            const uint32_t id = data->m_sequences[i].m_id;

            // Never accept a swim clip, however the resolution reached it.
            const char* own = FlightIdName(id);

            if (IsSwimName(own))
            {
                continue;
            }

            if (pass == 0)
            {
                const int rank = FlightNameRank(own, static_cast<int>(role));

                if (rank >= 0 && rank < bestRank)
                {
                    bestRank = rank;
                    best = id;
                    bestSteps = 0;
                }

                continue;
            }

            if (own)
            {
                // Named already handled in pass 1; do not follow its chain.
                continue;
            }

            uint32_t cur = id;

            for (uint32_t step = 1; step <= limit; step++)
            {
                AnimationDataRec* rec = g_animationDataDB.GetRecord(cur);

                if (!rec)
                {
                    break;
                }

                const uint32_t next = static_cast<uint32_t>(rec->m_fallback);

                if (next == cur)
                {
                    break;
                }

                cur = next;

                const char* landed = FlightIdName(cur);

                // A chain that runs into a swim clip is not a flight movement; treat it as a dead end.
                if (IsSwimName(landed))
                {
                    break;
                }

                const int rank = FlightNameRank(landed, static_cast<int>(role));

                if (rank < 0)
                {
                    continue;
                }

                if (rank < bestRank || (rank == bestRank && step < bestSteps))
                {
                    bestRank = rank;
                    bestSteps = step;
                    best = id;
                }

                break;
            }
        }

        // A named clip of the best possible rank is final; no chain match can beat it.
        if (best && bestRank == 0)
        {
            return best;
        }
    }

    return best;
}

'''

s = s[:a] + new + s[b:]
open(p, 'w', encoding='utf-8').write(s)
print('resolver rewritten with ladders + swim ban')
