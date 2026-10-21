import io

p = 'WoW/Source/Object/ObjectClient/Unit_C.cpp'
s = open(p, encoding='utf-8').read()

anchor = '// VERIFY: 71fe5a98 manual\n// OFFSET: 0x00717600\nint32_t CGUnit_C::FindDefaultAnimForTier(int32_t tier, uint32_t* anim) const {'
assert anchor in s

block = r'''namespace
{
    /**
     * @brief Which flight role each official animation name satisfies.
     *
     * Names are wowdev.wiki's /M2/AnimationList, which is what makes a mount's ids interpretable: the numbers
     * mean nothing on their own, but every one of them falls back onto a stock id whose official name does.
     * Several names are accepted per role because the stock, Mount*, Fly* and AdvFly* families each spell the
     * same movement differently.
     *
     * Note the direction of the left/right pair: FlyRunLeft is 322 and FlyRunRight is 321.
     */
    struct FlightNameRole
    {
        const char* name;
        int         role;
    };

    constexpr int kRoleHover    = 0;
    constexpr int kRoleForward  = 1;
    constexpr int kRoleBackward = 2;
    constexpr int kRoleLeft     = 3;
    constexpr int kRoleRight    = 4;

    const FlightNameRole s_flightNameRoles[] = {
        { "FlyStand",               kRoleHover },
        { "FlyStop",                kRoleHover },
        { "AdvFlyForwardGlide",     kRoleForward },
        { "AdvFlyForwardGlideSlow", kRoleForward },
        { "AdvFlyForwardGlideFast", kRoleForward },
        { "AdvFlyForward",          kRoleForward },
        { "FlyRun",                 kRoleForward },
        { "MountFlightBackwards",   kRoleBackward },
        { "RunBackwards",           kRoleBackward },
        { "AdvFlyLeft",             kRoleLeft },
        { "FlyRunLeft",             kRoleLeft },
        { "AdvFlyRight",            kRoleRight },
        { "FlyRunRight",            kRoleRight },
    };

    constexpr uint32_t kAdvFlyGlide      = 1532;
    constexpr uint32_t kAdvFlyGlideSlow  = 1722;
    constexpr uint32_t kAdvFlyGlideFast  = 1724;
    constexpr uint32_t kAdvFlyLeft       = 1520;
    constexpr uint32_t kAdvFlyRight      = 1522;
    constexpr uint32_t kAdvFlyDown       = 1530;
    constexpr uint32_t kAdvFlyFlapUp     = 1702;

    constexpr uint32_t kFlapWave[] = { 1706, 1680, 1702, 1726, 1638 };

    /**
     * @brief Official name of a sequence id, or null.
     *
     * Only the ids in the flight vocabulary are listed. That is deliberate: this resolver only ever asks
     * "does this chain end on a flight movement", so the other ~1760 names would be dead weight.
     */
    const char* FlightIdName(uint32_t id)
    {
        switch (id)
        {
            case 229: return "FlyStand";
            case 232: return "FlyStop";
            case 233: return "FlyWalk";
            case 234: return "FlyRun";
            case 321: return "FlyRunRight";
            case 322: return "FlyRunLeft";
            case 364: return "FlyFly";
            case 135: return "Fly";
            case 550: return "MountFlightBackwards";
            case 1146: return "RunBackwards";
            case 1530: return "AdvFlyDown";
            case 1532: return "AdvFlyForwardGlide";
            case 1520: return "AdvFlyLeft";
            case 1522: return "AdvFlyRight";
            case 1524: return "AdvFlyForward";
            case 1558: return "AdvFlyRollEnd";
            case 1638: return "AdvFlyFlap";
            case 1678: return "AdvFlyDownStart";
            case 1680: return "AdvFlyFlapBig";
            case 1702: return "AdvFlyFlapUp";
            case 1704: return "AdvFlySlowFall";
            case 1706: return "AdvFlyFlapFoward";
            case 1722: return "AdvFlyForwardGlideSlow";
            case 1724: return "AdvFlyForwardGlideFast";
            case 1726: return "AdvFlySecondFlapUp";
            default: return nullptr;
        }
    }

    /// Role this name satisfies, or -1.
    int FlightNameToRole(const char* name)
    {
        if (!name) return -1;

        for (const FlightNameRole& entry : s_flightNameRoles)
        {
            if (std::strcmp(entry.name, name) == 0)
            {
                return entry.role;
            }
        }

        return -1;
    }
}

bool CGUnit_C::ModelHasSequence(const M2Data* data, uint32_t id)
{
    if (!data || !data->m_sequences.Data())
    {
        return false;
    }

    const uint32_t count = data->m_sequences.m_count;

    // A modern mount carries a few hundred sequences; anything beyond this is a corrupt count, not a model.
    if (!count || count > 4096)
    {
        return false;
    }

    for (uint32_t i = 0; i < count; i++)
    {
        if (data->m_sequences[i].m_id == id)
        {
            return true;
        }
    }

    return false;
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

    // The retail advanced-flying block, when the model carries it, is the authoritative source for forward
    // and lateral movement: a modern mount's plain flight ids are only the stock fallbacks.
    const bool advanced = CGUnit_C::ModelHasSequence(data, kAdvFlyGlide) ||
                          CGUnit_C::ModelHasSequence(data, kAdvFlyGlideSlow) ||
                          CGUnit_C::ModelHasSequence(data, kAdvFlyFlapUp) ||
                          CGUnit_C::ModelHasSequence(data, kAdvFlyDown);

    if (advanced)
    {
        const uint32_t count2 = count;

        auto pick = [&](const uint32_t* ids, uint32_t n) -> uint32_t
        {
            for (uint32_t k = 0; k < n; k++)
            {
                if (CGUnit_C::ModelHasSequence(data, ids[k])) return ids[k];
            }
            return 0;
        };

        (void)count2;

        switch (role)
        {
            case FLYROLE_FORWARD:
            {
                const uint32_t ids[] = { kAdvFlyGlide, kAdvFlyGlideSlow, kAdvFlyGlideFast };
                const uint32_t got = pick(ids, 3);
                if (got) return got;
                break;
            }
            case FLYROLE_LEFT:
            {
                const uint32_t ids[] = { kAdvFlyLeft };
                const uint32_t got = pick(ids, 1);
                if (got) return got;
                break;
            }
            case FLYROLE_RIGHT:
            {
                const uint32_t ids[] = { kAdvFlyRight };
                const uint32_t got = pick(ids, 1);
                if (got) return got;
                break;
            }
            default:
                break;
        }

        // Hover and backwards are not part of the AdvFly block; they fall through to the chain walk.
    }

    // Chain walk: the model's own id whose fallback chain reaches a stock sequence with this role, in the
    // fewest hops. Fewest hops matters -- a spell animation that merely falls back to FlyStand would
    // otherwise win the hover role over FlyStand itself.
    const uint32_t limit = static_cast<uint32_t>(g_animationDataDB.m_numRecords);
    uint32_t best = 0;
    uint32_t bestSteps = 0xFFFFFFFF;

    for (uint32_t i = 0; i < count; i++)
    {
        const uint32_t id = data->m_sequences[i].m_id;

        // A direct hit is zero hops and therefore always wins.
        if (FlightNameToRole(FlightIdName(id)) == static_cast<int>(role))
        {
            return id;
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

            if (FlightNameToRole(FlightIdName(cur)) == static_cast<int>(role))
            {
                if (step < bestSteps)
                {
                    bestSteps = step;
                    best = id;
                }
                break;
            }
        }
    }

    return best;
}

void CGUnit_C::ResolveFlightRoles(const M2Data* data)
{
    if (this->m_flightRolesFor == data)
    {
        return;
    }

    this->m_flightRolesFor = data;

    for (uint32_t i = 0; i < FLYROLE_COUNT; i++)
    {
        this->m_flightRoleIds[i] = 0;
    }

    this->m_flightFlapId = 0;
    this->m_flightBeatUntilMs = 0;
    this->m_flightBeatNextMs = 0;

    if (!data)
    {
        return;
    }

    this->m_flightRoleIds[FLYROLE_HOVER]    = CGUnit_C::ResolveFlightRole(data, FLYROLE_HOVER);
    this->m_flightRoleIds[FLYROLE_FORWARD]  = CGUnit_C::ResolveFlightRole(data, FLYROLE_FORWARD);
    this->m_flightRoleIds[FLYROLE_BACKWARD] = CGUnit_C::ResolveFlightRole(data, FLYROLE_BACKWARD);
    this->m_flightRoleIds[FLYROLE_LEFT]     = CGUnit_C::ResolveFlightRole(data, FLYROLE_LEFT);
    this->m_flightRoleIds[FLYROLE_RIGHT]    = CGUnit_C::ResolveFlightRole(data, FLYROLE_RIGHT);

    // The flap that punctuates a glide. Only meaningful on a model whose forward clip is a glide.
    for (uint32_t id : kFlapWave)
    {
        if (CGUnit_C::ModelHasSequence(data, id))
        {
            this->m_flightFlapId = id;
            break;
        }
    }

    MODERN_ASSETS_LOG_INFO("flight: roles hover=%u forward=%u back=%u left=%u right=%u flap=%u",
                           this->m_flightRoleIds[FLYROLE_HOVER], this->m_flightRoleIds[FLYROLE_FORWARD],
                           this->m_flightRoleIds[FLYROLE_BACKWARD], this->m_flightRoleIds[FLYROLE_LEFT],
                           this->m_flightRoleIds[FLYROLE_RIGHT], this->m_flightFlapId);
}

'''

s = s.replace(anchor, block + anchor, 1)
open(p, 'w', encoding='utf-8').write(s)
print('inserted')
