import re, collections

path = r'bin/Win/Logs/modern-assets.log'

seqs = {}          # file -> list of ids
mounts = set()
ordered = []

with open(path, encoding='utf-8', errors='ignore') as f:
    for line in f:
        m = re.search(r"'([^']+\.m2)' sequences (\d+): ([0-9 ]+)", line)
        if m:
            fn = m.group(1); ids = [int(x) for x in m.group(3).split()]
            seqs[fn] = ids
            continue
        m = re.search(r"seqall: '([^']+)' ids: ([0-9 ]+)", line)
        if m:
            seqs[m.group(1)] = [int(x) for x in m.group(2).split()]
            continue
        m = re.search(r"requested '([^']+\.m2)'", line)
        if m:
            mounts.add(m.group(1))

print('models with a sequence dump:', len(seqs))
for k in sorted(seqs):
    print('  %-70s %d ids' % (k, len(set(seqs[k]))))
print()
print('models requested at least once:', len(mounts))
