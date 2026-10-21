p = 'docs/M2-ANIMATIONS.md'
d = open(p, encoding='utf-8').read()

d = d.replace('behaviour = `id  229`', 'behaviour = `id - 229`')
d = d.replace('offsets currently wrong - see 1', 'offsets currently wrong - see section 1')

open(p, 'w', encoding='utf-8', newline='\n').write(d)

for i, l in enumerate(d.splitlines(), 1):
    if i in (80, 202):
        print(i, l)
