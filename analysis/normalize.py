import io

p = 'docs/M2-ANIMATIONS.md'
d = open(p, encoding='utf-8', errors='replace').read()

# Report non-ascii found, then normalise to plain ASCII so the file reads correctly in any tool.
replacements = {
    '\u2014': '-',   # em dash
    '\u2013': '-',   # en dash
    '\u2192': '->',  # right arrow
    '\u2018': "'", '\u2019': "'",
    '\u201c': '"', '\u201d': '"',
    '\u2026': '...',
    '\ufffd': '',    # replacement char from earlier mangling
}

counts = {}
for ch in sorted(set(d)):
    if ord(ch) > 127:
        counts[ch] = d.count(ch)

for k, v in replacements.items():
    d = d.replace(k, v)

# Anything still non-ascii: drop it rather than guess.
leftover = {ch: d.count(ch) for ch in set(d) if ord(ch) > 127}
for ch in leftover:
    d = d.replace(ch, '')

open(p, 'w', encoding='utf-8', newline='\n').write(d)

print('normalised. replaced:', {hex(ord(k)): v for k, v in counts.items()})
print('leftover dropped:', {hex(ord(k)): v for k, v in leftover.items()})
print('ascii only now:', all(ord(c) < 128 for c in d))
