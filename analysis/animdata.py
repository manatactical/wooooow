import struct, os, glob, json, collections

# --- AnimationData: id -> (fallback, tier, behavior) -------------------------
def load_animdata(path):
    d = open(path, 'rb').read()
    assert d[:4] == b'WDC5'
    (recCount, fieldCount, recSize) = struct.unpack_from('<III', d, 12)
    schema = d[8:136].split(b'\0')[0].decode()
    o = 4 + 4 + 128
    (reccount, fcount, rsize, strsize, tableHash, layoutHash,
     minId, maxId, locale, flags, idIndex) = struct.unpack_from('<9I H H I', d, o)
    o2 = o + 9*4 + 2 + 2 + 4
    (totalFieldCount, bitpackedOffset, lookupCols, fsiSize,
     commonSize, palletSize, sectionCount) = struct.unpack_from('<7I', d, o2)

    o3 = o2 + 7*4
    sections = [struct.unpack_from('<Q8I', d, o3 + i*40) for i in range(sectionCount)]
    o3 += sectionCount*40
    fmeta = [struct.unpack_from('<hH', d, o3 + i*4) for i in range(fcount)]
    o3 += fcount*4
    cols = [struct.unpack_from('<HHIIIII', d, o3 + i*24) for i in range(fcount)]
    o3 += fcount*24

    # field storage info
    storage = []
    for i in range(fcount):
        storage.append(struct.unpack_from('<IIIII', d, o3 + i*20))
    o3 += fsiSize

    common = d[o3:o3+commonSize]; o3 += commonSize
    pallet = d[o3:o3+palletSize]; o3 += palletSize

    def pal(idx):
        return struct.unpack_from('<I', pallet, idx*4)[0]

    rows = {}
    for s in sections:
        fileOffset = s[1]; recordCount = s[2]
        # records
        recStart = fileOffset
        secStart = recStart + recordCount*rsize
        stringSize = s[3]
        idListSize = s[5]
        idList = secStart + stringSize
        ids = [struct.unpack_from('<I', d, idList + i*4)[0] for i in range(idListSize//4)]
        # find section file offset for the record data we need
        for ri in range(recordCount):
            base = recStart + ri*rsize
            bits = d[base:base+rsize]
            bitpos = 0
            def readbits(n):
                nonlocal bitpos
                v = 0
                for k in range(n):
                    byte = bits[bitpos >> 3]
                    v |= ((byte >> (bitpos & 7)) & 1) << k
                    bitpos += 1
                return v
            vals = []
            for fi,(ro,sz,addSize,comp,v1,v2,v3) in enumerate(cols):
                if comp == 4:  # pallet array
                    n = v3
                    idx = readbits(v2)
                    arr = [pal(idx*n + e) for e in range(n)]
                    vals.append(arr)
                else:
                    if comp == 1:  # immediate
                        vals.append([readbits(v2)])
                    elif comp == 5:  # signed immediate
                        w = v2
                        raw = readbits(w)
                        if w<32 and raw & (1<<(w-1)): raw -= (1<<w)
                        vals.append([raw & 0xFFFFFFFF])
                    else:
                        # none / common
                        vals.append([v1])
            rid = ids[ri] if (flags & 4)==0 and idListSize else (vals[idIndex][0] if vals else 0)
            if not idListSize:
                rid = vals[idIndex][0] if len(vals)>idIndex else 0
            f0 = vals[0][0] if len(vals[0]) else 0
            tier = vals[1][0] if len(vals[1]) else 0
            beh = vals[2][0] if len(vals[2]) else 0
            rows[rid] = (f0, tier, beh)
    return rows

rows = load_animdata(r'bin/Win/Logs/animationdata-client.db2')
print('AnimationData rows:', len(rows))
for probe in (0,3,5,42,135,229,234,1532,1706,1638):
    print('  id=%-5d %s' % (probe, rows.get(probe)))
