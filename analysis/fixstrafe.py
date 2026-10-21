p = 'WoW/Source/Object/ObjectClient/Unit_C.cpp'
s = open(p, encoding='utf-8').read()

# 1) Add the literal mount-flight lateral names + keep FlyRun* preferred over AdvFly*.
old_tbl = '''        { "AdvFlyLeft",             kRoleLeft },
        { "FlyRunLeft",             kRoleLeft },
        { "AdvFlyRight",            kRoleRight },
        { "FlyRunRight",            kRoleRight },'''

new_tbl = '''        // Lateral flight, best first. These are flight-locomotion gaits, which is what a WotLK client's
        // strafe expects; AdvFlyLeft/Right are the retail advanced-fly drift and read wrong here, so they
        // are only ever a last resort (kept in the table so a model that has nothing else still gets
        // something, but the resolver tries the FlyRun/MountFlight names first).
        { "MountFlightLeft",        kRoleLeft },
        { "FlyRunLeft",             kRoleLeft },
        { "MountSelfRunLeft",       kRoleLeft },
        { "MountFlightRight",       kRoleRight },
        { "FlyRunRight",            kRoleRight },
        { "MountSelfRunRight",      kRoleRight },
        { "AdvFlyLeft",             kRoleLeft },
        { "AdvFlyRight",            kRoleRight },'''

assert old_tbl in s
s = s.replace(old_tbl, new_tbl, 1)

# 2) Named id table: add the literal lateral ids.
old_names = '''            case 321: return "FlyRunRight";
            case 322: return "FlyRunLeft";'''
new_names = '''            case 321: return "FlyRunRight";
            case 322: return "FlyRunLeft";
            case 552: return "MountFlightLeft";
            case 554: return "MountFlightRight";
            case 626: return "MountSelfRunLeft";
            case 628: return "MountSelfRunRight";'''
assert old_names in s
s = s.replace(old_names, new_names, 1)

# 3) The AdvFly shortcut must not pre-empt the chain walk for the lateral roles.
old_adv = '''            case FLYROLE_LEFT:
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
            }'''

new_adv = '''            case FLYROLE_LEFT:
            case FLYROLE_RIGHT:
                // Deliberately NOT taken from the AdvFly block. 1520/1522 are the retail advanced-fly drift,
                // which does not match how this client's strafe moves the unit; the chain walk below finds
                // FlyRunLeft/Right (or MountFlightLeft/Right) first, which do.
                break;'''

assert old_adv in s
s = s.replace(old_adv, new_adv, 1)

open(p, 'w', encoding='utf-8').write(s)
print('lateral priority fixed')
