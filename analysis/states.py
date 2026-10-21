import re, collections

rows = {}
for line in open(r'analysis/anim.txt', encoding='utf-16', errors='ignore'):
    m = re.match(r'\s*id=(\d+)\s+f0=\[(\d+)\]\s+f1=\[(-?\d+)\]\s+f2=\[(\d+)\]', line)
    if m:
        i, f0, tier, beh = (int(x) for x in m.groups())
        rows[i] = (f0, tier, beh)

# The movement behaviours. At tier 0 these are the ground/swim set; at tier 3 the same behaviour id is
# the flight equivalent -- behaviour is the movement, tier is the context.
MOVE = {
    0:  'stand/hover',
    4:  'walk',
    5:  'run',
    42: 'forward',      # tier0 = swim fwd, tier3 = fly fwd
    43: 'left',
    44: 'right',
    45: 'backward',
    92: 'strafe-left',
    93: 'strafe-right',
    135:'fly-cycle',
}

models = collections.defaultdict(set)
for line in open(r'bin/Win/Logs/m2-sequences.csv', encoding='utf-8', errors='ignore'):
    p = line.rstrip('\n').split(',')
    if len(p) < 6: continue
    try: models[p[0]].add(int(p[1]))
    except ValueError: pass

mounts = {k: v for k, v in models.items() if 'mount' in k.lower()}

# For each mount, what tier-3 movement behaviours does it author, and which id carries each?
cov = collections.Counter()
print('%-42s %s' % ('mount', 'tier3 movement behaviours (id)'))
print('-' * 100)
for name in sorted(mounts):
    found = {}
    for i in mounts[name]:
        if i in rows and rows[i][1] == 3 and rows[i][2] in MOVE:
            found.setdefault(rows[i][2], i)
    for b in found:
        cov[b] += 1
    short = name.split('\\')[-1]
    desc = '  '.join('%s=%d' % (MOVE[b], found[b]) for b in sorted(found))
    print('%-42s %s' % (short[:42], desc or '(none)'))

print()
print('COVERAGE across %d mounts:' % len(mounts))
for b in sorted(MOVE):
    print('  behaviour %-3d %-12s  %2d mounts' % (b, MOVE[b], cov[b]))
