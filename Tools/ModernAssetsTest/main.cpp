// Offline check for the modern DB2 decoder. Not part of the client.
//
//   ModernAssetsTest              runs the built-in self-check and exits non-zero on failure
//   ModernAssetsTest <file.db2>   decodes one table and reports what came out
//
// The self-check builds small synthetic WDC1 / WDC2 / WDC3 tables covering every field storage type
// (inline, bitpacked, common-data, pallet, pallet-array), an id list, a copy table, a declared string
// column, and then truncated / hostile copies of them, so a regression in the decoder is caught
// without a game install.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#endif

#include "Engine/Source/ModernAssets/Db2/Db2Decode.h"
#include "Engine/Source/ModernAssets/Db2/Db2Table.h"
#include "Engine/Source/ModernAssets/Db2/LightTables.h"
#include "Engine/Source/ModernAssets/Db2/Wdc5.h"
#include "Engine/Source/ModernAssets/Dbc/DbcMerge.h"
#include "Engine/Source/ModernAssets/Dbc/DbcStringFields.h"
#include "Engine/Source/ModernAssets/M2/M2Modern.h"
#include "Engine/Source/ModernAssets/ModernAssetsFile.h"

namespace
{
    int s_failures = 0;

    void Check(bool condition, const char* what)
    {
        if (condition) return;
        std::printf("FAILED: %s\n", what);
        ++s_failures;
    }

    struct Writer
    {
        std::vector<uint8_t> bytes;

        template <class T>
        void Add(T value)
        {
            uint8_t raw[sizeof(T)];
            std::memcpy(raw, &value, sizeof(T));
            bytes.insert(bytes.end(), raw, raw + sizeof(T));
        }

        void Blob(const void* data, size_t size)
        {
            const uint8_t* p = static_cast<const uint8_t*>(data);
            bytes.insert(bytes.end(), p, p + size);
        }

        size_t Size() const { return bytes.size(); }
    };

    template <class T>
    T Read(const std::vector<char>& bytes, size_t offset)
    {
        T value{};
        if (offset + sizeof(T) <= bytes.size()) std::memcpy(&value, bytes.data() + offset, sizeof(T));
        return value;
    }

    template <class T>
    T Read(const std::vector<uint8_t>& bytes, size_t offset)
    {
        T value{};
        if (offset + sizeof(T) <= bytes.size()) std::memcpy(&value, bytes.data() + offset, sizeof(T));
        return value;
    }

    /// Bounds-checked read out of a raw buffer of a stated size.
    template <class T>
    T Read(const uint8_t* bytes, size_t offset, size_t size)
    {
        T value{};
        if (bytes && offset + sizeof(T) <= size) std::memcpy(&value, bytes + offset, sizeof(T));
        return value;
    }

    // ---- one shared table shape ------------------------------------------------------------------
    // Six columns, one per storage type plus an inline id:
    //   0 inline u16              (byte 4, width 2)
    //   1 bitpacked id, 16 bits   (bit 0)          <- idIndex
    //   2 bitpacked signed, 5 bits (bit 16)
    //   3 common data, default 7
    //   4 pallet (scalar)
    //   5 pallet array, 2 elements
    //
    // Records are 8 bytes: [bitpacked id|flag][inline u16 @4][reserved].
    constexpr uint32_t kRecordSize = 8;

    // Expected decoded layout, computed from the rules in DecodeDB2's BuildLayout:
    //   [0..3] id, [4..5] inline, [6] signed flag, [8..11] common, [12..15] pallet, [16..23] array
    constexpr uint32_t kRowSize = 24;
    constexpr uint32_t kOffInline = 4;
    constexpr uint32_t kOffFlag   = 6;
    constexpr uint32_t kOffCommon = 8;
    constexpr uint32_t kOffPallet = 12;
    constexpr uint32_t kOffArray  = 16;

    void EmitFieldStruct(Writer& w)
    {
        // size 16 -> 2-byte element; size 24 -> 1-byte; size 0 -> 4-byte.
        for (int16_t size : { 16, 24, 24, 0, 0, 0 })
        {
            w.Add<int16_t>(size);
            w.Add<uint16_t>(0);
        }
    }

    void EmitFieldInfo(Writer& w)
    {
        // 0: inline, bits 32..47
        w.Add<uint16_t>(4 * 8); w.Add<uint16_t>(16); w.Add<uint32_t>(0); w.Add<uint32_t>(0);
        w.Add<uint32_t>(0); w.Add<uint32_t>(0); w.Add<uint32_t>(0);
        // 1: bitpacked id, 16 bits at 0
        w.Add<uint16_t>(0); w.Add<uint16_t>(16); w.Add<uint32_t>(0); w.Add<uint32_t>(1);
        w.Add<uint32_t>(0); w.Add<uint32_t>(0); w.Add<uint32_t>(0);
        // 2: bitpacked signed, 5 bits at 16
        w.Add<uint16_t>(16); w.Add<uint16_t>(5); w.Add<uint32_t>(0); w.Add<uint32_t>(1);
        w.Add<uint32_t>(0); w.Add<uint32_t>(0); w.Add<uint32_t>(1);
        // 3: common data, default 7, two overrides
        w.Add<uint16_t>(0); w.Add<uint16_t>(0); w.Add<uint32_t>(16); w.Add<uint32_t>(2);
        w.Add<uint32_t>(7); w.Add<uint32_t>(0); w.Add<uint32_t>(0);
        // 4: pallet, one dword per field
        w.Add<uint16_t>(0x20); w.Add<uint16_t>(2); w.Add<uint32_t>(12); w.Add<uint32_t>(3);
        w.Add<uint32_t>(0); w.Add<uint32_t>(0); w.Add<uint32_t>(0);
        // 5: pallet array, two elements
        w.Add<uint16_t>(0x28); w.Add<uint16_t>(2); w.Add<uint32_t>(24); w.Add<uint32_t>(4);
        w.Add<uint32_t>(0); w.Add<uint32_t>(0); w.Add<uint32_t>(2);
    }

    void EmitPalletAndCommon(Writer& w)
    {
        for (uint32_t v : { 1000u, 2000u, 3000u }) w.Add<uint32_t>(v);       // scalar pallet
        for (uint32_t v : { 11u, 12u, 21u, 22u, 31u, 32u }) w.Add<uint32_t>(v); // array pallet
        w.Add<uint32_t>(10); w.Add<uint32_t>(500);                            // common: 10 -> 500
        w.Add<uint32_t>(12); w.Add<uint32_t>(700);                            // common: 12 -> 700
    }

    void EmitRecords(Writer& w)
    {
        for (uint32_t r = 0; r < 3; ++r)
        {
            uint8_t rec[kRecordSize] = {};
            const uint32_t id = 10 + r;
            const int32_t  flag = -3; // 5-bit signed; also exercises sign extension
            const uint16_t inlineValue = static_cast<uint16_t>(100 + r);

            std::memcpy(rec + kOffInline, &inlineValue, 2);
            const uint32_t bits = (id & 0xFFFFu) | ((static_cast<uint32_t>(flag) & 0x1Fu) << 16);
            std::memcpy(rec, &bits, 4);

            w.Blob(rec, sizeof(rec));
        }
    }

    // "\\0ab..." -- offset 0 is the empty string, offset 1 is "a", and so on.
    const char kStrings[] = "\0a\0b\0c\0d\0e\0f\0g\0";
    constexpr uint32_t kStringSize = sizeof(kStrings) - 1;

    std::vector<uint8_t> BuildWdc1()
    {
        Writer w;
        w.Add<uint32_t>(0x31434457); // 'WDC1'
        w.Add<uint32_t>(3);          // recordCount
        w.Add<uint32_t>(6);          // fieldCount
        w.Add<uint32_t>(kRecordSize);
        w.Add<uint32_t>(kStringSize);
        w.Add<uint32_t>(0);          // tableHash
        w.Add<uint32_t>(0);          // layoutHash
        w.Add<uint32_t>(10);         // minId
        w.Add<uint32_t>(12);         // maxId
        w.Add<uint32_t>(0);          // locale
        w.Add<uint32_t>(8);          // copyTableSize (one entry)
        w.Add<uint16_t>(0);          // flags: no offset map
        w.Add<uint16_t>(1);          // idIndex
        w.Add<uint32_t>(6);          // totalFieldCount
        w.Add<uint32_t>(0);          // bitpackedDataOffset
        w.Add<uint32_t>(0);          // lookupColumnCount
        w.Add<uint32_t>(0);          // offsetMapOffset
        w.Add<uint32_t>(0);          // idListSize (inline id)
        w.Add<uint32_t>(6 * 24);     // fieldStorageInfoSize
        w.Add<uint32_t>(16);         // commonDataSize
        w.Add<uint32_t>(36);         // palletDataSize
        w.Add<uint32_t>(0);          // relationshipDataSize

        EmitFieldStruct(w);
        EmitRecords(w);
        w.Blob(kStrings, kStringSize);
        w.Add<uint32_t>(99); w.Add<uint32_t>(12); // copy: id 99 duplicates id 12

        EmitFieldInfo(w);
        EmitPalletAndCommon(w);
        return w.bytes;
    }

    // WDC3 differs in the header and in carrying one section header; the field metadata and the field
    // data blocks are byte-identical, which is the point of the shared emitters above.
    std::vector<uint8_t> BuildWdc3()
    {
        constexpr uint32_t kSectionHeaderBytes = 40;

        Writer w;
        auto writeHeader = [&](uint32_t sectionFileOffset)
        {
            w.Add<uint32_t>(0x33434457); // 'WDC3'
            w.Add<uint32_t>(3);          // recordCount
            w.Add<uint32_t>(6);          // fieldCount
            w.Add<uint32_t>(kRecordSize);
            w.Add<uint32_t>(4);          // stringTableSize (section-local)
            w.Add<uint32_t>(0);          // tableHash
            w.Add<uint32_t>(0);          // layoutHash
            w.Add<uint32_t>(1);          // minId
            w.Add<uint32_t>(3);          // maxId
            w.Add<uint32_t>(0);          // locale
            w.Add<uint16_t>(0);          // flags: no offset map
            w.Add<uint16_t>(1);          // idIndex
            w.Add<uint32_t>(6);          // totalFieldCount
            w.Add<uint32_t>(0);          // bitpackedDataOffset
            w.Add<uint32_t>(0);          // lookupColumnCount
            w.Add<uint32_t>(6 * 24);     // fieldStorageInfoSize
            w.Add<uint32_t>(16);         // commonDataSize
            w.Add<uint32_t>(36);         // palletDataSize
            w.Add<uint32_t>(1);          // sectionCount
            (void)sectionFileOffset;
        };

        constexpr uint32_t kHeaderBytes = 72;
        constexpr uint32_t kFileOffset =
            kHeaderBytes + kSectionHeaderBytes + 24 /*fieldStruct*/ + 6 * 24 /*fieldInfo*/ + 36 /*pallet*/ + 16 /*common*/;

        writeHeader(kFileOffset);

        // Section header.
        w.Add<uint64_t>(0);          // tactKeyHash
        w.Add<uint32_t>(kFileOffset);
        w.Add<uint32_t>(3);          // recordCount
        w.Add<uint32_t>(4);          // stringTableSize
        w.Add<uint32_t>(0);          // offsetRecordsEnd
        w.Add<uint32_t>(0);          // idListSize (inline id)
        w.Add<uint32_t>(0);          // relationshipDataSize
        w.Add<uint32_t>(0);          // offsetMapIdCount
        w.Add<uint32_t>(1);          // copyTableCount

        EmitFieldStruct(w);
        EmitFieldInfo(w);
        EmitPalletAndCommon(w);

        // Section payload at kFileOffset: records, strings, copy table.
        if (w.bytes.size() != kFileOffset)
        {
            std::printf("fixture bug: section offset is %zu, expected %u\n", w.bytes.size(), kFileOffset);
            return {};
        }

        EmitRecords(w);
        w.Add<uint8_t>(0); w.Add<uint8_t>('a'); w.Add<uint8_t>(0); w.Add<uint8_t>('b');
        w.Add<uint32_t>(99); w.Add<uint32_t>(12);
        return w.bytes;
    }

    // Offsets of blocks inside the WDC1 fixture, derived from how BuildWdc1 emits them: header,
    // field-structure block, records, strings, copy table, THEN the field-storage block (that is the
    // order ParseWDC1 walks them in).
    constexpr size_t kFieldStructOffset = 84;
    constexpr size_t kFieldStructBytes  = 6 * 4;
    constexpr size_t kRecordBytes       = 3 * kRecordSize;
    constexpr size_t kCopyTableBytes    = 8;

    size_t Wdc1FieldInfoOffset()
    {
        return kFieldStructOffset + kFieldStructBytes + kRecordBytes + kStringSize + kCopyTableBytes;
    }

    // ---- assertions ------------------------------------------------------------------------------
    void CheckDecodedRows(const modern::db2::DB2Decoded& out, const char* label)
    {
        char what[160];

        std::snprintf(what, sizeof(what), "%s: row size", label);
        Check(out.rowSize == kRowSize, what);

        std::snprintf(what, sizeof(what), "%s: row count includes the copy-table row", label);
        Check(out.ids.size() == 4, what);
        if (out.ids.size() != 4 || out.rowSize != kRowSize) return;

        std::snprintf(what, sizeof(what), "%s: ids", label);
        Check(out.ids[0] == 10 && out.ids[1] == 11 && out.ids[2] == 12 && out.ids[3] == 99, what);

        const std::vector<char>& r = out.records;
        std::snprintf(what, sizeof(what), "%s: id slot", label);
        Check(Read<uint32_t>(r, 0) == 10, what);
        std::snprintf(what, sizeof(what), "%s: inline u16", label);
        Check(Read<uint16_t>(r, kOffInline) == 100, what);
        std::snprintf(what, sizeof(what), "%s: bitpacked signed flag", label);
        Check(static_cast<int8_t>(r[kOffFlag]) == -3, what);
        std::snprintf(what, sizeof(what), "%s: common data override", label);
        Check(Read<uint32_t>(r, kOffCommon) == 500, what);
        std::snprintf(what, sizeof(what), "%s: pallet scalar", label);
        Check(Read<uint32_t>(r, kOffPallet) == 1000, what);
        std::snprintf(what, sizeof(what), "%s: pallet array element 0", label);
        Check(Read<uint32_t>(r, kOffArray) == 11, what);
        std::snprintf(what, sizeof(what), "%s: pallet array element 1", label);
        Check(Read<uint32_t>(r, kOffArray + 4) == 12, what);

        // Row 1 has no common-data override, so the declared default must be used.
        std::snprintf(what, sizeof(what), "%s: common-data default", label);
        Check(Read<uint32_t>(r, kRowSize + kOffCommon) == 7, what);

        // Row 2 is the one carrying an override.
        std::snprintf(what, sizeof(what), "%s: common-data override on row 2", label);
        Check(Read<uint32_t>(r, 2 * kRowSize + kOffCommon) == 700, what);

        // The copy-table row duplicates row 2's bytes under id 99.
        std::snprintf(what, sizeof(what), "%s: copy-table row id", label);
        Check(Read<uint32_t>(r, 3 * kRowSize) == 99, what);
        std::snprintf(what, sizeof(what), "%s: copy-table row payload", label);
        Check(Read<uint32_t>(r, 3 * kRowSize + kOffCommon) == 700, what);
    }

    void TestWdc1()
    {
        const std::vector<uint8_t> file = BuildWdc1();
        modern::db2::DB2Decoded out;
        const bool ok = modern::db2::DecodeDB2(file.data(), static_cast<uint32_t>(file.size()), out, nullptr, 0);
        Check(ok, "WDC1 decodes");
        if (ok) CheckDecodedRows(out, "WDC1");
    }

    void TestWdc3()
    {
        const std::vector<uint8_t> file = BuildWdc3();
        if (file.empty()) { Check(false, "WDC3 fixture builds"); return; }

        modern::db2::DB2Decoded out;
        const bool ok = modern::db2::DecodeDB2(file.data(), static_cast<uint32_t>(file.size()), out, nullptr, 0);
        Check(ok, "WDC3 decodes");
        if (ok) CheckDecodedRows(out, "WDC3");
    }

    void TestTruncated()
    {
        // Every prefix of a valid table must be rejected rather than read out of bounds. Truncating at
        // many lengths walks past each individual region's bound check.
        const std::vector<uint8_t> full = BuildWdc1();
        for (size_t size = 4; size < full.size(); size += 7)
        {
            modern::db2::DB2Decoded out;
            if (modern::db2::DecodeDB2(full.data(), static_cast<uint32_t>(size), out, nullptr, 0))
            {
                // Acceptable only if nothing was actually decoded.
                if (!out.ids.empty())
                {
                    std::printf("FAILED: truncated WDC1 at %zu bytes decoded %zu row(s)\n",
                                size, out.ids.size());
                    ++s_failures;
                    break;
                }
            }
        }
    }

    void TestHostileCounts()
    {
        // A field claiming an enormous element count must not make the layout allocate gigabytes.
        std::vector<uint8_t> file = BuildWdc1();
        // FieldStorageInfo is 24 bytes: offsetBits@0, sizeBits@2, additionalDataSize@4, storageType@8,
        // p1@12, p2@16, p3@20. Field 5 (pallet array) keeps its element count in p3.
        const size_t infoStart = Wdc1FieldInfoOffset();
        const size_t p3 = infoStart + 5 * 24 + 20;
        if (p3 + 4 <= file.size())
        {
            const uint32_t huge = 0xFFFFFFFFu;
            std::memcpy(file.data() + p3, &huge, 4);
            modern::db2::DB2Decoded out;
            Check(!modern::db2::DecodeDB2(file.data(), static_cast<uint32_t>(file.size()), out, nullptr, 0),
                  "a field with an absurd element count is rejected");
        }

        // An inline field claiming it extends far past the record must be rejected too.
        file = BuildWdc1();
        const size_t sizeBits = infoStart + 2; // field 0's declared bit span
        if (sizeBits + 2 <= file.size())
        {
            const uint16_t wide = 0xFFFF;
            std::memcpy(file.data() + sizeBits, &wide, 2);
            modern::db2::DB2Decoded out;
            Check(!modern::db2::DecodeDB2(file.data(), static_cast<uint32_t>(file.size()), out, nullptr, 0),
                  "an inline field reaching past recordSize is rejected");
        }
    }

    void TestGarbage()
    {
        const uint8_t junk[16] = { 'N', 'O', 'P', 'E', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
        modern::db2::DB2Decoded out;
        Check(!modern::db2::DecodeDB2(junk, static_cast<uint32_t>(sizeof(junk)), out, nullptr, 0),
              "unknown magic is rejected");
        Check(!modern::db2::DecodeDB2(nullptr, 0, out, nullptr, 0), "null input is rejected");
    }

    // ---- DBC add-on merge ------------------------------------------------------------------------
    // The merge re-bases each add-on row's string-block offsets into the base table's block. A field
    // the string-field map does not know is copied through verbatim and then indexes the base block at
    // the wrong place, which is what printed garbage into item and spell tooltips. These checks build
    // a two-string table by hand and assert where the offsets land.

    std::vector<uint8_t> BuildWdbc(uint32_t fieldCount, uint32_t recordSize,
                                  const std::vector<std::vector<uint8_t>>& rows,
                                  const std::string& strings)
    {
        Writer w;
        w.Add<uint32_t>(0x43424457u); // 'WDBC'
        w.Add<uint32_t>(static_cast<uint32_t>(rows.size()));
        w.Add<uint32_t>(fieldCount);
        w.Add<uint32_t>(recordSize);
        w.Add<uint32_t>(static_cast<uint32_t>(strings.size()));
        for (const std::vector<uint8_t>& row : rows) w.Blob(row.data(), row.size());
        w.Blob(strings.data(), strings.size());
        return w.bytes;
    }

    std::vector<uint8_t> Row32(std::initializer_list<uint32_t> values)
    {
        std::vector<uint8_t> row;
        for (uint32_t value : values)
        {
            uint8_t raw[4];
            std::memcpy(raw, &value, 4);
            row.insert(row.end(), raw, raw + 4);
        }
        return row;
    }

    void CheckStringAt(const std::vector<uint8_t>& dbc, uint32_t rowIndex, uint32_t fieldOffset,
                       const char* expected, const char* what)
    {
        const uint32_t recordCount = Read<uint32_t>(dbc, 4);
        const uint32_t recordSize = Read<uint32_t>(dbc, 12);
        const uint32_t stringSize = Read<uint32_t>(dbc, 16);

        if (recordCount * recordSize + 20 + stringSize > dbc.size())
        {
            Check(false, what);
            return;
        }

        const size_t rowAt = 20 + static_cast<size_t>(rowIndex) * recordSize;
        const uint32_t offset = Read<uint32_t>(dbc, rowAt + fieldOffset);
        const size_t blockAt = 20 + static_cast<size_t>(recordCount) * recordSize;

        if (offset >= stringSize)
        {
            Check(false, what);
            return;
        }

        const char* text = reinterpret_cast<const char*>(dbc.data()) + blockAt + offset;
        Check(std::strcmp(text, expected) == 0, what);
    }

    void TestDbcMerge()
    {
        // Spell-shaped rows: id, a numeric field, then (for the purpose of the check) treat the map's
        // own entry as authoritative. Using the real table keeps the test honest about which offsets
        // the generated map actually carries.
        const std::vector<uint32_t>* spellFields = nullptr;
        for (const auto& entry : modern::dbc::StringFieldTable())
            if (entry.first == "Spell") spellFields = &entry.second;

        Check(spellFields != nullptr, "the generated map knows the Spell table");
        if (!spellFields) return;
        Check(spellFields->size() == 64, "Spell carries 4 strings x 16 locales");

        const uint32_t rowSize = 936;
        Check(static_cast<uint32_t>(spellFields->back()) + 4 <= rowSize, "Spell string fields fit the row");

        // Base table: one row, and a string block whose byte 0 is deliberately NOT a NUL, so an empty
        // field cannot get away with pointing there.
        std::vector<uint8_t> baseRow(rowSize, 0);
        const uint32_t baseId = 1;
        std::memcpy(baseRow.data(), &baseId, 4);
        const uint32_t baseName = 0; // the base block's first string
        std::memcpy(baseRow.data() + (*spellFields)[0], &baseName, 4);

        std::vector<uint8_t> base = BuildWdbc(234, rowSize, { baseRow }, std::string("Base\0", 5));

        // Add-on table: a new row whose strings live in its own block, plus a field pointing outside it.
        std::vector<uint8_t> contRow(rowSize, 0);
        const uint32_t id = 2;
        std::memcpy(contRow.data(), &id, 4);

        // The map holds four string groups of 16 locales each, in the record's field order:
        // name, nameSubtext, description, auraDescription. Locale 0 of each starts a group.
        const uint32_t nameField = (*spellFields)[0];
        const uint32_t subtextField = (*spellFields)[16];
        const uint32_t descriptionField = (*spellFields)[32];
        const uint32_t auraField = (*spellFields)[48];
        Check((*spellFields)[15] + 4 <= subtextField && subtextField < (*spellFields)[31],
              "the map's string groups are contiguous per locale");

        // "Add\0" then "Subtext\0" then "Describe\0": offsets 0, 4 and 12.
        const uint32_t name = 0, subtext = 4, description = 12, aura = 30; // aura is out of range
        std::memcpy(contRow.data() + nameField, &name, 4);
        std::memcpy(contRow.data() + subtextField, &subtext, 4);
        std::memcpy(contRow.data() + descriptionField, &description, 4);
        std::memcpy(contRow.data() + auraField, &aura, 4);

        std::vector<uint8_t> cont = BuildWdbc(234, rowSize, { contRow },
                                              std::string("Add\0Subtext\0Describe\0", 21));

        std::vector<uint8_t> merged;
        uint32_t appended = 0, replaced = 0;
        Check(modern::dbc::MergeBytes("Spell", base.data(), static_cast<uint32_t>(base.size()),
                                      { cont }, merged, &appended, &replaced),
              "an add-on row is merged");
        Check(appended == 1 && replaced == 0, "one row appended, none replaced");
        Check(Read<uint32_t>(merged, 4) == 2, "merged table has both rows");

        CheckStringAt(merged, 1, nameField, "Add", "the add-on name is re-based");
        CheckStringAt(merged, 1, subtextField, "Subtext", "the add-on subtext is re-based");
        CheckStringAt(merged, 1, descriptionField, "Describe", "the add-on description is re-based");

        // An offset past the add-on block must become an empty string that actually reads as empty.
        const uint32_t recordCount = Read<uint32_t>(merged, 4);
        const uint32_t rowAt = 20 + 1 * rowSize;
        const uint32_t auraOffset = Read<uint32_t>(merged, rowAt + auraField);
        const size_t blockAt = 20 + static_cast<size_t>(recordCount) * rowSize;
        const uint32_t stringSize = Read<uint32_t>(merged, 16);
        Check(auraOffset < stringSize, "an out-of-range source offset is replaced with a real offset");
        Check(std::strcmp(reinterpret_cast<const char*>(merged.data()) + blockAt + auraOffset, "") == 0,
              "an out-of-range source offset reads as an empty string");

        // The base row's own string must still resolve, untouched by the merge.
        CheckStringAt(merged, 0, nameField, "Base", "the base row's string is unchanged");

        // An unknown table is refused rather than merged with un-based offsets.
        Check(!modern::dbc::MergeBytes("NotARealTable", base.data(), static_cast<uint32_t>(base.size()),
                                       { cont }, merged, &appended, &replaced),
              "a table without a string-field map is refused");

        // Junk add-on bytes are skipped, not trusted.
        Check(!modern::dbc::MergeBytes("Spell", base.data(), static_cast<uint32_t>(base.size()),
                                       { std::vector<uint8_t>(32, 0x5A) }, merged, &appended, &replaced),
              "a malformed add-on file is skipped");
    }



    // ---- WDC5 ----------------------------------------------------------------------------------
    // Three columns covering the compression modes a retail table actually uses, plus an inline id:
    //   0 inline id (None, 32 bits)   1 common-data   2 pallet
    // Records are 8 bytes; the id is bitpacked at 0, the pallet index at bit 32.
    std::vector<uint8_t> BuildWdc5()
    {
        struct H
        {
            uint32_t magic = 0x35434457; // 'WDC5'
            uint32_t schemaVersion = 1;
            char     schema[128] = "fixture";
            uint32_t recordCount = 3;
            uint32_t fieldCount = 3;
            uint32_t recordSize = 8;
            uint32_t stringTableSize = 0;
            uint32_t tableHash = 0x1234;
            uint32_t layoutHash = 0x5678;
            int32_t  minId = 10;
            int32_t  maxId = 12;
            uint32_t locale = 0;
            uint16_t flags = 0;
            uint16_t idIndex = 0;
            uint32_t totalFieldCount = 3;
            uint32_t bitpackedDataOffset = 0;
            uint32_t lookupColumnCount = 0;
            uint32_t fieldStorageInfoSize = 0;
            uint32_t commonDataSize = 8;
            uint32_t palletDataSize = 12;
            uint32_t sectionCount = 1;
        };
        static_assert(sizeof(H) == 204, "WDC5 fixture header");

        struct SH
        {
            uint64_t tactKeyLookup = 0;
            uint32_t fileOffset = 0;
            uint32_t recordCount = 3;
            uint32_t stringTableSize = 0;
            uint32_t offsetRecordsEnd = 0;
            uint32_t idListSize = 0;
            uint32_t relationshipDataSize = 0;
            uint32_t offsetMapIdCount = 0;
            uint32_t copyTableCount = 0;
        };

        struct FM { int16_t bits; uint16_t offset; };
        struct CM
        {
            uint16_t recordOffsetBits, sizeBits;
            uint32_t additionalDataSize, compression, value1, value2, value3;
        };

        constexpr uint32_t kHeaderBytes        = 204;
        constexpr uint32_t kSectionHeaderBytes = 40;
        constexpr uint32_t kFieldMetaBytes     = 3 * 4;
        constexpr uint32_t kColumnMetaBytes    = 3 * 24;
        constexpr uint32_t kCommonBytes        = 8;
        constexpr uint32_t kPalletBytes        = 12;
        // Layout: header, section header, field meta, column meta, common data, pallet data, records.
        const uint32_t recordsAt = kHeaderBytes + kSectionHeaderBytes + kFieldMetaBytes +
                                   kColumnMetaBytes + kCommonBytes + kPalletBytes;

        Writer w;
        H h;
        SH sh;
        sh.fileOffset = recordsAt;
        sh.recordCount = 3;

        // Field meta: bits is the value width for the None column (32 - bits), offset is bits.
        FM fm[3] = { { 0, 0 }, { 0, 32 }, { 0, 32 } };
        // Columns: id (None), common (default 77, one override), pallet (3 entries).
        CM cm[3] = {
            { 0, 32, 0,  0, 0, 32, 0 },
            { 0,  0, 8,  2, 77, 0, 0 },
            { 0,  0, 12, 3, 0, 2, 0 },
        };

        // Emit.
        w.Blob(&h, sizeof(h));
        w.Blob(&sh, sizeof(sh));
        w.Blob(fm, sizeof(fm));
        w.Blob(cm, sizeof(cm));
        w.Add<uint32_t>(11); w.Add<uint32_t>(900); // common override: row id 11 -> 900
        for (uint32_t v : { 100u, 200u, 300u }) w.Add<uint32_t>(v); // pallet

        if (w.Size() != recordsAt)
        {
            std::printf("fixture bug: WDC5 records at %zu, expected %u\n", w.Size(), recordsAt);
            return {};
        }

        for (uint32_t r = 0; r < 3; ++r)
        {
            uint8_t rec[8] = {};
            const uint32_t id = 10 + r;
            const uint32_t bits = (id & 0xFFFFu) | ((r & 3u) << 32);
            std::memcpy(rec, &bits, 4);
            rec[4] = static_cast<uint8_t>(r & 3u); // pallet index lives at bit 32
            w.Blob(rec, sizeof(rec));
        }
        return w.bytes;
    }

    void TestWdc5()
    {
        const std::vector<uint8_t> file = BuildWdc5();
        if (file.empty()) { Check(false, "WDC5 fixture builds"); return; }

        const std::vector<modern::db2::wdc5::FieldShape> shapes = { {1}, {1}, {1} };
        modern::db2::wdc5::Table table;
        std::string error;
        const bool ok = table.Load(file.data(), file.size(), shapes, 0x5678, &error);
        Check(ok, error.empty() ? "WDC5 decodes" : error.c_str());
        if (!ok) return;

        Check(table.Rows().size() == 3, "WDC5 row count");
        Check(table.TableHash() == 0x1234, "WDC5 table hash");
        Check(table.LayoutHash() == 0x5678, "WDC5 layout hash");

        const modern::db2::wdc5::Row* row0 = table.Find(10);
        const modern::db2::wdc5::Row* row1 = table.Find(11);
        Check(row0 != nullptr && row1 != nullptr, "WDC5 inline ids resolve");
        if (row0 && row1)
        {
            Check(table.Value(*row0, 0) == 10, "WDC5 id column");
            Check(table.Value(*row0, 1) == 77, "WDC5 common-data default");
            Check(table.Value(*row1, 1) == 900, "WDC5 common-data override");
            Check(table.Value(*row0, 2) == 100, "WDC5 pallet index 0");
            Check(table.Value(*row1, 2) == 200, "WDC5 pallet index 1");
        }

        // A wrong layout hash must be refused rather than silently decoding the wrong layout.
        modern::db2::wdc5::Table wrong;
        Check(!wrong.Load(file.data(), file.size(), shapes, 0xDEAD, &error),
              "WDC5 refuses a mismatched layout hash");

        // A shape that does not match the file's field count is refused too.
        const std::vector<modern::db2::wdc5::FieldShape> tooFew = { {1}, {1} };
        modern::db2::wdc5::Table mismatch;
        Check(!mismatch.Load(file.data(), file.size(), tooFew, 0, &error),
              "WDC5 refuses a field count that disagrees with the file");

        // Truncation: no prefix may decode.
        for (size_t size = 4; size < file.size(); size += 11)
        {
            modern::db2::wdc5::Table truncated;
            if (truncated.Load(file.data(), size, shapes, 0, &error) && !truncated.Rows().empty())
            {
                std::printf("FAILED: truncated WDC5 at %zu bytes decoded %zu row(s)\n",
                            size, truncated.Rows().size());
                ++s_failures;
                break;
            }
        }
    }

    bool ReadFile(const char* path, std::vector<uint8_t>& buffer);

    /// Repacks a modern model into the v264 layout and validates the result independently of the
    /// converter: every header array must land inside the body, at the client's record width, with
    /// in-range cross-references. This is what makes "the model converted" mean something.
    int ProbeModernModel(const char* path)
    {
        std::vector<uint8_t> file;
        if (!ReadFile(path, file))
        {
            std::printf("cannot read %s\n", path);
            return 2;
        }

        std::printf("%s: %zu byte(s), %s\n", path, file.size(),
                    modern::m2::IsModernContainer(file.data(), static_cast<uint32_t>(file.size()))
                        ? "MD21 container" : "not an MD21 container");

        // The converter appends resolved texture paths, so give it the same reserve the client does.
        const uint32_t originalSize = static_cast<uint32_t>(file.size());
        file.resize(originalSize + 0x4000);

        const modern::m2::Stats before = modern::m2::GetStats();

        uint32_t bodySize = originalSize;
        const char* why = nullptr;
        if (!modern::m2::Convert(file.data(), static_cast<uint32_t>(file.size()), bodySize, &why))
        {
            std::printf("  conversion refused: %s\n", why ? why : "unknown");
            return 1;
        }

        const modern::m2::Stats after = modern::m2::GetStats();
        std::printf("  converted: body %u byte(s) (was %u)\n", bodySize, originalSize);
        std::printf("  particles=%u cameras=%u textures=%u/%u sequences=%u external=%u gate=%u\n",
                    after.particlesNormalized, after.camerasNormalized,
                    after.texturesResolved, after.texturesResolved + after.texturesUnresolved,
                    after.sequencesRemapped, after.externalSequences, after.shadowGateForced);
        (void)before;

        // --- independent validation of the result --------------------------------------------
        const uint8_t* body = file.data();
        int failures = 0;

        const uint32_t magic = Read<uint32_t>(body, 0, bodySize);
        const uint32_t version = Read<uint32_t>(body, 4, bodySize);
        std::printf("  magic=%c%c%c%c version=%u\n",
                    magic & 0xFF, (magic >> 8) & 0xFF, (magic >> 16) & 0xFF, (magic >> 24) & 0xFF,
                    version);
        if (magic != 0x3032444D) { std::printf("  FAIL: not MD20\n"); ++failures; }
        if (version != 264)      { std::printf("  FAIL: version is not 264\n"); ++failures; }

        // [header offset, label, record stride]
        struct A { uint32_t at; const char* name; uint32_t stride; };
        const A arrays[] = {
            { 0x08, "name", 1 }, { 0x14, "loops", 4 }, { 0x1C, "sequences", 0x40 },
            { 0x24, "sequenceLookup", 2 }, { 0x2C, "bones", 0x58 }, { 0x34, "boneLookup", 2 },
            { 0x3C, "vertices", 0x30 }, { 0x48, "colors", 0x28 }, { 0x50, "textures", 0x10 },
            { 0x58, "textureWeights", 0x14 }, { 0x60, "textureTransforms", 0x3C },
            { 0x68, "textureReplacements", 2 }, { 0x70, "materials", 4 },
            { 0x78, "boneCombos", 2 }, { 0x80, "textureCombos", 2 },
            { 0x88, "textureUnitLookup", 2 }, { 0x90, "textureWeightCombos", 2 },
            { 0x98, "textureTransformCombos", 2 }, { 0xD8, "collisionIndices", 2 },
            { 0xE0, "collisionPositions", 0xC }, { 0xE8, "collisionNormals", 0xC },
            { 0xF0, "attachments", 0x28 }, { 0xF8, "attachmentLookup", 2 },
            { 0x100, "events", 0x24 }, { 0x108, "lights", 0x9C },
            { 0x110, "cameras", 0x64 }, { 0x118, "cameraLookup", 2 },
            { 0x120, "ribbons", 0xB0 }, { 0x128, "particles", 0x1DC },
        };

        for (const A& a : arrays)
        {
            const uint32_t count = Read<uint32_t>(body, a.at, bodySize);
            const uint32_t offset = Read<uint32_t>(body, a.at + 4, bodySize);
            if (!count) continue;
            const uint64_t end = static_cast<uint64_t>(offset) + static_cast<uint64_t>(count) * a.stride;
            if (offset > bodySize || end > bodySize)
            {
                std::printf("  FAIL: %s %u x 0x%x at %u runs past body %u\n",
                            a.name, count, a.stride, offset, bodySize);
                ++failures;
            }
        }

        // Cross-reference checks: an index the client dereferences has to be in range.
        const uint32_t boneCount = Read<uint32_t>(body, 0x2C, bodySize);
        const uint32_t boneOffset = Read<uint32_t>(body, 0x30, bodySize);
        const uint32_t textureCount = Read<uint32_t>(body, 0x50, bodySize);
        const uint32_t particleCount = Read<uint32_t>(body, 0x128, bodySize);
        const uint32_t particleOffset = Read<uint32_t>(body, 0x12C, bodySize);

        for (uint32_t i = 0; i < particleCount; ++i)
        {
            const uint32_t at = particleOffset + i * 0x1DC;
            const uint16_t bone = Read<uint16_t>(body, at + 0x14, bodySize);
            const uint16_t texture = Read<uint16_t>(body, at + 0x16, bodySize);
            if (bone >= boneCount)
            {
                std::printf("  FAIL: particle %u bone %u >= %u\n", i, bone, boneCount);
                ++failures;
                break;
            }
            if (textureCount && texture >= textureCount)
            {
                std::printf("  FAIL: particle %u texture %u >= %u\n", i, texture, textureCount);
                ++failures;
                break;
            }
            // The client's blend switch has no default; a value past its table draws the quad opaque.
            const uint8_t blend = Read<uint8_t>(body, at + 0x28, bodySize);
            if (blend > 6)
            {
                std::printf("  FAIL: particle %u blend %u is past the client's table\n", i, blend);
                ++failures;
                break;
            }
        }

        // Camera field of view has to be a plausible angle now that it is a plain float again.
        const uint32_t cameraCount = Read<uint32_t>(body, 0x110, bodySize);
        const uint32_t cameraOffset = Read<uint32_t>(body, 0x114, bodySize);
        for (uint32_t i = 0; i < cameraCount; ++i)
        {
            const uint32_t at = cameraOffset + i * 0x64;
            const float fov = Read<float>(body, at + 4, bodySize);
            if (!(fov > 0.01f && fov < 3.2f))
            {
                std::printf("  FAIL: camera %u field of view %f is not a plausible angle\n", i, fov);
                ++failures;
            }
            else
            {
                std::printf("  camera %u: fov=%.4f rad (%.1f deg)\n", i, fov, fov * 57.29578f);
            }
        }

        // Bone parents must be self-consistent, which only holds if the stride is right.
        for (uint32_t i = 0; i < boneCount; ++i)
        {
            const int16_t parent = Read<int16_t>(body, boneOffset + i * 0x58 + 8, bodySize);
            if (parent != -1 && (parent < 0 || parent >= static_cast<int32_t>(boneCount)))
            {
                std::printf("  FAIL: bone %u parent %d out of range\n", i, parent);
                ++failures;
                break;
            }
        }

        std::printf("  %s\n", failures ? "VALIDATION FAILED" : "validated: every array and index is in range");
        return failures ? 1 : 0;
    }

    /// Decodes the three lighting tables through the declarative WDC5 layer and reports each one.
    /// This is the path a caller's own Definition takes, exercised against real retail files.
    ///
    /// With no arguments it reads through the client's archive set (which a standalone tool does not
    /// have mounted); given the three loose .db2 paths it decodes those instead, so the same
    /// definitions can be proven offline.
    int ProbeLightTables(const char* lightPath, const char* dataPath, const char* paramsPath)
    {
        modern::db2::light::StoreStatus status;

        std::vector<uint8_t> lightBytes, dataBytes, paramsBytes;
        const bool loose = lightPath && dataPath && paramsPath;
        if (loose)
        {
            if (!ReadFile(lightPath, lightBytes) || !ReadFile(dataPath, dataBytes) ||
                !ReadFile(paramsPath, paramsBytes))
            {
                std::printf("cannot read the given .db2 files\n");
                return 2;
            }
            status = modern::db2::light::LoadFrom(lightBytes.data(), lightBytes.size(),
                                                 dataBytes.data(), dataBytes.size(),
                                                 paramsBytes.data(), paramsBytes.size());
        }
        else
        {
            status = modern::db2::light::Status();
        }


        std::printf("lighting tables: loaded=%s failed=%s\n",
                    status.loaded ? "yes" : "no", status.failed ? "yes" : "no");
        if (!status.error.empty()) std::printf("  error: %s\n", status.error.c_str());
        std::printf("  light=%u row(s)  lightdata=%u row(s)  lightparams=%u row(s)\n",
                    status.lights, status.data, status.params);

        if (const modern::db2::Table* params = modern::db2::light::Params())
        {
            std::printf("  lightparams: layout=0x%08x, %zu row(s)\n",
                        params->LayoutHash(), params->Rows().size());

            // Print several rows and how many of their columns are non-zero, so a decode that
            // silently returned zeroes would be visible rather than plausible.
            const size_t shown = std::min<size_t>(params->Rows().size(), 4);
            for (size_t i = 0; i < shown; ++i)
            {
                const modern::db2::wdc5::Row& row = params->Rows()[i];
                size_t nonZero = 0;
                for (uint32_t v : row.values) if (v) ++nonZero;
                std::printf("   params %u: %zu/%zu column(s) non-zero\n",
                            row.id, nonZero, row.values.size());
            }

            if (!params->Rows().empty())
            {
                modern::db2::light::LightParamsRow row;
                if (modern::db2::light::FindParams(params->Rows().front().id, row))
                    std::printf("   params %u: flags=%u sunPolar=%.4f sunAzimuth=%.4f\n",
                                row.paramsId, row.flags, row.sunPolar, row.sunAzimuth);
            }
        }

        if (const modern::db2::Table* lights = modern::db2::light::Lights())
        {
            // Print a few real rows so the decoded numbers can be compared against the file by hand;
            // an all-zero table would decode "successfully" too and prove nothing.
            const size_t shown = std::min<size_t>(lights->Rows().size(), 5);
            for (size_t i = 0; i < shown; ++i)
            {
                modern::db2::light::LightPoint point;
                if (!modern::db2::light::FindLight(lights->Rows()[i].id, point)) continue;
                std::printf("   light %u: coords=(%.1f, %.1f, %.1f) falloff=%.3f..%.3f continent=%u params=%u\n",
                            point.lightId, point.coords[0], point.coords[1], point.coords[2],
                            point.falloffStart, point.falloffEnd, point.continentId, point.paramsId);
            }

            modern::db2::light::LightPoint point;
            if (!lights->Rows().empty() && modern::db2::light::FindLight(lights->Rows().front().id, point))
            {
                std::vector<const modern::db2::wdc5::Row*> curve;
                if (modern::db2::light::RowsForParams(point.paramsId, curve))
                    std::printf("   lightdata: %zu time stamp(s) for params %u\n",
                                curve.size(), point.paramsId);
                else
                    std::printf("   lightdata: no time stamps for params %u\n", point.paramsId);
            }
        }

        return status.loaded ? 0 : 1;
    }

    bool ReadFile(const char* path, std::vector<uint8_t>& buffer)
    {
        buffer.clear();
        FILE* f = std::fopen(path, "rb");
        if (!f) return false;
        std::fseek(f, 0, SEEK_END);
        const long length = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        if (length <= 0 || static_cast<unsigned long long>(length) > 0xFFFFFFFFull)
        {
            std::fclose(f);
            return false;
        }
        buffer.resize(static_cast<size_t>(length));
        const size_t got = std::fread(buffer.data(), 1, buffer.size(), f);
        const bool error = std::ferror(f) != 0;
        std::fclose(f);
        return !error && got == buffer.size();
    }

    /// Prints only a NUL-terminated string that lives inside the block; the decoder already guarantees
    /// that for Str(), this path bypasses it deliberately to show raw offsets.
    void PrintOffsetString(const modern::db2::DB2Decoded& out, uint32_t offset)
    {
        if (offset >= out.strings.size())
        {
            std::printf("(offset %u out of range)", offset);
            return;
        }
        const char* text = out.strings.data() + offset;
        const void* end = std::memchr(text, '\0', out.strings.size() - offset);
        std::printf("%s", end ? text : "(unterminated)");
    }
}

int main(int argc, char** argv)
{
#if defined(_WIN32)
    SetConsoleOutputCP(CP_UTF8);
#endif

    // m2 <file.m2>  -> repacks a modern model and reports what the conversion produced
    if (argc > 1 && std::string(argv[1]) == "m2")
    {
        if (argc < 3)
        {
            std::printf("usage: ModernAssetsTest m2 <file.m2>\n");
            return 2;
        }
        return ProbeModernModel(argv[2]);
    }

    // lighttables                       -> through the client's archive set
    // lighttables <light> <data> <params> -> through the three loose .db2 files
    if (argc > 1 && std::string(argv[1]) == "lighttables")    {
        const bool loose = argc > 4;
        return ProbeLightTables(loose ? argv[2] : nullptr,
                                loose ? argv[3] : nullptr,
                                loose ? argv[4] : nullptr);
    }

    if (argc > 1)
    {
        std::vector<uint8_t> bytes;
        if (!ReadFile(argv[1], bytes))
        {
            std::printf("cannot read %s\n", argv[1]);
            return 2;
        }

        modern::db2::DB2Decoded out;
        if (!modern::db2::DecodeDB2(bytes.data(), static_cast<uint32_t>(bytes.size()), out, nullptr, 0))
        {
            // A WDC5 file is a different family; report what it is and what schema it declares so the
            // next step is a definition rather than a guess.
            if (bytes.size() >= 204 && Read<uint32_t>(bytes, 0) == 0x35434457u)
            {
                std::printf("%s: WDC5 (not WDC1/2/3). fields=%u recordSize=%u rows=%u\n",
                            argv[1], Read<uint32_t>(bytes, 140), Read<uint32_t>(bytes, 144),
                            Read<uint32_t>(bytes, 136));

                char schema[129] = {};
                std::memcpy(schema, bytes.data() + 8, 128);
                std::printf("   declared schema: %s\n", schema[0] ? schema : "(none)");
                std::printf("   read it by declaring a modern::db2::Definition with these fields.\n");
                return 0;
            }

            std::printf("%s: not a supported DB2 (WDC1/2/3) or malformed\n", argv[1]);
            return 1;
        }

        std::printf("%s: %zu rows, rowSize=%u, strings=%zu, relationship=%s\n",
                    argv[1], out.ids.size(), out.rowSize, out.strings.size(),
                    out.hasRelationship ? "yes" : "no");

        const size_t shown = std::min<size_t>(out.ids.size(), 8);
        if (out.records.size() < shown * out.rowSize)
        {
            std::printf("   (record buffer too short to display)\n");
            return 0;
        }

        // A two-column path table stores {id, stringOffset}; print those so a decode can be eyeballed
        // against what the table is supposed to contain.
        const bool looksLikePathTable = (out.rowSize == 8);
        for (size_t i = 0; i < shown; ++i)
        {
            std::printf("   %d", out.ids[i]);
            if (looksLikePathTable)
            {
                const int32_t offset = Read<int32_t>(out.records, i * out.rowSize + 4);
                std::printf(" -> ");
                PrintOffsetString(out, static_cast<uint32_t>(offset));
            }
            std::printf("\n");
        }
        return 0;
    }

    TestWdc1();
    TestWdc3();
    TestWdc5();
    TestTruncated();
    TestHostileCounts();
    TestGarbage();
    TestDbcMerge();

    if (s_failures)
    {
        std::printf("%d check(s) failed\n", s_failures);
        return 1;
    }
    std::printf("all modern DB2 decoder checks passed\n");
    return 0;
}
