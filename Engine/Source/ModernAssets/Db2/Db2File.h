#ifndef ENGINE_SOURCE_MODERNASSETS_DB2_DB2FILE_H
#define ENGINE_SOURCE_MODERNASSETS_DB2_DB2FILE_H

#include <cstdint>
#include <unordered_map>
#include <vector>

// One .db2 table read through the client's own archive set and decoded into a flat array of
// fixed-size records, indexed by id. See Db2Decode.h for the decoded record layout.
//
// Ported from WarcraftXL wxl-db2 decode/DB2File.{hpp,cpp} (GPL-3.0-or-later, Copyright (C) 2026
// WarcraftXL). A row type mirrors the decoded record: first member is the id, then the fields in
// file order at their native widths, then the relationship id when the table carries one.
namespace modern::db2
{
    class DB2File
    {
    public:
        explicit DB2File(uint32_t rowSize);
        virtual ~DB2File() = default;

        // Loads "DBFilesClient\<fileName>" through the client's own storage seam. Returns false (and
        // logs) on any error; never throws or crashes.
        bool Load(const char* fileName);
        // Decodes a .db2 already in memory (used by Load).
        bool LoadBytes(const uint8_t* data, uint32_t size, const char* nameForLog);
        void Unload();
        bool IsLoaded() const { return m_loaded; }

        int32_t  MinId() const { return m_minId; }
        int32_t  MaxId() const { return m_maxId; }
        uint32_t RowCount() const { return m_numRows; }

        const void* RowById(int32_t id) const;
        const void* RowByIndex(uint32_t index) const;
        int32_t     IdAt(uint32_t index) const;
        // Resolves a string-table offset to a C string ("" if out of range).
        const char* Str(uint32_t offset) const;

    protected:
        // A table may override this to list the FIELD indices (file order, 0-based) that hold string
        // offsets. Only needed for WDC2/WDC3 tables; a single-section WDC1 table stores absolute
        // offsets and needs nothing here.
        virtual const uint32_t* StringColumns(uint32_t* count) const { *count = 0; return nullptr; }

        uint32_t m_rowSize;
        bool     m_loaded = false;
        uint32_t m_numRows = 0;
        int32_t  m_minId = 0x7FFFFFFF;
        int32_t  m_maxId = -1;

        std::vector<char>    m_records;
        std::vector<int32_t> m_ids;
        std::vector<char>    m_strings;
        std::unordered_map<int32_t, uint32_t> m_idIndex;

    private:
        void BuildIndex();
    };

    template <typename Row>
    class DB2Table : public DB2File
    {
    public:
        DB2Table() : DB2File(sizeof(Row)) {}

        const Row* Find(int32_t id) const { return static_cast<const Row*>(RowById(id)); }
        const Row* At(uint32_t index) const { return static_cast<const Row*>(RowByIndex(index)); }
    };
}

#endif
