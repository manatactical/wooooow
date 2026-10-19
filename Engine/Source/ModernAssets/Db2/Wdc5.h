#ifndef ENGINE_SOURCE_MODERNASSETS_DB2_WDC5_H
#define ENGINE_SOURCE_MODERNASSETS_DB2_WDC5_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

// WDC5 reader: the modern database revision that replaced the fixed record with a bitpacked row whose
// columns may be inline, bitpacked, common-data backed or palletised. Unlike the WDC1/2/3 reader
// (Db2Decode.h), which reconstructs a flat native-width record, this one is declarative: the caller
// describes the table's physical fields and gets rows back addressable by field index, because a WDC5
// record has no self-describing layout to lean on.
//
// Ported from WarcraftXL wxl-db2 decode/Wdc5.{hpp,cpp} (GPL-3.0-or-later, Copyright (C) 2026
// WarcraftXL). Every offset, count and pallet index is bounds-checked here; the upstream reader trusts
// them.
namespace modern::db2::wdc5
{
    /// One physical field of the table, in file order. Arrays use elements > 1.
    struct FieldShape
    {
        uint16_t elements = 1;
    };

    /**
     * @brief One decoded row: its id, its relationship parent (0 when the table carries none), and
     *        every field flattened into one allocation, field by field in declaration order.
     */
    struct Row
    {
        uint32_t id = 0;
        uint32_t parentId = 0;
        // Flattened, not a vector per field: retail item tables run to hundreds of thousands of rows
        // and a vector each would be millions of client-heap allocations.
        std::vector<uint32_t> values;
    };

    class Table
    {
    public:
        /**
         * @brief Decodes one table.
         * @param bytes               the file bytes.
         * @param size                byte size of those bytes.
         * @param shapes              one entry per physical field, in file order.
         * @param expectedLayoutHash  refuse the file unless its layout hash matches this (0 = accept any).
         * @param error               receives a human-readable reason on failure.
         * @return true when the table decoded completely.
         */
        bool Load(const void* bytes, size_t size, const std::vector<FieldShape>& shapes,
                  uint32_t expectedLayoutHash, std::string* error = nullptr);

        const Row* Find(uint32_t id) const noexcept;
        uint32_t Value(const Row& row, size_t field, size_t element = 0) const noexcept;
        size_t ElementCount(size_t field) const noexcept;
        const std::vector<Row>& Rows() const noexcept { return rows_; }
        uint32_t LayoutHash() const noexcept { return layoutHash_; }
        uint32_t TableHash() const noexcept { return tableHash_; }
        /// The schema string the file carries about itself (empty when it has none).
        const std::string& Schema() const noexcept { return schema_; }

    private:
        uint32_t layoutHash_ = 0;
        uint32_t tableHash_ = 0;
        std::string schema_;
        std::vector<Row> rows_;
        std::unordered_map<uint32_t, size_t> byId_;
        std::vector<size_t> fieldOffsets_;
    };
}

#endif
