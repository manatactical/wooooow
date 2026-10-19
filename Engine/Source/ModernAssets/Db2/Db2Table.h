#ifndef ENGINE_SOURCE_MODERNASSETS_DB2_DB2TABLE_H
#define ENGINE_SOURCE_MODERNASSETS_DB2_DB2TABLE_H

#include "Engine/Source/ModernAssets/Db2/Wdc5.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// Declarative WDC5 tables: a caller describes a table (its filename, its physical fields, optionally
// how it relates to other tables) and gets rows addressed by field NAME instead of a fragile numeric
// position. This is the layer that makes an arbitrary modern table readable without a hand-written
// decoder per table.
//
// Ported from WarcraftXL wxl-db2 api/Db2.{hpp,cpp} (GPL-3.0-or-later, Copyright (C) 2026
// WarcraftXL).
namespace modern::db2
{
    struct TransparentStringHash
    {
        using is_transparent = void;
        size_t operator()(std::string_view value) const noexcept
        {
            return std::hash<std::string_view>{}(value);
        }
    };

    /// A physical field in file order. Arrays use elements > 1.
    struct Field
    {
        std::string_view name;
        uint16_t elements = 1;
    };

    /// Where a relationship obtains its source key.
    enum class RelationSource : uint8_t
    {
        Field,
        RowId,
        ParentId,
    };

    /**
     * @brief Describes a table relationship without forcing either table to stay resident.
     *        ParentId is used for the relationship column modern tables carry inline.
     */
    struct Relation
    {
        std::string_view name;
        RelationSource source = RelationSource::Field;
        std::string_view sourceField;
        uint16_t sourceElement = 0;
        std::string_view targetTable;
        std::string_view targetField = "@id";
    };

    /**
     * @brief An immutable declaration of one WDC5 layout. Lives as long as the call that uses it.
     *
     * Every physical field must be declared, including ones nothing reads yet: the decoder
     * cross-checks the declared field count against the file, and dropping a column would silently
     * shift every field after it.
     */
    struct Definition
    {
        std::string_view name;
        std::string_view filename;
        uint32_t layoutHash = 0; ///< refuse the file unless its layout hash matches (0 = accept any)
        std::vector<Field> fields;
        std::vector<Relation> relations;
        bool required = true;
    };

    /// One loaded WDC5 table, queryable by field name.
    class Table
    {
    public:
        /**
         * @brief Loads a table described by definition through the client's archive set.
         * @param definition  what the table's layout is.
         * @param error       receives a human-readable reason on failure.
         */
        bool Load(const Definition& definition, std::string* error = nullptr);

        /**
         * @brief Loads the declared layout from bytes already in memory (a file read some other way).
         * @param definition  what the table's layout is.
         * @param bytes       the file bytes.
         * @param size        byte size of those bytes.
         * @param error       receives a human-readable reason on failure.
         */
        bool LoadBytes(const Definition& definition, const void* bytes, size_t size,
                       std::string* error = nullptr);

        const wdc5::Row* Find(uint32_t id) const noexcept { return table_.Find(id); }
        const std::vector<wdc5::Row>& Rows() const noexcept { return table_.Rows(); }

        uint32_t Value(const wdc5::Row& row, size_t field, size_t element = 0) const noexcept;
        uint32_t Value(const wdc5::Row& row, std::string_view field, size_t element = 0) const noexcept;
        /// Index of a declared field, or SIZE_MAX-style npos when it is not part of this table.
        size_t FieldIndex(std::string_view field) const noexcept;
        size_t ElementCount(std::string_view field) const noexcept;
        uint32_t RelationKey(const wdc5::Row& row, std::string_view relation) const noexcept;

        std::string_view Name() const noexcept { return name_; }
        std::string_view Filename() const noexcept { return filename_; }
        uint32_t LayoutHash() const noexcept { return table_.LayoutHash(); }
        uint32_t TableHash() const noexcept { return table_.TableHash(); }
        const std::string& EmbeddedSchema() const noexcept { return table_.Schema(); }
        bool Loaded() const noexcept { return loaded_; }

        static constexpr size_t kNoField = static_cast<size_t>(-1);

    private:
        struct RelationBinding
        {
            RelationSource source = RelationSource::Field;
            size_t field = kNoField;
            uint16_t element = 0;
        };

        std::string name_;
        std::string filename_;
        bool loaded_ = false;
        wdc5::Table table_;
        std::unordered_map<std::string, size_t, TransparentStringHash, std::equal_to<>> fields_;
        std::unordered_map<std::string, RelationBinding, TransparentStringHash, std::equal_to<>> relations_;
    };

    /// Validates names and declared links across a related set before any large table is decoded.
    bool ValidateDefinitions(const std::vector<Definition>& definitions, std::string* error = nullptr);
}

#endif
