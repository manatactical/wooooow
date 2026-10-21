import csv, collections, re

# AnimationData: id -> (fallback, tier, behavior)
rows = {}
with open(r'bin/Win/Logs/animationdata-client.db2', 'rb'):
    pass

# Reuse the decoded table if present, else decode with the tool output.
import subprocess, io, sys
out = subprocess.run([r'build\x64\bin\Debug\ModernAssetsTest.exe', 'animdb2',
                      r'bin/Win/Logs/animationdata-client.db2', '1', '1', '1', '2'],
                     capture_output=True)
text = out.stdout.decode('utf-16', errors='ignore')
for line in text.splitlines():
    m = re.match(r'\s*id=(\d+)\s+f0=\[(\d+)\]\s+f1=\[(-?\d+)\]\s+f2=\[(\d+)\]', line)
    if m:
        i, f0, tier, beh = (int(x) for x in m.groups())
        rows[i] = (f0, tier, beh)

print('AnimationData rows:', len(rows))

# Load the harvest
models = collections.defaultdict(list)
with open(r'bin/Win/Logs/m2-sequences.csv', encoding='utf-8', errors='ignore') as f:
    for line in f:
        p = line.rstrip('\n').split(',')
        if len(p) < 6: continue
        name = p[0]
        try:
            sid = int(p[1]); dur = int(p[2]); flags = int(p[4], 16); spd = float(p[5])
        except ValueError:
            continue
        models[name].append((sid, dur, flags, spd))

print('models:', len(models))

# Split mounts vs other creatures by path
mounts = {k: v for k, v in models.items() if 'mount' in k.lower()}
others = {k: v for k, v in models.items() if 'mount' not in k.lower()}
print('mount-like models:', len(mounts), ' other:', len(others))

with open(r'analysis/mounts.txt', 'w', encoding='utf-8') as f:
    for k in sorted(mounts):
        f.write(k + '\n')
print('wrote analysis/mounts.txt')
