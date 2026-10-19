#ifndef ENGINE_SOURCE_MODERNASSETS_DB2_DB2DECODE_H
#define ENGINE_SOURCE_MODERNASSETS_DB2_DB2DECODE_H

#include <cstdint>
#include <vector>

// Modern client database (.db2) decoder for the WDC1 / WDC2 / WDC3 family.
//
// Ported from WarcraftXL wxl-db2 (GPL-3.0-or-later, Copyright (C) 2026 WarcraftXL); see
// ThirdParty/README-modern-assets.md. The on-disk record may be bitpacked, palletised, common-data
// backed and split across sections; every record is reconstructed into a native-width layout
//   [id (u32)] [each field in file order at its native width] [relationship id (u32), if any]
// laid out with natural C alignment, so a row struct mirrors the table fields exactly (a uint16 field
// stays 2 bytes, an array stays consecutive). Strings stay as offsets into the rebuilt string table.
//
// Unlike the upstream original, every region and pallet index is bounds-checked against the buffer
// before it is read: a truncated or hostile file fails the decode instead of faulting.
namespace modern::db2
{
    struct DB2Decoded
    {
        uint32_t          rowSize = 0; // decoded bytes per record
        std::vector<char> records;     // rowCount * rowSize
        std::vector<int32_t> ids;      // rowCount, parallel to records
        std::vector<char> strings;     // reconstructed string table (never empty)
        bool              hasRelationship = false;
    };

    // Decodes the file bytes into out. stringColumns (FIELD indices holding a string offset) may be
    // null/0; a single-section WDC1 table stores absolute offsets and needs nothing there.
    // Returns false and leaves out cleared if the data is not a supported DB2 or is malformed.
    bool DecodeDB2(const uint8_t* data, uint32_t size, DB2Decoded& out, const uint32_t* stringColumns,
                   uint32_t stringColumnCount);
}

#endif
