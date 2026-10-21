import re, collections, csv

# --- AnimationData (decoded output of ModernAssetsTest animdb2) ---------------
rows = {}
with open(r'analysis/anim.txt', encoding='utf-16', errors='ignore') as f:
    for line in f:
        m = re.match(r'\s*id=(\d+)\s+f0=\[(\d+)\]\s+f1=\[(-?\d+)\]\s+f2=\[(\d+)\]', line)
        if m:
            i, f0, tier, beh = (int(x) for x in m.groups())
            rows[i] = (f0, tier, beh)

# --- sequence dump -----------------------------------------------------------
models = collections.defaultdict(set)
with open(r'bin/Win/Logs/m2-sequences.csv', encoding='utf-8', errors='ignore') as f:
    for line in f:
        p = line.rstrip('\n').split(',')
        if len(p) < 6: continue
        try: models[p[0]].add(int(p[1]))
        except ValueError: pass

# Semantics established from the table itself.
MEANING = {
    0: 'Stand', 3: 'Stand', 4: 'Walk', 5: 'Run',
    42: 'SWIM', 135: 'FLY', 229: 'FlyStand', 234: 'FlyRun',
    232: 'FlyStand?', 233: 'FlyWalk',
}

def endpoint(i, depth=16):
    seen = set(); cur = i
    for _ in range(depth):
        if cur in seen or cur not in rows: return cur
        seen.add(cur)
        f0, tier, beh = rows[cur]
        if cur in MEANING: return cur
        if f0 == cur: return cur
        cur = f0
    return cur

def tier_of(i):
    return rows.get(i, (0, 0, 0))[1]

# Classify every authored id of a model by the stock behaviour it ultimately expresses.
def classify(ids):
    got = collections.defaultdict(list)
    for i in ids:
        ep = endpoint(i)
        if ep in MEANING:
            got[MEANING[ep]].append(i)
    return got

# Mounts: creature models that are mounts (path has 'mount' or known flying stock)
mounts = {k: v for k, v in models.items() if 'mount' in k.lower() or k.lower().startswith('creature\\')}
mounts = {k: v for k, v in mounts.items() if 'mount' in k.lower()}

# Coverage across all mounts
coverage = collections.Counter()
detail = []

for name in sorted(mounts):
    ids = mounts[name]
    got = classify(ids)
    states = {
        'hover': 'FlyStand' in got,
        'fly':   'FLY' in got,
        'flyrun':'FlyRun' in got,
        'flywalk':'FlyWalk' in got,
        'swim':  'SWIM' in got,
    }
    for k, v in states.items():
        if v: coverage[k] += 1
    detail.append((name, len(ids), states, {k: sorted(v)[:4] for k, v in got.items() if k in
                  ('FLY','FlyStand','FlyRun','FlyWalk','SWIM','Run','Walk')}))

print('mount models analysed:', len(detail))
print()
print('STATE COVERAGE across those mounts:')
for k in ('hover','fly','flyrun','flywalk','swim'):
    print('  %-8s %3d / %d' % (k, coverage[k], len(detail)))

print()
print('per-mount detail (first 25):')
for name, n, st, got in detail[:25]:
    print('  %-58s n=%-4d %s' % (name.split('\\')[-1], n,
          ' '.join('%s=%d' % (k, v) for k, v in sorted(st.items()) if v)))
    if got:
        print('        ids: %s' % {k: v for k, v in got.items()})
