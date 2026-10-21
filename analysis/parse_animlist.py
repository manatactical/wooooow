import re, html

src = open(r'analysis/animlist.html', encoding='utf-8', errors='ignore').read()

# Strip scripts/styles
src = re.sub(r'<(script|style)[^>]*>.*?</\1>', ' ', src, flags=re.S | re.I)

# Pull table rows
rows = re.findall(r'<tr[^>]*>(.*?)</tr>', src, flags=re.S | re.I)
print('table rows found:', len(rows))

out = []
for r in rows:
    cells = re.findall(r'<t[dh][^>]*>(.*?)</t[dh]>', r, flags=re.S | re.I)
    vals = []
    for c in cells:
        t = re.sub(r'<[^>]+>', '', c)
        t = html.unescape(t)
        t = ' '.join(t.split())
        vals.append(t)
    if vals:
        out.append(vals)

with open(r'analysis/animlist.txt', 'w', encoding='utf-8') as f:
    for v in out:
        f.write(' | '.join(v) + '\n')

print('wrote analysis/animlist.txt rows:', len(out))
for v in out[:15]:
    print('  ', ' | '.join(v)[:130])
