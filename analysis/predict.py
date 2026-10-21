import collections

# Mirror of the C++ ladders, in the same order.
LADDERS = {
 'hover':   ['FlyStand','FlyStop','DragonSpitHover','AdvFlyUp','Stand'],
 'forward': ['FlyRun','FlyFly','MountFlightSprint','MountFlightWalk',
             'AdvFlyForwardGlide','AdvFlyForwardGlideSlow','AdvFlyForwardGlideFast',
             'AdvFlyForward','Run'],
 'backward':['MountFlightBackwards','FlyWalkbackwards','RunBackwards','Walkbackwards','AdvFlyRollEnd'],
 'left':    ['MountFlightLeft','FlyRunLeft','MountSelfRunLeft','AdvFlyLeft','RunLeft'],
 'right':   ['MountFlightRight','FlyRunRight','MountSelfRunRight','AdvFlyRight','RunRight'],
}

IDS = {
 229:'FlyStand',232:'FlyStop',183:'DragonSpitHover',1528:'AdvFlyUp',0:'Stand',
 234:'FlyRun',364:'FlyFly',558:'MountFlightSprint',560:'MountFlightWalk',
 1532:'AdvFlyForwardGlide',1722:'AdvFlyForwardGlideSlow',1724:'AdvFlyForwardGlideFast',
 1524:'AdvFlyForward',5:'Run',
 550:'MountFlightBackwards',242:'FlyWalkbackwards',1146:'RunBackwards',13:'Walkbackwards',1558:'AdvFlyRollEnd',
 552:'MountFlightLeft',322:'FlyRunLeft',626:'MountSelfRunLeft',1520:'AdvFlyLeft',93:'RunLeft',
 554:'MountFlightRight',321:'FlyRunRight',628:'MountSelfRunRight',1522:'AdvFlyRight',92:'RunRight',
}

seqs = collections.defaultdict(set)
for line in open('bin/Win/Logs/m2-sequences.csv', encoding='utf-8', errors='ignore'):
    p = line.rstrip().split(',')
    if len(p) < 6: continue
    try: seqs[p[0]].add(int(p[1]))
    except ValueError: pass

def resolve(ids, role):
    ladder = LADDERS[role]
    best, bestrank = 0, 10**9
    for i in ids:
        nm = IDS.get(i)
        if nm and 'Swim' in nm: continue
        if nm and nm in ladder:
            r = ladder.index(nm)
            if r < bestrank:
                bestrank, best = r, i
    return best, (ladder[bestrank] if best else '-')

print('%-34s %s' % ('mount', 'resolved roles'))
print('-'*104)
for name in sorted(seqs):
    if 'mount' not in name.lower(): continue
    out = []
    for role in ('hover','forward','backward','left','right'):
        i, nm = resolve(seqs[name], role)
        out.append('%s=%s' % (role, i if i else 'NONE'))
    print('%-34s %s' % (name.split('\\')[-1][:34], '  '.join(out)))
