import io
p = 'bin/Win/Data/custom.MPQ/Interface/GlueXML/VideoOptionsPanels.xml'
lines = io.open(p, encoding='utf-8', newline='').read().split('\r\n')
for i in range(979, 1000):
    print(i + 1, lines[i])
print('physics decls', sum(1 for s in lines if 'VideoOptionsPhysicsPanel"' in s))
print('scrollframes', sum(1 for s in lines if '<ScrollFrame' in s))