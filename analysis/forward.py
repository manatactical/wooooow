import re, collections

nam = {}
for L in open('analysis/animlist.txt', encoding='utf-8').read().splitlines()[1:]:
    p = [x.strip() for x in L.split('|')]
    if len(p) >= 2 and p[0].isdigit():
        nam[int(p[0])] = p[1]

# id -> (fallback, tier, behaviour)
rows = {}
for line in open('analysis/anim.txt', encoding='utf-16', errors='ignore'):
    m = re.match(r'\s*id=(\d+)\s+f0=\[(\d+)\]\s+f1=\[(-?\d+)\]\s+f2=\[(\d+)\]', line)
    if m:
        i, f0, t, b = (int(x) for x in m.groups())
        rows[i] = (f0, t, b)

# model -> {id: (duration, flags)}
seqs = collections.defaultdict(dict)
for line in open('bin/Win/Logs/m2-sequences.csv', encoding='utf-8', errors='ignore'):
    p = line.rstrip().split(',')
    if len(p) < 6:
        continue
    try:
        seqs[p[0]][int(p[1])] = (int(p[2]), int(p[4], 16))
    except ValueError:
        pass

mounts = {k: v for k, v in seqs.items() if 'mount' in k.lower()}

SKIP = ('Emote', 'Attack', 'Spell', 'Channel', 'Ready', 'Death', 'Wound', 'Shuffle', 'Turn')
WANT = ('FlyRun', 'FlyWalk', 'AdvFlyForward', 'Glide', 'AdvFlyFlap', 'FlyFly', 'AdvFlyRoll', 'AdvFlyDown')

use = collections.Counter()
for n, ids in mounts.items():
    for i in ids:
        nm = nam.get(i, '')
        if not nm or any(s in nm for s in SKIP):
            continue
        if any(w in nm for w in WANT):
            use[(i, nm)] += 1

print('%-6s %-30s %-7s %-9s %-8s %s' % ('id', 'official name', 'mounts', 'duration', 'tier', 'fallback'))
print('-' * 92)
for (i, nm), n in sorted(use.items(), key=lambda x: -x[1]):
    dur = ''
    for mname, dd in seqs.items():
        if i in dd:
            dur = str(dd[i][0])
            break
    tier = rows.get(i, (0, 0, 0))[1]
    fb = rows.get(i, (0, 0, 0))[0]
    print('%-6d %-30s %-7d %-9s %-8s %d' % (i, nm, n, dur, ('t%d' % tier) if tier else 'base', fb))
