import re, collections

# --- official animation names ------------------------------------------------
NAMES = {}
for line in open(r'analysis/animlist.txt', encoding='utf-8').read().splitlines()[1:]:
    p = [x.strip() for x in line.split('|')]
    if len(p) >= 2 and p[0].isdigit():
        NAMES[int(p[0])] = p[1]

# --- AnimationData: fallback / tier / behaviour -----------------------------
rows = {}
for line in open(r'analysis/anim.txt', encoding='utf-16', errors='ignore'):
    m = re.match(r'\s*id=(\d+)\s+f0=\[(\d+)\]\s+f1=\[(-?\d+)\]\s+f2=\[(\d+)\]', line)
    if m:
        i, f0, tier, beh = (int(x) for x in m.groups())
        rows[i] = (f0, tier, beh)

# Movement roles, keyed by the OFFICIAL name each id resolves to. A mount's clip for a role is the
# best-ranked id whose fallback chain ends at a stock id with that name.
ROLE = {
    'FlyStand': 'hover',
    'FlyStop': 'hover',
    'FlyWalk': 'walk',
    'FlyRun': 'forward',
    'FlyFly': 'flycycle',
    'FlyRunLeft': 'strafeL',
    'FlyRunRight': 'strafeR',
    'MountFlightBackwards': 'backward',
    'RunBackwards': 'backward',
    'AdvFlyForwardGlide': 'glide',
    'AdvFlyForwardGlideSlow': 'glideSlow',
    'AdvFlyForward': 'advForward',
    'AdvFlyLeft': 'advLeft',
    'AdvFlyRight': 'advRight',
    'AdvFlyFlap': 'flap',
    'AdvFlyFlapBig': 'flapBig',
    'AdvFlyFlapFoward': 'flapFwd',
    'AdvFlySecondFlapUp': 'flap2',
    'AdvFlyFlapUp': 'flapUp',
    'AdvFlySlowFall': 'slowfall',
    'AdvFlyDown': 'advDown',
    'FlyShuffleLeft': 'turnL',
    'FlyShuffleRight': 'turnR',
    'FlySwim': 'flyswim',
    'FlySwimLeft': 'flyswimL',
    'FlySwimRight': 'flyswimR',
    'FlySwimBackwards': 'flyswimB',
    'Swim': 'swim',
    'SwimIdle': 'swimidle',
    'SwimLeft': 'swimL',
    'SwimRight': 'swimR',
    'SwimBackwards': 'swimB',
}

def resolve(i, depth=24):
    seen = set(); cur = i
    for _ in range(depth):
        if cur in seen or cur not in rows: return None
        n = NAMES.get(cur)
        if n in ROLE: return ROLE[n]
        seen.add(cur)
        f0, _, _ = rows[cur]
        if f0 == cur: return None
        cur = f0
    return None

models = collections.defaultdict(set)
for line in open(r'bin/Win/Logs/m2-sequences.csv', encoding='utf-8', errors='ignore'):
    p = line.rstrip('\n').split(',')
    if len(p) < 6: continue
    try: models[p[0]].add(int(p[1]))
    except ValueError: pass

mounts = {k: v for k, v in models.items() if 'mount' in k.lower()}

ORDER = ['hover','walk','forward','strafeL','strafeR','backward','glide','advForward','advLeft','advRight',
         'flycycle','flap','flyswim']

cov = collections.Counter()
per = {}
for name in sorted(mounts):
    best = {}
    for i in mounts[name]:
        role = resolve(i)
        if not role: continue
        # rank: tier-3 first, then modern (>505), then stock -- prefer the richest clip
        rank = (2 if rows.get(i, (0,0,0))[1] == 3 else 0) + (1 if i > 505 else 0)
        if role not in best or rank > best[role][0]:
            best[role] = (rank, i)
    per[name] = {k: v[1] for k, v in best.items()}
    for k in best: cov[k] += 1

print('%-40s %s' % ('mount', 'roles (id)'))
print('-'*110)
for name in sorted(per):
    print('%-40s %s' % (name.split('\\')[-1][:40],
        ' '.join('%s=%d' % (k, per[name][k]) for k in ORDER if k in per[name]) or '(none)'))

print()
print('COVERAGE across %d mounts:' % len(mounts))
for k in ORDER:
    if cov[k]: print('  %-12s %2d' % (k, cov[k]))
