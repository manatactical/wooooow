import re, collections

rows = {}
for line in open(r'analysis/anim.txt', encoding='utf-16', errors='ignore'):
    m = re.match(r'\s*id=(\d+)\s+f0=\[(\d+)\]\s+f1=\[(-?\d+)\]\s+f2=\[(\d+)\]', line)
    if m:
        i, f0, tier, beh = (int(x) for x in m.groups())
        rows[i] = (f0, tier, beh)

# Stock ids that name a movement. A chain that lands on one of these tells us what the
# modern id IS, because the client's own fallback resolver works the same way.
STOCK_MOVE = {
    3:   'stand',   0: 'stand',   229: 'stand',   41: 'idle',
    4:   'walk',    233:'walk',   5:   'forward', 234: 'forward', 42: 'forward', 271: 'forward',
    43:  'left',    272:'left',   44:  'right',   273: 'right',   45: 'backward', 274: 'backward',
    321: 'strafeL', 322:'strafeR', 364:'flycycle', 135: 'flycycle',
}

def movement(i, depth=24):
    """Follow the fallback chain to a stock id; that stock id names the movement."""
    seen = set(); cur = i
    for _ in range(depth):
        if cur in seen or cur not in rows:
            return None
        if cur in STOCK_MOVE:
            return STOCK_MOVE[cur]
        seen.add(cur)
        f0, tier, beh = rows[cur]
        if f0 == cur:
            return None
        cur = f0
    return None

models = collections.defaultdict(set)
for line in open(r'bin/Win/Logs/m2-sequences.csv', encoding='utf-8', errors='ignore'):
    p = line.rstrip('\n').split(',')
    if len(p) < 6: continue
    try: models[p[0]].add(int(p[1]))
    except ValueError: pass

mounts = {k: v for k, v in models.items() if 'mount' in k.lower()}

STATES = ['stand','idle','walk','forward','left','right','backward','strafeL','strafeR','flycycle']
cov = collections.Counter()
print('%-40s %s' % ('mount', 'movements (best id)'))
print('-'*108)
per = {}
for name in sorted(mounts):
    best = {}
    for i in mounts[name]:
        mv = movement(i)
        if not mv: continue
        # prefer a tier-3 id, then a modern id (>505), then stock
        rank = (2 if rows.get(i,(0,0,0))[1] == 3 else 0) + (1 if i > 505 else 0)
        if mv not in best or rank > best[mv][0]:
            best[mv] = (rank, i)
    per[name] = {k: v[1] for k, v in best.items()}
    for k in best: cov[k] += 1
    print('%-40s %s' % (name.split('\\')[-1][:40],
          ' '.join('%s=%d' % (k, per[name][k]) for k in STATES if k in per[name]) or '(none)'))

print()
print('COVERAGE across %d mounts:' % len(mounts))
for k in STATES:
    if cov[k]: print('  %-10s %2d' % (k, cov[k]))
