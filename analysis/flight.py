import re, collections

rows = {}
for line in open(r'analysis/anim.txt', encoding='utf-16', errors='ignore'):
    m = re.match(r'\s*id=(\d+)\s+f0=\[(\d+)\]\s+f1=\[(-?\d+)\]\s+f2=\[(\d+)\]', line)
    if m:
        i, f0, tier, beh = (int(x) for x in m.groups())
        rows[i] = (f0, tier, beh)

# Stock flight block: 229..485 at tier 3. behaviour == id - 229 for the contiguous part,
# but the ones that matter are the ones the client already names via ANIM_*.
STOCK = {
    0: 'Stand', 3: 'Stand', 4: 'Walk', 5: 'Run',
    11: 'WaterWalk?', 42: 'SWIM', 43: 'SwimLeft', 44: 'SwimRight', 45: 'SwimBack',
    135: 'FLY', 187: 'JumpLandRun',
}

print('=== stock flight block (tier 3), behaviour -> id ===')
for i in sorted(rows):
    f0, tier, beh = rows[i]
    if tier == 3 and i <= 485:
        print('  id=%-4d behavior=%-4d fallback=%-4d  %s' % (i, beh, f0, STOCK.get(beh, '')))

print()
print('=== what the mounts actually author at tier 3 ===')
models = collections.defaultdict(set)
for line in open(r'bin/Win/Logs/m2-sequences.csv', encoding='utf-8', errors='ignore'):
    p = line.rstrip('\n').split(',')
    if len(p) < 6: continue
    try: models[p[0]].add(int(p[1]))
    except ValueError: pass

mounts = {k: v for k, v in models.items() if 'mount' in k.lower()}
allt3 = collections.Counter()
for name, ids in mounts.items():
    for i in ids:
        if i in rows and rows[i][1] == 3:
            allt3[rows[i][2]] += 1

print('behaviour -> how many of the %d mounts author it at tier 3:' % len(mounts))
for beh, n in sorted(allt3.items()):
    print('  behavior=%-4d %3d mounts   %s' % (beh, n, STOCK.get(beh, '')))
