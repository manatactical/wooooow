import re
p = 'WoW/Source/Object/ObjectClient/Unit_C.cpp'
s = open(p, encoding='utf-8').read()

start = s.index('    const FlightNameRole s_flightNameRoles[] = {')
end = s.index('    };', start) + len('    };')

new = '''    // Role ladders, best first. A movement is resolved by walking this list and taking the first name the
    // model actually carries (checked against its own sequence table), so a mount that lacks the ideal clip
    // still gets the most movement-appropriate one it has.
    //
    // SWIM ANIMATIONS ARE DELIBERATELY ABSENT. Every Swim* / MountSwim* name is excluded on purpose: a
    // downported mount often has only the swim set available, and playing those in the air makes a flying
    // mount paddle. A role with no non-swim clip stays unresolved and the caller keeps the previous clip.
    const FlightNameRole s_flightNameRoles[] = {
        // hover: a held flight pose
        { "FlyStand",               kRoleHover },
        { "FlyStop",                kRoleHover },
        { "DragonSpitHover",        kRoleHover },
        { "AdvFlyUp",               kRoleHover },
        { "Stand",                  kRoleHover },

        // forward: continuous wingbeats first, held glides only as a fallback
        { "FlyRun",                 kRoleForward },
        { "FlyFly",                 kRoleForward },
        { "MountFlightRun",         kRoleForward },
        { "MountFlightWalk",        kRoleForward },
        { "AdvFlyForwardGlide",     kRoleForward },
        { "AdvFlyForwardGlideSlow", kRoleForward },
        { "AdvFlyForwardGlideFast", kRoleForward },
        { "AdvFlyForward",          kRoleForward },
        { "Run",                    kRoleForward },

        // backward
        { "MountFlightBackwards",   kRoleBackward },
        { "FlyWalkbackwards",       kRoleBackward },
        { "RunBackwards",           kRoleBackward },
        { "Walkbackwards",          kRoleBackward },
        { "AdvFlyRollEnd",          kRoleBackward },

        // lateral: flight gaits, not retail drift
        { "MountFlightLeft",        kRoleLeft },
        { "FlyRunLeft",             kRoleLeft },
        { "MountSelfRunLeft",       kRoleLeft },
        { "AdvFlyLeft",             kRoleLeft },
        { "RunLeft",                kRoleLeft },

        { "MountFlightRight",       kRoleRight },
        { "FlyRunRight",            kRoleRight },
        { "MountSelfRunRight",      kRoleRight },
        { "AdvFlyRight",            kRoleRight },
        { "RunRight",               kRoleRight },
    };'''

s = s[:start] + new + s[end:]

# Extend the id -> name lookup to cover every id in the ladders.
old_names_start = s.index('    const char* FlightIdName(uint32_t id)')
old_names_end = s.index('    /// Role this name satisfies, or -1.', old_names_start)

new_names = '''    const char* FlightIdName(uint32_t id)
    {
        switch (id)
        {
            // hover
            case 229: return "FlyStand";
            case 232: return "FlyStop";
            case 183: return "DragonSpitHover";
            case 1528: return "AdvFlyUp";
            case 0:   return "Stand";

            // forward
            case 234: return "FlyRun";
            case 364: return "FlyFly";
            case 558: return "MountFlightSprint";
            case 560: return "MountFlightWalk";
            case 1532: return "AdvFlyForwardGlide";
            case 1722: return "AdvFlyForwardGlideSlow";
            case 1724: return "AdvFlyForwardGlideFast";
            case 1524: return "AdvFlyForward";
            case 5:   return "Run";

            // backward
            case 550: return "MountFlightBackwards";
            case 242: return "FlyWalkbackwards";
            case 1146: return "RunBackwards";
            case 13:  return "Walkbackwards";
            case 1558: return "AdvFlyRollEnd";

            // lateral
            case 552: return "MountFlightLeft";
            case 322: return "FlyRunLeft";
            case 626: return "MountSelfRunLeft";
            case 1520: return "AdvFlyLeft";
            case 93:  return "RunLeft";

            case 554: return "MountFlightRight";
            case 321: return "FlyRunRight";
            case 628: return "MountSelfRunRight";
            case 1522: return "AdvFlyRight";
            case 92:  return "RunRight";

            default: return nullptr;
        }
    }

'''

s = s[:old_names_start] + new_names + s[old_names_end:]

open(p, 'w', encoding='utf-8').write(s)
print('ladders rewritten, swim excluded')
