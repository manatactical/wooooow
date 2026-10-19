#include "Engine/Source/ModernAssets/Dbc/DbcMerge.h"

#include "Engine/Source/ModernAssets/Dbc/DbcStringFields.h"
#include "Engine/Source/ModernAssets/ModernAssetsFile.h"
#include "Engine/Source/ModernAssets/ModernAssetsLog.h"

#include <cctype>
#include <cstring>
#include <unordered_map>

// Ported from WarcraftXL wxl-extended-dbc (GPL-3.0-or-later, Copyright (C) 2026 WarcraftXL):
// DbcMerge.cpp (ParseDbc / WriteDbc / MergeContinuation / RemapRecordStrings) and the string-field
// table in DbcStringFields.generated.hpp.
//
// What is different: their host owns a storage transform that rewrites the bytes a file read returns.
// This client's table loader streams from an SFileImpl instead, so the merge produces the file bytes
// and the caller hands the reader a memory file.

namespace modern::dbc
{
    namespace
    {
        constexpr uint32_t kWdbcMagic = 0x43424457u; // 'WDBC'

        struct WdbcHeader
        {
            uint32_t magic;
            uint32_t recordCount;
            uint32_t fieldCount;
            uint32_t recordSize;
            uint32_t stringBlockSize;
        };
        static_assert(sizeof(WdbcHeader) == 20);
        static_assert(sizeof(WdbcHeader) == 20);

        struct DbcFile
        {
            WdbcHeader header{};
            std::vector<uint8_t> records;
            std::vector<uint8_t> stringBlock;
        };

        bool HeaderValid(const WdbcHeader& h, uint64_t size)
        {
            if (h.magic != kWdbcMagic) return false;
            if (h.recordSize == 0 || h.recordSize > 0x10000) return false;
            const uint64_t records = static_cast<uint64_t>(h.recordCount) * h.recordSize;
            const uint64_t total = sizeof(WdbcHeader) + records + h.stringBlockSize;
            return records <= 0xFFFFFFFFull && total <= size;
        }

        bool ParseDbc(const uint8_t* data, uint32_t size, DbcFile& out)
        {
            if (!data || size < sizeof(WdbcHeader)) return false;

            WdbcHeader h{};
            std::memcpy(&h, data, sizeof h);
            if (!HeaderValid(h, size)) return false;

            const uint32_t recordsBytes = h.recordCount * h.recordSize;
            out.header = h;
            out.records.assign(data + sizeof(WdbcHeader), data + sizeof(WdbcHeader) + recordsBytes);
            out.stringBlock.assign(data + sizeof(WdbcHeader) + recordsBytes,
                                   data + sizeof(WdbcHeader) + recordsBytes + h.stringBlockSize);
            return true;
        }

        bool WriteDbc(const DbcFile& in, std::vector<uint8_t>& out)
        {
            WdbcHeader h = in.header;
            h.stringBlockSize = static_cast<uint32_t>(in.stringBlock.size());
            h.recordCount = in.records.empty() ? 0
                          : static_cast<uint32_t>(in.records.size() / in.header.recordSize);

            const uint64_t total = sizeof(WdbcHeader) + in.records.size() + in.stringBlock.size();
            if (total > 0xFFFFFFFFull) return false;

            out.resize(static_cast<size_t>(total));
            std::memcpy(out.data(), &h, sizeof h);
            if (!in.records.empty())
                std::memcpy(out.data() + sizeof(WdbcHeader), in.records.data(), in.records.size());
            if (!in.stringBlock.empty())
                std::memcpy(out.data() + sizeof(WdbcHeader) + in.records.size(),
                            in.stringBlock.data(), in.stringBlock.size());
            return true;
        }

        /// Byte offsets of the fields that hold a string-block offset, per table.
        ///
        /// A record's string fields are offsets into that file's own string block, so an appended row
        /// has to have them re-based into the base table's block. The table is generated from each
        /// table's own Read() and covers every table the client ships (see DbcStringFields.h) -- an
        /// incomplete list is not a harmless omission: an unre-based offset is then read as an index
        /// into the base block and prints whatever bytes happen to sit there, which is exactly how
        /// garbage reached item and spell tooltips.
        const std::vector<uint32_t>* StringFields(const char* tableName)
        {
            static const std::vector<uint32_t> kNone;
            const auto& fields = StringFieldTable();
            const auto it = fields.find(tableName ? tableName : "");
            return it == fields.end() ? &kNone : &it->second;
        }

        std::string BaseNameFromPath(const char* path)
        {
            std::string s(path ? path : "");
            const size_t slash = s.find_last_of("\\/");
            if (slash != std::string::npos) s = s.substr(slash + 1);
            const size_t dot = s.find_last_of('.');
            if (dot != std::string::npos) s = s.substr(0, dot);
            return s;
        }

        std::string DirectoryOf(const char* path)
        {
            std::string s(path ? path : "");
            const size_t slash = s.find_last_of("\\/");
            return slash == std::string::npos ? std::string() : s.substr(0, slash + 1);
        }

        /// Copies one record's string fields into the destination block, returning the new offsets.
        void RemapStrings(uint8_t* row, uint32_t recordSize, const std::vector<uint32_t>& fields,
                          const std::vector<uint8_t>& fromBlock, std::vector<uint8_t>& toBlock,
                          std::unordered_map<uint32_t, uint32_t>& cache, uint32_t emptyAt)
        {
            for (uint32_t fieldOffset : fields)
            {
                if (fieldOffset + 4 > recordSize) continue;

                uint32_t offset = 0;
                std::memcpy(&offset, row + fieldOffset, 4);
                if (offset >= fromBlock.size())
                {
                    std::memcpy(row + fieldOffset, &emptyAt, 4);
                    continue;
                }

                // The string, bounded by the source block's end rather than by a strlen that could run
                // off it if the block is not terminated the way it should be.
                const char* text = reinterpret_cast<const char*>(fromBlock.data()) + offset;
                const size_t room = fromBlock.size() - offset;
                const char* end = static_cast<const char*>(std::memchr(text, '\0', room));

                // A field pointing at a string that is not terminated before the end of the block is
                // as broken as one pointing outside it; treat it as empty rather than copy the tail.
                if (!end)
                {
                    std::memcpy(row + fieldOffset, &emptyAt, 4);
                    continue;
                }

                const size_t length = static_cast<size_t>(end - text) + 1;

                const uint32_t key = offset;
                auto found = cache.find(key);
                if (found != cache.end())
                {
                    std::memcpy(row + fieldOffset, &found->second, 4);
                    continue;
                }

                const uint32_t at = static_cast<uint32_t>(toBlock.size());
                toBlock.insert(toBlock.end(), text, text + length);
                cache.emplace(key, at);
                std::memcpy(row + fieldOffset, &at, 4);
            }
        }
    }

    std::vector<Continuation> DiscoverContinuations(const char* basePath)
    {
        std::vector<Continuation> found;
        if (!basePath) return found;

        // A folder patch is just the mounted archive set, so the client's own file search answers
        // whether a continuation exists. Each tier is probed by the exact names a pack uses, which
        // avoids needing a manifest for the folder case.
        static const char* kProjects[] = { "mana" };
        const std::string dir = DirectoryOf(basePath);
        const std::string base = BaseNameFromPath(basePath);

        for (uint8_t tier = 1; tier <= 9; ++tier)
        {
            for (const char* project : kProjects)
            {
                char candidate[300];
                std::snprintf(candidate, sizeof(candidate), "%s%s.dbc%u-%s",
                              dir.c_str(), base.c_str(), tier, project);

                std::vector<uint8_t> bytes;
                if (modern::assets::ReadWholeFile(candidate, bytes))
                    found.push_back(Continuation{ candidate, tier });
            }
        }

        return found;
    }

    bool MergeBytes(const char* tableName, const uint8_t* base, uint32_t baseSize,
                    const std::vector<std::vector<uint8_t>>& continuations, std::vector<uint8_t>& out,
                    uint32_t* appended, uint32_t* replaced)
    {
        if (appended) *appended = 0;
        if (replaced) *replaced = 0;
        if (continuations.empty()) return false;

        DbcFile merged{};
        if (!ParseDbc(base, baseSize, merged)) return false;

        const uint32_t recordSize = merged.header.recordSize;
        if (!recordSize) return false;

        // A table the generated list does not know cannot be merged safely: its string offsets would
        // stay pointed at the add-on file's own block. Say so once instead of loading garbage.
        if (!tableName || !StringFieldTable().count(tableName))
        {
            MODERN_ASSETS_LOG_WARN("dbc: '%s' has no string-field map; add-on rows skipped",
                                   tableName ? tableName : "<null>");
            return false;
        }

        const std::vector<uint32_t>* stringFields = StringFields(tableName);

        // A field whose source offset is unusable has to become an empty string. Offset 0 is only
        // empty if the base block happens to start with a NUL, so point those at a byte this code
        // owns instead of trusting the base table's first byte.
        uint32_t emptyAt = 0;
        if (merged.stringBlock.empty() || merged.stringBlock[0] != 0)
        {
            emptyAt = static_cast<uint32_t>(merged.stringBlock.size());
            merged.stringBlock.push_back(0);
        }

        uint32_t totalAppended = 0;
        uint32_t totalReplaced = 0;
        bool anyApplied = false;

        for (const std::vector<uint8_t>& bytes : continuations)
        {
            DbcFile cont{};
            if (!ParseDbc(bytes.data(), static_cast<uint32_t>(bytes.size()), cont))
            {
                MODERN_ASSETS_LOG_WARN("dbc: '%s' add-on file is not a valid WDBC table",
                                       tableName ? tableName : "<null>");
                continue;
            }

            // A continuation has to describe the same rows the base table does, or its fields would
            // land in the wrong columns.
            if (cont.header.fieldCount != merged.header.fieldCount ||
                cont.header.recordSize != merged.header.recordSize)
            {
                MODERN_ASSETS_LOG_WARN("dbc: '%s' schema mismatch (fields %u/%u, row %u/%u); skipped",
                                       tableName ? tableName : "<null>", cont.header.fieldCount,
                                       merged.header.fieldCount, cont.header.recordSize, recordSize);
                continue;
            }

            std::unordered_map<uint32_t, uint32_t> idToIndex;
            idToIndex.reserve(merged.header.recordCount + cont.header.recordCount);
            for (uint32_t i = 0; i < merged.header.recordCount; ++i)
            {
                uint32_t id = 0;
                std::memcpy(&id, merged.records.data() + static_cast<size_t>(i) * recordSize, 4);
                idToIndex[id] = i;
            }

            std::unordered_map<uint32_t, uint32_t> stringCache;

            for (uint32_t i = 0; i < cont.header.recordCount; ++i)
            {
                const uint8_t* srcRow = cont.records.data() + static_cast<size_t>(i) * recordSize;
                uint32_t id = 0;
                std::memcpy(&id, srcRow, 4);

                std::vector<uint8_t> row(srcRow, srcRow + recordSize);
                RemapStrings(row.data(), recordSize, *stringFields, cont.stringBlock,
                             merged.stringBlock, stringCache, emptyAt);

                const auto existing = idToIndex.find(id);
                if (existing != idToIndex.end())
                {
                    // A later tier overwrites an earlier row with the same id.
                    std::memcpy(merged.records.data() + static_cast<size_t>(existing->second) * recordSize,
                                row.data(), recordSize);
                    ++totalReplaced;
                }
                else
                {
                    const uint32_t index = static_cast<uint32_t>(merged.records.size() / recordSize);
                    merged.records.insert(merged.records.end(), row.begin(), row.end());
                    idToIndex.emplace(id, index);
                    ++totalAppended;
                }
                anyApplied = true;
            }

            merged.header.recordCount = static_cast<uint32_t>(merged.records.size() / recordSize);
        }

        if (!anyApplied) return false;
        if (merged.stringBlock.empty()) merged.stringBlock.push_back(0);

        if (!WriteDbc(merged, out)) return false;

        if (appended) *appended = totalAppended;
        if (replaced) *replaced = totalReplaced;
        return true;
    }

    bool MergeInto(const char* tableName, const uint8_t* base, uint32_t baseSize,
                   std::vector<uint8_t>& out, uint32_t* appended, uint32_t* replaced)
    {
        const std::vector<Continuation> found = DiscoverContinuations(
            (std::string("DBFilesClient\\") + (tableName ? tableName : "") + ".dbc").c_str());

        std::vector<std::vector<uint8_t>> continuations;
        continuations.reserve(found.size());

        for (const Continuation& entry : found)
        {
            std::vector<uint8_t> bytes;
            if (!modern::assets::ReadWholeFile(entry.path.c_str(), bytes))
            {
                MODERN_ASSETS_LOG_WARN("dbc: cannot read continuation '%s'", entry.path.c_str());
                continue;
            }
            continuations.push_back(std::move(bytes));
        }

        return MergeBytes(tableName, base, baseSize, continuations, out, appended, replaced);
    }
}
