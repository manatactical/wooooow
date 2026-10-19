#include "Engine/Source/ModernAssets/Db2/Db2File.h"

#include "Engine/Source/ModernAssets/Db2/Db2Decode.h"
#include "Engine/Source/ModernAssets/ModernAssetsFile.h"
#include "Engine/Source/ModernAssets/ModernAssetsLog.h"

#include <cstdio>
#include <cstring>

namespace modern::db2
{
    DB2File::DB2File(uint32_t rowSize) : m_rowSize(rowSize) {}

    bool DB2File::Load(const char* fileName)
    {
        if (m_loaded) return true;

        char path[300];
        std::snprintf(path, sizeof(path), "DBFilesClient\\%s", fileName);

        std::vector<uint8_t> bytes;
        if (!modern::assets::ReadWholeFile(path, bytes))
        {
            MODERN_ASSETS_LOG_INFO("db2: %s not found in the mounted archives", path);
            return false;
        }
        return LoadBytes(bytes.data(), static_cast<uint32_t>(bytes.size()), fileName);
    }

    bool DB2File::LoadBytes(const uint8_t* data, uint32_t size, const char* nameForLog)
    {
        if (m_loaded) return true;
        if (!data || size < 4)
        {
            MODERN_ASSETS_LOG_WARN("db2: empty/short data for %s", nameForLog ? nameForLog : "?");
            return false;
        }

        uint32_t strColCount = 0;
        const uint32_t* strCols = StringColumns(&strColCount);

        DB2Decoded dec;
        if (!DecodeDB2(data, size, dec, strCols, strColCount))
        {
            MODERN_ASSETS_LOG_WARN("db2: %s is not a supported DB2 (WDC1/2/3) or is malformed",
                                   nameForLog ? nameForLog : "?");
            return false;
        }

        if (dec.rowSize != m_rowSize)
        {
            MODERN_ASSETS_LOG_WARN("db2: %s decoded record size %u != definition %u%s",
                                   nameForLog ? nameForLog : "?", dec.rowSize, m_rowSize,
                                   dec.hasRelationship ? " (a trailing relationship column is appended)" : "");
            return false;
        }

        // RowByIndex/RowById do pointer arithmetic from the flat buffer; if the decoder's own bookkeeping
        // were ever inconsistent that arithmetic would run off the end, so it is checked once here.
        if (dec.records.size() != dec.ids.size() * static_cast<size_t>(m_rowSize))
        {
            MODERN_ASSETS_LOG_WARN("db2: %s decoded %zu byte(s) for %zu row(s) of %u -- inconsistent",
                                   nameForLog ? nameForLog : "?", dec.records.size(),
                                   dec.ids.size(), m_rowSize);
            return false;
        }

        m_numRows = static_cast<uint32_t>(dec.ids.size());
        m_records = std::move(dec.records);
        m_ids     = std::move(dec.ids);
        m_strings = std::move(dec.strings);

        BuildIndex();
        m_loaded = true;
        MODERN_ASSETS_LOG_INFO("db2: loaded %s (%u rows, id %d..%d)", nameForLog ? nameForLog : "?",
                               m_numRows, m_minId, m_maxId);
        return true;
    }

    void DB2File::BuildIndex()
    {
        m_idIndex.clear();
        m_idIndex.reserve(m_ids.size());
        m_minId = 0x7FFFFFFF;
        m_maxId = -1;
        for (uint32_t i = 0; i < m_ids.size(); ++i)
        {
            const int32_t id = m_ids[i];
            m_idIndex[id] = i;
            if (id < m_minId) m_minId = id;
            if (id > m_maxId) m_maxId = id;
        }
        if (m_ids.empty()) { m_minId = 0; m_maxId = -1; }
    }

    void DB2File::Unload()
    {
        m_loaded = false;
        m_numRows = 0;
        m_minId = 0x7FFFFFFF;
        m_maxId = -1;
        m_records.clear();
        m_ids.clear();
        m_strings.clear();
        m_idIndex.clear();
    }

    const void* DB2File::RowById(int32_t id) const
    {
        auto it = m_idIndex.find(id);
        if (it == m_idIndex.end() || it->second >= m_numRows) return nullptr;
        return m_records.data() + static_cast<size_t>(it->second) * m_rowSize;
    }

    const void* DB2File::RowByIndex(uint32_t index) const
    {
        if (index >= m_numRows) return nullptr;
        return m_records.data() + static_cast<size_t>(index) * m_rowSize;
    }

    int32_t DB2File::IdAt(uint32_t index) const
    {
        return (index < m_ids.size()) ? m_ids[index] : -1;
    }

    const char* DB2File::Str(uint32_t offset) const
    {
        if (offset >= m_strings.size()) return "";
        // The string block is copied verbatim out of the file, so its last byte is whatever the table
        // put there. An offset with no terminator before the end would let a caller's strlen run off the
        // buffer, so only offsets that do reach a terminator are reported.
        const char* text = m_strings.data() + offset;
        return std::memchr(text, '\0', m_strings.size() - offset) ? text : "";
    }
}
