import re, collections

rows = {}
with open(r'analysis/anim.txt', encoding='utf-16', errors='ignore') as f:
    for line in f:
        m = re.match(r'\s*id=(\d+)\s+f0=\[(\d+)\]\s+f1=\[(-?\d+)\]\s+f2=\[(\d+)\]', line)
        if m:
            i, f0, tier, beh = (int(x) for x in m.groups())
            rows[i] = (f0, tier, beh)

print('AnimationData rows:', len(rows))
for probe in (0, 3, 5, 42, 135, 229, 232, 233, 234, 1532, 1702, 1706, 1638):
    print('  id=%-5d fallback=%-5s tier=%-3s behavior=%s' % ((probe,) + rows.get(probe, ('-','-','-'))))

# Which behaviours exist, and at what tiers?
by_beh = collections.defaultdict(set)
for i, (f0, tier, beh) in rows.items():
    by_beh[beh].add(tier)

print()
print('behaviours with a flight tier (tier>=3):')
for beh, tiers in sorted(by_beh.items()):
    hi = {t for t in tiers if t >= 3}
    if hi:
        print('  behavior=%-4d tiers=%s' % (beh, sorted(hi)))
