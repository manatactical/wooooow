p = 'WoW/Source/Object/ObjectClient/Unit_C.cpp'
s = open(p, encoding='utf-8').read()

old = '''    for (uint32_t i = 0; i < count; i++)
    {
        const uint32_t id = data->m_sequences[i].m_id;

        // A direct hit is zero hops and therefore always wins.
        if (FlightNameToRole(FlightIdName(id)) == static_cast<int>(role))
        {
            return id;
        }
'''

new = '''    // A clip that carries its own official name is authoritative for that name -- no chain needed. The
    // chain walk is only for modern ids that have no name of their own. Walking first is what made a mount
    // with 1542 WACurious resolve no hover at all: that id's chain ends on Stand, never FlyStand, even
    // though the model carries a perfectly good FlyStand elsewhere.
    for (uint32_t i = 0; i < count; i++)
    {
        const uint32_t id = data->m_sequences[i].m_id;

        if (FlightNameToRole(FlightIdName(id)) == static_cast<int>(role))
        {
            return id;
        }
    }

    for (uint32_t i = 0; i < count; i++)
    {
        const uint32_t id = data->m_sequences[i].m_id;
'''

assert old in s
s = s.replace(old, new, 1)

# Also widen the vocabulary: these names carry the same role and were missing.
old_names = '''            case 552: return "MountFlightLeft";
            case 554: return "MountFlightRight";'''
new_names = '''            case 552: return "MountFlightLeft";
            case 554: return "MountFlightRight";
            case 546: return "MountSwimWalkBackwards";
            case 534: return "MountSwimBackwards";
            case 45:  return "SwimBackwards";'''
assert old_names in s
s = s.replace(old_names, new_names, 1)

old_tbl = '''        { "MountFlightBackwards",   kRoleBackward },
        { "RunBackwards",           kRoleBackward },'''
new_tbl = '''        { "MountFlightBackwards",   kRoleBackward },
        { "MountSwimWalkBackwards", kRoleBackward },
        { "MountSwimBackwards",     kRoleBackward },
        { "RunBackwards",           kRoleBackward },
        { "Walkbackwards",          kRoleBackward },'''
assert old_tbl in s
s = s.replace(old_tbl, new_tbl, 1)

# Forward: FlyRun first, per request.
old_fwd = '''        { "AdvFlyForwardGlide",     kRoleForward },
        { "AdvFlyForwardGlideSlow", kRoleForward },
        { "AdvFlyForwardGlideFast", kRoleForward },
        { "AdvFlyForward",          kRoleForward },
        { "FlyRun",                 kRoleForward },'''
new_fwd = '''        // FlyRun is the stock WotLK flight run cycle: continuous wingbeats, which is what a mount travelling
        // in the air should look like. The AdvFly glides are held poses and read as hanging motionless, so
        // they are only a fallback for models that carry no FlyRun.
        { "FlyRun",                 kRoleForward },
        { "AdvFlyForwardGlide",     kRoleForward },
        { "AdvFlyForwardGlideSlow", kRoleForward },
        { "AdvFlyForwardGlideFast", kRoleForward },
        { "AdvFlyForward",          kRoleForward },'''
assert old_fwd in s
s = s.replace(old_fwd, new_fwd, 1)

# Hover: accept the stock stand names too, for a model with no FlyStand at all.
old_hover = '''        { "FlyStand",               kRoleHover },
        { "FlyStop",                kRoleHover },'''
new_hover = '''        { "FlyStand",               kRoleHover },
        { "FlyStop",                kRoleHover },'''
s = s.replace(old_hover, new_hover, 1)

# Forward must not be pre-empted by the AdvFly block any more.
old_advfwd = '''            case FLYROLE_FORWARD:
            {
                const uint32_t ids[] = { kAdvFlyGlide, kAdvFlyGlideSlow, kAdvFlyGlideFast };
                const uint32_t got = pick(ids, 3);
                if (got) return got;
                break;
            }'''
new_advfwd = '''            case FLYROLE_FORWARD:
                // Left to the name walk below so FlyRun wins when the model carries it; the AdvFly glide is
                // only reached if there is no FlyRun.
                break;'''
assert old_advfwd in s
s = s.replace(old_advfwd, new_advfwd, 1)

open(p, 'w', encoding='utf-8').write(s)
print('resolver reordered: own-name first, FlyRun forward, wider backward vocabulary')
