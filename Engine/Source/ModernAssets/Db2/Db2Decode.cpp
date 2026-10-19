#include "Engine/Source/ModernAssets/Db2/Db2Decode.h"

#include <cstring>
#include <unordered_map>

// Ported from WarcraftXL wxl-db2 decode/Db2Decode.cpp (GPL-3.0-or-later, Copyright (C) 2026
// WarcraftXL). Behaviour is identical; the difference is that every on-disk offset, count and pallet
// index is checked against the buffer before use, so a truncated or hostile file returns false
// instead of reading out of bounds.

namespace modern::db2
{
namespace
{
    constexpr uint32_t kMagicWDC1 = 0x31434457; // 'WDC1'
    constexpr uint32_t kMagicWDC2 = 0x32434457; // 'WDC2'
    constexpr uint32_t kMagic1SLC = 0x434C5331; // '1SLC'
    constexpr uint32_t kMagicWDC3 = 0x33434457; // 'WDC3'

#pragma pack(push, 1)
    struct FieldStructure
    {
        int16_t  size;     // byteSize = (32 - size) / 8
        uint16_t position;
    };

    struct FieldStorageInfo
    {
        uint16_t fieldOffsetBits;
        uint16_t fieldSizeBits;
        uint32_t additionalDataSize;
        uint32_t storageType;
        uint32_t p1, p2, p3;
    };

    struct Wdc1Header
    {
        uint32_t magic, recordCount, fieldCount, recordSize, stringTableSize;
        uint32_t tableHash, layoutHash, minId, maxId, locale, copyTableSize;
        uint16_t flags, idIndex;
        uint32_t totalFieldCount, bitpackedDataOffset, lookupColumnCount, offsetMapOffset;
        uint32_t idListSize, fieldStorageInfoSize, commonDataSize, palletDataSize, relationshipDataSize;
    };

    struct Wdc23Header
    {
        uint32_t magic, recordCount, fieldCount, recordSize, stringTableSize;
        uint32_t tableHash, layoutHash, minId, maxId, locale;
        uint16_t flags, idIndex;
        uint32_t totalFieldCount, bitpackedDataOffset, lookupColumnCount;
        uint32_t fieldStorageInfoSize, commonDataSize, palletDataSize, sectionCount;
    };

    struct Wdc2Section
    {
        uint64_t tactKeyHash;
        uint32_t fileOffset, recordCount, stringTableSize, copyTableSize;
        uint32_t offsetMapOffset, idListSize, relationshipDataSize;
    };

    struct Wdc3Section
    {
        uint64_t tactKeyHash;
        uint32_t fileOffset, recordCount, stringTableSize, offsetRecordsEnd;
        uint32_t idListSize, relationshipDataSize, offsetMapIdCount, copyTableCount;
    };
#pragma pack(pop)

    static_assert(sizeof(FieldStructure) == 4);
    static_assert(sizeof(FieldStorageInfo) == 24);

    /// True when [off, off+len) lies inside a buffer of size bytes. 64-bit so a hostile count cannot wrap.
    inline bool InRange(uint32_t size, uint64_t off, uint64_t len)
    {
        return off <= size && len <= static_cast<uint64_t>(size) - off;
    }

    template <class T>
    inline const T* At(const uint8_t* d, uint32_t size, uint64_t off, uint64_t count = 1)
    {
        if (!InRange(size, off, count * sizeof(T))) return nullptr;
        return reinterpret_cast<const T*>(d + off);
    }

    // Little-endian bitfield read of up to ~57 useful bits.
    uint64_t ReadBitsU(const uint8_t* p, uint32_t bitOffset, uint32_t bitCount)
    {
        if (bitCount == 0) return 0;
        uint32_t byteIdx = bitOffset >> 3;
        uint32_t shift   = bitOffset & 7;
        uint32_t need    = (bitCount + shift + 7) >> 3;
        uint64_t v = 0;
        for (uint32_t i = 0; i < need && i < 8; ++i)
            v |= static_cast<uint64_t>(p[byteIdx + i]) << (8 * i);
        v >>= shift;
        if (bitCount < 64) v &= (static_cast<uint64_t>(1) << bitCount) - 1;
        return v;
    }

    int32_t SignExtend(uint64_t v, uint32_t bits)
    {
        if (bits == 0 || bits >= 32) return static_cast<int32_t>(v);
        uint64_t m = static_cast<uint64_t>(1) << (bits - 1);
        return static_cast<int32_t>((v ^ m) - m);
    }

    struct Section
    {
        const uint8_t* records = nullptr;
        uint32_t recordCount = 0;
        uint32_t recordsFileOffset = 0;
        uint32_t stringFileOffset = 0;
        uint32_t stringSize = 0;
        const uint32_t* idList = nullptr;
        uint32_t idListCount = 0;
        std::vector<std::pair<uint32_t, uint32_t>> copyTable;  // (newId, srcId)
        std::vector<std::pair<uint32_t, uint16_t>> offsetMap;  // (fileOffset, size)
        std::vector<uint32_t> offsetMapIds;
        std::unordered_map<uint32_t, uint32_t> relationship;   // recordIndex -> foreignId
        uint32_t stringBaseInBlob = 0;
    };

    struct FieldMeta
    {
        int storageType = 0;
        uint32_t fieldOffsetBits = 0;
        uint32_t fieldSizeBits = 0;
        uint32_t diskBytes = 4;      // bytes one element occupies on disk
        uint32_t elementBytes = 4;
        uint32_t elementCount = 1;
        uint32_t palletBase = 0;
        uint32_t palletBytes = 0;
        int32_t  defaultValue = 0;
        bool     signExt = false;
        uint32_t outByteOffset = 0;
        bool     isInlineId = false;
        std::unordered_map<uint32_t, uint32_t> common;
    };

    struct Parsed
    {
        uint32_t version = 0;
        uint32_t recordSize = 0;
        uint32_t totalFieldCount = 0;
        uint16_t flags = 0;
        uint16_t idIndex = 0;
        uint32_t minId = 0;
        uint32_t maxId = 0;
        const FieldStructure*   fieldStruct = nullptr;
        const FieldStorageInfo* fieldInfo = nullptr;
        uint32_t fieldInfoCount = 0;
        bool     hasIdList = false;
        const uint8_t* palletData = nullptr;
        uint32_t palletDataSize = 0;
        const uint8_t* commonData = nullptr;
        uint32_t commonDataSize = 0;
        std::vector<Section> sections;
    };

    bool ParseWDC1(const uint8_t* d, uint32_t size, Parsed& out)
    {
        if (size < sizeof(Wdc1Header)) return false;
        const auto& h = *reinterpret_cast<const Wdc1Header*>(d);

        out.version = 1;
        out.recordSize = h.recordSize;
        out.totalFieldCount = h.totalFieldCount;
        out.flags = h.flags;
        out.idIndex = h.idIndex;
        out.minId = h.minId;
        out.maxId = h.maxId;

        uint64_t cur = sizeof(Wdc1Header);

        out.fieldStruct = At<FieldStructure>(d, size, cur, h.totalFieldCount);
        if (!out.fieldStruct && h.totalFieldCount) return false;
        cur += static_cast<uint64_t>(h.totalFieldCount) * sizeof(FieldStructure);

        Section sec;
        const bool offsetMap = (h.flags & 0x01) != 0;
        if (!offsetMap)
        {
            if (!InRange(size, cur, static_cast<uint64_t>(h.recordCount) * h.recordSize)) return false;
            sec.records = d + cur;
            sec.recordCount = h.recordCount;
            sec.recordsFileOffset = cur;
            cur += static_cast<uint64_t>(h.recordCount) * h.recordSize;
            sec.stringFileOffset = cur;
            sec.stringSize = h.stringTableSize;
            if (!InRange(size, cur, h.stringTableSize)) return false;
            cur += h.stringTableSize;
        }
        else
        {
            cur = h.offsetMapOffset;
            const uint64_t entries = (h.maxId >= h.minId)
                                   ? (static_cast<uint64_t>(h.maxId) - h.minId + 1) : 0;
            if (!InRange(size, cur, entries * 6)) return false;
            for (uint64_t i = 0; i < entries; ++i)
            {
                uint32_t off; uint16_t len;
                std::memcpy(&off, d + cur, 4); cur += 4;
                std::memcpy(&len, d + cur, 2); cur += 2;
                if (off != 0 && len != 0) sec.offsetMap.emplace_back(off, len);
            }
            sec.recordCount = static_cast<uint32_t>(sec.offsetMap.size());
        }

        if (h.idListSize)
        {
            sec.idList = At<uint32_t>(d, size, cur, h.idListSize / 4);
            if (!sec.idList) return false;
            sec.idListCount = h.idListSize / 4;
            cur += h.idListSize;
            out.hasIdList = true;
        }
        if (h.copyTableSize)
        {
            const uint32_t n = h.copyTableSize / 8;
            const uint32_t* ct = At<uint32_t>(d, size, cur, static_cast<uint64_t>(n) * 2);
            if (!ct) return false;
            for (uint32_t i = 0; i < n; ++i) sec.copyTable.emplace_back(ct[2 * i], ct[2 * i + 1]);
            cur += h.copyTableSize;
        }

        out.fieldInfo = At<FieldStorageInfo>(d, size, cur, h.fieldStorageInfoSize / sizeof(FieldStorageInfo));
        if (!out.fieldInfo && h.fieldStorageInfoSize) return false;
        out.fieldInfoCount = h.fieldStorageInfoSize / sizeof(FieldStorageInfo);
        cur += h.fieldStorageInfoSize;

        if (!InRange(size, cur, h.palletDataSize)) return false;
        out.palletData = d + cur;
        out.palletDataSize = h.palletDataSize;
        cur += h.palletDataSize;

        if (!InRange(size, cur, h.commonDataSize)) return false;
        out.commonData = d + cur;
        out.commonDataSize = h.commonDataSize;
        cur += h.commonDataSize;

        if (h.relationshipDataSize)
        {
            if (!InRange(size, cur, h.relationshipDataSize) || h.relationshipDataSize < 12) return false;
            uint32_t numEntries;
            std::memcpy(&numEntries, d + cur, 4);
            // The entries have to fit the block the header declared, not merely the file.
            if (static_cast<uint64_t>(numEntries) * 8 > static_cast<uint64_t>(h.relationshipDataSize) - 12)
                return false;
            const uint32_t* e = At<uint32_t>(d, size, cur + 12, static_cast<uint64_t>(numEntries) * 2);
            if (!e) return false;
            for (uint32_t i = 0; i < numEntries; ++i)
                sec.relationship[e[2 * i + 1]] = e[2 * i];
            cur += h.relationshipDataSize;
        }

        out.sections.push_back(std::move(sec));
        return true;
    }

    bool ParseWDC23(const uint8_t* d, uint32_t size, Parsed& out, uint32_t version)
    {
        if (size < sizeof(Wdc23Header)) return false;
        const auto& h = *reinterpret_cast<const Wdc23Header*>(d);

        out.version = version;
        out.recordSize = h.recordSize;
        out.totalFieldCount = h.totalFieldCount;
        out.flags = h.flags;
        out.idIndex = h.idIndex;
        out.minId = h.minId;
        out.maxId = h.maxId;

        uint64_t cur = sizeof(Wdc23Header);
        const uint32_t sectionStride = (version == 3) ? sizeof(Wdc3Section) : sizeof(Wdc2Section);
        if (!InRange(size, cur, static_cast<uint64_t>(h.sectionCount) * sectionStride)) return false;
        const uint8_t* sectionHeaders = d + cur;
        cur += static_cast<uint64_t>(h.sectionCount) * sectionStride;

        out.fieldStruct = At<FieldStructure>(d, size, cur, h.totalFieldCount);
        if (!out.fieldStruct && h.totalFieldCount) return false;
        cur += static_cast<uint64_t>(h.totalFieldCount) * sizeof(FieldStructure);

        out.fieldInfo = At<FieldStorageInfo>(d, size, cur, h.fieldStorageInfoSize / sizeof(FieldStorageInfo));
        if (!out.fieldInfo && h.fieldStorageInfoSize) return false;
        out.fieldInfoCount = h.fieldStorageInfoSize / sizeof(FieldStorageInfo);
        cur += h.fieldStorageInfoSize;

        if (!InRange(size, cur, h.palletDataSize)) return false;
        out.palletData = d + cur;
        out.palletDataSize = h.palletDataSize;
        cur += h.palletDataSize;

        if (!InRange(size, cur, h.commonDataSize)) return false;
        out.commonData = d + cur;
        out.commonDataSize = h.commonDataSize;
        cur += h.commonDataSize;

        const bool offsetMap = (h.flags & 0x01) != 0;

        for (uint32_t s = 0; s < h.sectionCount; ++s)
        {
            Section sec;
            if (version == 3)
            {
                const auto& sh = reinterpret_cast<const Wdc3Section*>(sectionHeaders)[s];
                uint64_t p = sh.fileOffset;
                if (!InRange(size, p, 0)) return false;
                if (!offsetMap)
                {
                    if (!InRange(size, p, static_cast<uint64_t>(sh.recordCount) * h.recordSize)) return false;
                    sec.records = d + p;
                    sec.recordCount = sh.recordCount;
                    sec.recordsFileOffset = p;
                    p += static_cast<uint64_t>(sh.recordCount) * h.recordSize;
                    sec.stringFileOffset = p;
                    sec.stringSize = sh.stringTableSize;
                    if (!InRange(size, p, sh.stringTableSize)) return false;
                    p += sh.stringTableSize;
                }
                else
                {
                    sec.recordsFileOffset = p;
                    p = sh.offsetRecordsEnd;
                    if (!InRange(size, p, 0)) return false;
                }
                if (sh.idListSize)
                {
                    sec.idList = At<uint32_t>(d, size, p, sh.idListSize / 4);
                    if (!sec.idList) return false;
                    sec.idListCount = sh.idListSize / 4;
                    p += sh.idListSize;
                    out.hasIdList = true;
                }
                if (sh.copyTableCount)
                {
                    const uint32_t* ct = At<uint32_t>(d, size, p, static_cast<uint64_t>(sh.copyTableCount) * 2);
                    if (!ct) return false;
                    for (uint32_t i = 0; i < sh.copyTableCount; ++i)
                        sec.copyTable.emplace_back(ct[2 * i], ct[2 * i + 1]);
                    p += static_cast<uint64_t>(sh.copyTableCount) * 8;
                }
                if (offsetMap)
                {
                    if (!InRange(size, p, static_cast<uint64_t>(sh.offsetMapIdCount) * 10)) return false;
                    const uint8_t* om = d + p;
                    const uint32_t* ids = reinterpret_cast<const uint32_t*>(om + sh.offsetMapIdCount * 6);
                    for (uint32_t i = 0; i < sh.offsetMapIdCount; ++i)
                    {
                        uint32_t off; uint16_t len;
                        std::memcpy(&off, om + i * 6, 4);
                        std::memcpy(&len, om + i * 6 + 4, 2);
                        sec.offsetMap.emplace_back(off, len);
                        sec.offsetMapIds.push_back(ids[i]);
                    }
                    sec.recordCount = sh.offsetMapIdCount;
                    p += static_cast<uint64_t>(sh.offsetMapIdCount) * 10;
                }
                if (sh.relationshipDataSize)
                {
                    if (!InRange(size, p, sh.relationshipDataSize) || sh.relationshipDataSize < 12) return false;
                    uint32_t numEntries;
                    std::memcpy(&numEntries, d + p, 4);
                    if (static_cast<uint64_t>(numEntries) * 8 > static_cast<uint64_t>(sh.relationshipDataSize) - 12)
                        return false;
                    const uint32_t* e = At<uint32_t>(d, size, p + 12, static_cast<uint64_t>(numEntries) * 2);
                    if (!e) return false;
                    for (uint32_t i = 0; i < numEntries; ++i) sec.relationship[e[2 * i + 1]] = e[2 * i];
                    // The block is sized by the header, not by what its entries happen to need; a
                    // section carrying one has to step over it whole or the next block is read out of
                    // the middle of this one.
                    p += sh.relationshipDataSize;
                }
            }
            else // WDC2
            {
                const auto& sh = reinterpret_cast<const Wdc2Section*>(sectionHeaders)[s];
                uint64_t p = sh.fileOffset;
                if (!InRange(size, p, 0)) return false;
                if (!offsetMap)
                {
                    if (!InRange(size, p, static_cast<uint64_t>(sh.recordCount) * h.recordSize)) return false;
                    sec.records = d + p;
                    sec.recordCount = sh.recordCount;
                    sec.recordsFileOffset = p;
                    p += static_cast<uint64_t>(sh.recordCount) * h.recordSize;
                    sec.stringFileOffset = p;
                    sec.stringSize = sh.stringTableSize;
                    if (!InRange(size, p, sh.stringTableSize)) return false;
                    p += sh.stringTableSize;
                }
                else
                {
                    sec.recordsFileOffset = p;
                    const uint64_t entries = (h.maxId >= h.minId)
                                   ? (static_cast<uint64_t>(h.maxId) - h.minId + 1) : 0;
                    uint32_t om = sh.offsetMapOffset;
                    if (!InRange(size, om, entries * 6)) return false;
                    for (uint64_t i = 0; i < entries; ++i)
                    {
                        uint32_t off; uint16_t len;
                        std::memcpy(&off, d + om, 4); om += 4;
                        std::memcpy(&len, d + om, 2); om += 2;
                        if (off != 0 && len != 0) sec.offsetMap.emplace_back(off, len);
                    }
                    sec.recordCount = static_cast<uint32_t>(sec.offsetMap.size());
                    p = om;
                }
                if (sh.idListSize)
                {
                    sec.idList = At<uint32_t>(d, size, p, sh.idListSize / 4);
                    if (!sec.idList) return false;
                    sec.idListCount = sh.idListSize / 4;
                    p += sh.idListSize;
                    out.hasIdList = true;
                }
                if (sh.copyTableSize)
                {
                    const uint32_t n = sh.copyTableSize / 8;
                    const uint32_t* ct = At<uint32_t>(d, size, p, static_cast<uint64_t>(n) * 2);
                    if (!ct) return false;
                    for (uint32_t i = 0; i < n; ++i) sec.copyTable.emplace_back(ct[2 * i], ct[2 * i + 1]);
                    p += sh.copyTableSize;
                }
                if (sh.relationshipDataSize)
                {
                    if (!InRange(size, p, sh.relationshipDataSize) || sh.relationshipDataSize < 12) return false;
                    uint32_t numEntries;
                    std::memcpy(&numEntries, d + p, 4);
                    if (static_cast<uint64_t>(numEntries) * 8 > static_cast<uint64_t>(sh.relationshipDataSize) - 12)
                        return false;
                    const uint32_t* e = At<uint32_t>(d, size, p + 12, static_cast<uint64_t>(numEntries) * 2);
                    if (!e) return false;
                    for (uint32_t i = 0; i < numEntries; ++i) sec.relationship[e[2 * i + 1]] = e[2 * i];
                    // The block is sized by the header, not by what its entries happen to need; a
                    // section carrying one has to step over it whole or the next block is read out of
                    // the middle of this one.
                    p += sh.relationshipDataSize;
                }
            }
            out.sections.push_back(std::move(sec));
        }
        return true;
    }

    inline uint32_t AlignUp(uint32_t o, uint32_t a) { return (o + a - 1) & ~(a - 1); }

    inline uint32_t StdWidth(uint32_t bytes)
    {
        if (bytes <= 1) return 1;
        if (bytes == 2) return 2;
        if (bytes <= 4) return 4;
        return 8;
    }

    struct Layout
    {
        uint32_t rowSize = 0;
        uint32_t idOffset = 0;
        uint32_t relOffset = 0;
    };

    Layout BuildLayout(const Parsed& p, std::vector<FieldMeta>& meta, bool hasRel)
    {
        meta.resize(p.fieldInfoCount);
        const bool inlineId = !p.hasIdList;

        uint32_t palletCursor = 0, commonCursor = 0;
        Layout lay;

        uint32_t o = 0, maxAlign = 4;
        lay.idOffset = o; o += 4;

        // A field's element count and its on-disk reach are both file-controlled. Without a ceiling a
        // crafted header turns one field into a multi-gigabyte record (or wraps the layout arithmetic),
        // so anything that cannot describe a real DB2 row fails the parse here.
        constexpr uint32_t kMaxRecordBytes = 0x10000;  // no retail table record comes near 64 KiB
        constexpr uint32_t kMaxElements = 0x1000;      // nor does any single field that many elements

        for (uint32_t i = 0; i < p.fieldInfoCount; ++i)
        {
            const FieldStorageInfo& fi = p.fieldInfo[i];
            FieldMeta& m = meta[i];
            m.storageType = static_cast<int>(fi.storageType);
            m.fieldOffsetBits = fi.fieldOffsetBits;
            m.fieldSizeBits = fi.fieldSizeBits;

            uint32_t diskBytes = 4;
            if (i < p.totalFieldCount && p.fieldStruct)
            {
                const int16_t sz = p.fieldStruct[i].size;
                if (sz < 0 || sz > 32) return Layout{}; // width outside anything StdWidth can express
                diskBytes = static_cast<uint32_t>((32 - sz) / 8);
                if (diskBytes == 0) diskBytes = 4;
            }
            m.elementBytes = StdWidth(diskBytes);

            // A bitpacked field's on-disk width comes from its bit span instead of the field-structure
            // block; both spellings describe the same one element.
            m.diskBytes = (fi.storageType == 1)
                        ? static_cast<uint32_t>((fi.fieldSizeBits + 7u) / 8u)
                        : diskBytes;
            if (m.diskBytes == 0 || m.diskBytes > 8) return Layout{};

            switch (fi.storageType)
            {
                case 0:
                {
                    uint32_t elemBits = diskBytes * 8;
                    uint32_t totalBits = fi.fieldSizeBits ? fi.fieldSizeBits : elemBits;
                    m.elementCount = elemBits ? (totalBits / elemBits) : 1;
                    if (m.elementCount == 0) m.elementCount = 1;
                    break;
                }
                case 2:
                {
                    m.elementCount = 1;
                    m.defaultValue = static_cast<int32_t>(fi.p1);
                    const uint32_t n = fi.additionalDataSize / 8;
                    if (static_cast<uint64_t>(commonCursor) + static_cast<uint64_t>(n) * 8 <= p.commonDataSize)
                    {
                        for (uint32_t k = 0; k < n; ++k)
                        {
                            uint32_t key = 0, value = 0;
                            std::memcpy(&key, p.commonData + commonCursor + k * 8, 4);
                            std::memcpy(&value, p.commonData + commonCursor + k * 8 + 4, 4);
                            m.common[key] = value;
                        }
                    }
                    commonCursor += fi.additionalDataSize;
                    break;
                }
                case 1:
                    m.elementCount = 1;
                    m.signExt = (fi.p3 & 0x01) != 0;
                    break;
                case 3:
                    m.elementCount = 1;
                    m.palletBase = palletCursor;
                    m.palletBytes = fi.additionalDataSize;
                    palletCursor += fi.additionalDataSize;
                    break;
                case 4:
                    m.elementCount = fi.p3 ? fi.p3 : 1;
                    m.palletBase = palletCursor;
                    m.palletBytes = fi.additionalDataSize;
                    palletCursor += fi.additionalDataSize;
                    break;
                default:
                    m.elementCount = 1;
                    break;
            }

            // Both cursors walk file-controlled sizes. Past the end of its own blob a pallet or
            // common-data block cannot describe anything, so the table is rejected rather than decoded
            // against whatever neighbouring bytes happen to follow.
            if (palletCursor > p.palletDataSize || commonCursor > p.commonDataSize) return Layout{};

            if (inlineId && i == p.idIndex)
            {
                m.isInlineId = true;
                continue;
            }

            if (m.elementCount > kMaxElements) return Layout{};

            if (m.elementBytes > maxAlign) maxAlign = m.elementBytes;
            o = AlignUp(o, m.elementBytes);
            m.outByteOffset = o;
            o += m.elementBytes * m.elementCount;
            if (o > kMaxRecordBytes) return Layout{};
        }

        if (hasRel)
        {
            o = AlignUp(o, 4);
            lay.relOffset = o;
            o += 4;
        }
        lay.rowSize = AlignUp(o, maxAlign);
        return lay;
    }

    inline void WriteVal(char* dst, uint32_t width, uint64_t v)
    {
        for (uint32_t b = 0; b < width && b < 8; ++b) dst[b] = static_cast<char>((v >> (8 * b)) & 0xFF);
    }

    /// Reads the pallet dword at index idx, or 0 when it lies outside the declared pallet bytes.
    inline uint32_t PalletAt(const Parsed& p, const FieldMeta& m, uint64_t idx)
    {
        if (idx > static_cast<uint64_t>(m.palletBytes) / 4) return 0;
        const uint64_t at = idx * 4;
        if (at + 4 > m.palletBytes) return 0;
        if (m.palletBase + at + 4 > p.palletDataSize) return 0;
        uint32_t v;
        std::memcpy(&v, p.palletData + m.palletBase + at, 4);
        return v;
    }

    void DecodeField(const FieldMeta& m, const Parsed& p, const uint8_t* rec, int32_t id, char* recOut)
    {
        if (m.isInlineId) return;
        char* dst = recOut + m.outByteOffset;
        switch (m.storageType)
        {
            case 0:
            {
                const uint32_t diskBytes = m.diskBytes;
                uint32_t baseByte = m.fieldOffsetBits / 8;
                for (uint32_t e = 0; e < m.elementCount; ++e)
                {
                    uint64_t v = 0;
                    const uint8_t* src = rec + baseByte + e * diskBytes;
                    for (uint32_t b = 0; b < diskBytes && b < 8; ++b) v |= static_cast<uint64_t>(src[b]) << (8 * b);
                    WriteVal(dst + e * m.elementBytes, m.elementBytes, v);
                }
                break;
            }
            case 1:
            {
                uint64_t v = ReadBitsU(rec, m.fieldOffsetBits, m.fieldSizeBits);
                int32_t iv = m.signExt ? SignExtend(v, m.fieldSizeBits) : static_cast<int32_t>(v);
                WriteVal(dst, m.elementBytes, static_cast<uint32_t>(iv));
                break;
            }
            case 2:
            {
                auto it = m.common.find(static_cast<uint32_t>(id));
                int32_t iv = (it != m.common.end()) ? static_cast<int32_t>(it->second) : m.defaultValue;
                WriteVal(dst, m.elementBytes, static_cast<uint32_t>(iv));
                break;
            }
            case 3:
            {
                uint64_t idx = ReadBitsU(rec, m.fieldOffsetBits, m.fieldSizeBits);
                WriteVal(dst, m.elementBytes, PalletAt(p, m, idx));
                break;
            }
            case 4:
            {
                uint64_t idx = ReadBitsU(rec, m.fieldOffsetBits, m.fieldSizeBits);
                for (uint32_t a = 0; a < m.elementCount; ++a)
                    WriteVal(dst + a * m.elementBytes, m.elementBytes, PalletAt(p, m, idx * m.elementCount + a));
                break;
            }
            default:
                break;
        }
    }

    int32_t ReadScalar(const FieldMeta& m, const Parsed& p, const uint8_t* rec)
    {
        switch (m.storageType)
        {
            case 0:
            {
                const uint32_t diskBytes = m.diskBytes;
                uint64_t v = 0;
                const uint8_t* src = rec + m.fieldOffsetBits / 8;
                for (uint32_t b = 0; b < diskBytes && b < 8; ++b) v |= static_cast<uint64_t>(src[b]) << (8 * b);
                return static_cast<int32_t>(v);
            }
            case 1:
                return static_cast<int32_t>(ReadBitsU(rec, m.fieldOffsetBits, m.fieldSizeBits));
            case 3:
            {
                uint64_t idx = ReadBitsU(rec, m.fieldOffsetBits, m.fieldSizeBits);
                return static_cast<int32_t>(PalletAt(p, m, idx));
            }
            default:
                return 0;
        }
    }

    int32_t ResolveId(const Parsed& p, const std::vector<FieldMeta>& meta, const Section& sec,
                      const uint8_t* rec, uint32_t recordIndex)
    {
        if (sec.idList && recordIndex < sec.idListCount)
            return static_cast<int32_t>(sec.idList[recordIndex]);
        if (p.idIndex < meta.size())
            return ReadScalar(meta[p.idIndex], p, rec);
        return static_cast<int32_t>(recordIndex);
    }
}

bool DecodeDB2(const uint8_t* data, uint32_t size, DB2Decoded& out, const uint32_t* stringColumns,
               uint32_t stringColumnCount)
{
    out = DB2Decoded{};
    if (!data || size < 4) return false;

    uint32_t magic;
    std::memcpy(&magic, data, 4);

    Parsed p;
    bool ok = false;
    if (magic == kMagicWDC1) ok = ParseWDC1(data, size, p);
    else if (magic == kMagicWDC2 || magic == kMagic1SLC) ok = ParseWDC23(data, size, p, 2);
    else if (magic == kMagicWDC3) ok = ParseWDC23(data, size, p, 3);
    else return false;
    if (!ok || !p.recordSize) return false;

    // Every remaining read is against a record of recordSize bytes, so a field whose declared reach
    // starts or ends outside that record is a malformed table rather than something to decode and hope
    // about. Checking only the start would let a wide element run off the end of the last record.
    for (uint32_t i = 0; i < p.fieldInfoCount; ++i)
    {
        const FieldStorageInfo& fi = p.fieldInfo[i];
        const uint64_t startByte = fi.fieldOffsetBits / 8;
        uint64_t reach = 0;
        if (fi.storageType == 0)
        {
            // Inline: whole element bytes, or the declared bit span when it is wider.
            reach = fi.fieldSizeBits ? (fi.fieldSizeBits + 7u) / 8u : 0;
            if (reach == 0 && i < p.totalFieldCount && p.fieldStruct)
            {
                const int16_t sz = p.fieldStruct[i].size;
                if (sz < 0 || sz > 32) return false;
                reach = static_cast<uint32_t>((32 - sz) / 8);
            }
        }
        else if (fi.storageType == 1)
        {
            reach = (static_cast<uint64_t>(fi.fieldOffsetBits & 7u) + fi.fieldSizeBits + 7u) / 8u;
        }
        if (startByte > p.recordSize || reach > p.recordSize - startByte) return false;
    }

    std::vector<FieldMeta> meta;
    bool hasRel = false;
    for (const auto& s : p.sections) if (!s.relationship.empty()) hasRel = true;
    out.hasRelationship = hasRel;

    Layout lay = BuildLayout(p, meta, hasRel);
    if (!lay.rowSize) return false; // BuildLayout rejected a field's declared shape
    out.rowSize = lay.rowSize;

    for (auto& s : p.sections)
    {
        s.stringBaseInBlob = static_cast<uint32_t>(out.strings.size());
        if (s.stringSize)
        {
            if (!InRange(size, s.stringFileOffset, s.stringSize)) return false;
            const char* st = reinterpret_cast<const char*>(data + s.stringFileOffset);
            out.strings.insert(out.strings.end(), st, st + s.stringSize);
        }
    }
    if (out.strings.empty()) out.strings.push_back('\0');

    const bool multiSection = p.sections.size() > 1;

    std::unordered_map<uint32_t, uint32_t> idToRow;
    for (const auto& sec : p.sections)
    {
        const bool offsetMap = !sec.offsetMap.empty();
        for (uint32_t i = 0; i < sec.recordCount; ++i)
        {
            const uint8_t* rec;
            if (offsetMap)
            {
                const auto& ent = sec.offsetMap[i];
                if (!InRange(size, ent.first, ent.second)) return false;
                rec = data + ent.first;
            }
            else
            {
                const uint64_t off = static_cast<uint64_t>(sec.recordsFileOffset) +
                                     static_cast<uint64_t>(i) * p.recordSize;
                if (!InRange(size, off, p.recordSize)) return false;
                rec = data + off;
            }

            int32_t id;
            if (offsetMap)
                id = !sec.offsetMapIds.empty() ? static_cast<int32_t>(sec.offsetMapIds[i])
                                               : static_cast<int32_t>(p.minId + i);
            else
                id = ResolveId(p, meta, sec, rec, i);

            uint32_t rowStart = static_cast<uint32_t>(out.records.size());
            out.records.resize(rowStart + out.rowSize);
            char* recOut = out.records.data() + rowStart;

            WriteVal(recOut + lay.idOffset, 4, static_cast<uint32_t>(id));

            for (uint32_t f = 0; f < meta.size(); ++f)
                DecodeField(meta[f], p, rec, id, recOut);

            if (hasRel)
            {
                auto it = sec.relationship.find(i);
                WriteVal(recOut + lay.relOffset, 4, (it != sec.relationship.end()) ? it->second : 0u);
            }

            if (stringColumns && stringColumnCount && (p.version >= 2 || multiSection) && !offsetMap)
            {
                for (uint32_t sc = 0; sc < stringColumnCount; ++sc)
                {
                    uint32_t f = stringColumns[sc];
                    if (f >= meta.size() || meta[f].isInlineId) continue;
                    if (meta[f].elementBytes != 4) continue; // an offset needs a 32-bit slot
                    char* dst = recOut + meta[f].outByteOffset;
                    uint32_t raw; std::memcpy(&raw, dst, 4);
                    if (raw == 0) continue;
                    uint32_t fixed;
                    if (p.version >= 2)
                    {
                        // WDC2+ stores the offset relative to the field's own position in the file blob.
                        const uint64_t fieldFilePos = static_cast<uint64_t>(sec.recordsFileOffset) +
                                                      static_cast<uint64_t>(i) * p.recordSize +
                                                      meta[f].fieldOffsetBits / 8u;
                        if (fieldFilePos < sec.stringFileOffset) continue;
                        fixed = static_cast<uint32_t>(raw + (fieldFilePos - sec.stringFileOffset) +
                                                      sec.stringBaseInBlob);
                    }
                    else
                    {
                        fixed = raw + sec.stringBaseInBlob;
                    }
                    if (fixed >= out.strings.size()) continue; // not an offset into the string block
                    std::memcpy(dst, &fixed, 4);
                }
            }

            idToRow[static_cast<uint32_t>(id)] = static_cast<uint32_t>(out.ids.size());
            out.ids.push_back(id);
        }
    }

    for (const auto& sec : p.sections)
    {
        for (const auto& cp : sec.copyTable)
        {
            auto it = idToRow.find(cp.second);
            if (it == idToRow.end()) continue;
            // 64-bit so a table bigger than the addressable range cannot wrap the source index into
            // another row's bytes.
            const size_t srcStart = static_cast<size_t>(it->second) * out.rowSize;
            const size_t dstStart = out.records.size();
            out.records.resize(dstStart + out.rowSize);
            std::memcpy(out.records.data() + dstStart, out.records.data() + srcStart, out.rowSize);
            // The copied record still carries the source row's id in its id slot; the copy exists
            // under its own id, so that slot has to say so or a consumer reading the row gets the
            // wrong id back from the record it just looked up.
            uint32_t copyId = cp.first;
            std::memcpy(out.records.data() + dstStart, &copyId, sizeof(copyId));
            idToRow[cp.first] = static_cast<uint32_t>(out.ids.size());
            out.ids.push_back(static_cast<int32_t>(cp.first));
        }
    }

    if (out.ids.empty())
    {
        out = DB2Decoded{};
        return false;
    }
    return true;
}
}
