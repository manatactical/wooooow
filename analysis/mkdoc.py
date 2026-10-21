import io

# Build a compact appendix: every named animation id, in id order, grouped per 4 per line.
nam = {}
for L in open(r'analysis/animlist.txt', encoding='utf-8').read().splitlines()[1:]:
    p = [x.strip() for x in L.split('|')]
    if len(p) >= 2 and p[0].isdigit():
        nam[int(p[0])] = p[1]

out = io.StringIO()
out.write('\n---\n\n## Appendix A — complete animation id list\n\n')
out.write('Source: <https://wowdev.wiki/M2/AnimationList>. %d named ids, range 0..%d.\n\n'
          % (len(nam), max(nam)))
out.write('Regenerate from `analysis/animlist.txt` if the wiki is updated.\n\n')
out.write('| id | name | id | name | id | name |\n')
out.write('|---:|---|---:|---|---:|---|\n')

ids = sorted(nam)
rows = (len(ids) + 2) // 3
for r in range(rows):
    cells = []
    for c in range(3):
        k = r + c * rows
        if k < len(ids):
            i = ids[k]
            cells.append('%d' % i)
            cells.append(nam[i].replace('|', '\\|'))
        else:
            cells += ['', '']
    out.write('| ' + ' | '.join(cells) + ' |\n')

txt = out.getvalue()
with open('docs/M2-ANIMATIONS.md', 'a', encoding='utf-8') as f:
    f.write(txt)

print('appended %d ids, %d table rows' % (len(ids), rows))
