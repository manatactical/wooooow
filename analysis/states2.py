import re, collections

rows = {}
for line in open(r'analysis/anim.txt', encoding='utf-16', errors='ignore'):
    m = re.match(r'\s*id=(\d+)\s+f0=\[(\d+)\]\s+f1=\[(-?\d+)\]\s+f2=\[(\d+)\]', line)
    if m:
        i, f0, tier, beh = (int(x) for x in m.groups())
        rows[i] = (f0, tier, beh)

# Complete movement map: behaviour -> name. At tier 3 each of these is its flight form.
MOVE = {
    0: 'hover', 4: 'walk', 5: 'forward', 42: 'forward42', 43: 'left', 44: 'right', 45: 'backward',
    92: 'strafe-L', 93: 'strafe-R', 135: 'flycycle',
}

models = collections.defaultdict(set)
for line in open(r'bin/Win/Logs/m2-sequences.csv', encoding='utf-8', errors='ignore'):
    p = line.rstrip('\n').split(',')
    if len(p) < 6: continue
    try: models[p[0]].add(int(p[1]))
    except ValueError: pass

mounts = {k: v for k, v in models.items() if 'mount' in k.lower()}

cov = collections.Counter()
per = {}
for name in sorted(mounts):
    found = {}
    # a movement is available at flight tier if the mount has EITHER the tier-3 row itself,
    # OR a modern (untiered) id whose chain reaches it
    def chain_hits(i, depth=16):
        seen=set(); cur=i
        for _ in range(depth):
            if cur in seen or cur not in rows: return None
            seen.add(cur)
            f0,t,b = rows[cur]
            if t == 3 and b in MOVE: return (b, cur)
            if f0 == cur: return None
            cur = f0
        return None
    for i in mounts[name]:
        hit = chain_hits(i)
        if hit and hit[0] not in found:
            found[hit[0]] = i
    per[name] = found
    for b in found: cov[b] += 1

print('%-42s %s' % ('mount', 'flight movements available (via id)'))
print('-' * 104)
for name in sorted(per):
    d = '  '.join('%s=%d' % (MOVE[b], per[name][b]) for b in sorted(per[name]))
    print('%-42s %s' % (name.split('\\')[-1][:42], d or '(none)'))

print()
print('COVERAGE across %d mounts:' % len(mounts))
for b in sorted(MOVE):
    if cov[b]: print('  %-10s %2d mounts' % (MOVE[b], cov[b]))
