#include "Engine/Source/ModernAssets/Dbc/AnimationData.h"

#include "Engine/Source/ModernAssets/Db2/Db2Table.h"
#include "Engine/Source/ModernAssets/ModernAssetsFile.h"
#include "Engine/Source/ModernAssets/ModernAssetsLog.h"

#include <cstdio>
#include <cstring>

#include "Engine/Source/Os/OsFile.h"

// AnimationData.db2 -> the client's AnimationData, in the 8-column WDBC shape its loader reads.
//
// Column layout, read off the real file and cross-checked against ids whose meaning the client's own ANIM_
// enum pins down: f0 is the fallback (187 JUMP_LAND_RUN -> 5 RUN; 43/44/45 swim left/right/back -> 42
// SWIM; 97/98 -> 96), f1 the behaviour tier (3 for the modern fly set), f2 the behaviour id, f3 the flags.
// The physical shapes are 1/1/2/2.
//
// The modern table is not a supplement: it holds 1866 rows covering ids 0..1865, so it is the authority
// outright rather than a layer merged over the older one.
//
// If anything about the table does not line up, this returns 0 and the client keeps its stock table. A
// wrong table here would silently mis-animate every model in the game, so failing back is the only safe
// default.

namespace modern::db2::animation
{
    namespace
    {
        constexpr uint32_t kClientColumns = 8;
        constexpr uint32_t kClientRowSize = 32;

        Status g_status;

        uint32_t Fail(const char* why)
        {
            g_status.failed = true;
            g_status.loaded = false;

            std::snprintf(g_status.error, sizeof(g_status.error), "%s", why ? why : "unknown");
            MODERN_ASSETS_LOG_WARN("animationdata: %s (keeping the stock table)", g_status.error);
            return 0;
        }

        void Append32(std::vector<uint8_t>& out, uint32_t value)
        {
            const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&value);
            out.insert(out.end(), bytes, bytes + sizeof(value));
        }

        /**
         * @brief Writes the exact bytes the client received, so they can be decoded offline.
         *
         * The archive set can serve a different revision of a table than the loose file on disk, and a
         * pallet-layout mismatch is impossible to diagnose from a byte count alone. This dump is what makes
         * the failure reproducible outside the game.
         */
        void DumpForOfflineDecode(const std::vector<uint8_t>& bytes)
        {
            OsCreateDirectory("Logs", 0);

            FILE* file = std::fopen("Logs\\animationdata-client.db2", "wb");

            if (!file)
            {
                MODERN_ASSETS_LOG_WARN("animationdata: could not write Logs\\animationdata-client.db2");
                return;
            }

            std::fwrite(bytes.data(), 1, bytes.size(), file);
            std::fclose(file);
        }
    }

    uint32_t BuildWdbc(std::vector<uint8_t>& out)
    {
        g_status = Status{};

        std::vector<uint8_t> file;

        if (!modern::assets::ReadWholeFile("DBFilesClient\\AnimationData.db2", file) || file.empty())
            return Fail("DBFilesClient\\AnimationData.db2 is not in the archive set");

        DumpForOfflineDecode(file);

        const uint32_t magic = file.size() >= 4 ? *reinterpret_cast<const uint32_t*>(file.data()) : 0;
        const uint32_t version = file.size() >= 8 ? *reinterpret_cast<const uint32_t*>(file.data() + 4) : 0;

        if (magic != 0x35434457) // 'WDC5'
        {
            char why[120];
            std::snprintf(why, sizeof(why), "not a WDC5 table (magic %08x)", magic);
            return Fail(why);
        }

        // Physical shapes are 1/1/1/2: three scalars and one two-element array. Only the LAST column is a
        // pallet array (value3=2); the three before it are plain scalars. This was measured from the file's
        // own column metadata, not assumed -- reading them as 1/1/2/2 makes the reader consume 12 bits too
        // many per row and run out inside the pallet index.
        const modern::db2::Field fields[] = {
            { "fallback" }, { "behaviourTier" }, { "behaviourId" }, { "flags", 2 },
        };

        modern::db2::Definition def{ "AnimationData", "DBFilesClient\\AnimationData.db2", 0,
                                     std::vector<modern::db2::Field>(std::begin(fields), std::end(fields)) };

        modern::db2::Table table;
        std::string error;

        if (!table.Load(def, &error))
        {
            char why[240];
            std::snprintf(why, sizeof(why), "decode failed (%zu bytes, schema v%u): %s",
                          file.size(), version, error.empty() ? "(no reason)" : error.c_str());
            return Fail(why);
        }

        const auto& rows = table.Rows();

        if (rows.empty())
            return Fail("table decoded but holds no rows");

        out.clear();
        // WDBC: magic, recordCount, fieldCount, recordSize, stringSize. No string block -- nothing reads
        // this table's name column and it stores no names.
        out.reserve(20 + rows.size() * kClientRowSize);

        Append32(out, 0x43424457); // 'WDBC'
        Append32(out, static_cast<uint32_t>(rows.size()));
        Append32(out, kClientColumns);
        Append32(out, kClientRowSize);
        Append32(out, 0);

        uint32_t written = 0;

        for (const modern::db2::wdc5::Row& row : rows)
        {
            // id, nameOffset, weaponFlags, bodyFlags, flags, fallback, behaviourId, behaviourTier -- the
            // order AnimationDataRec::Read consumes.
            Append32(out, row.id);
            Append32(out, 0);
            Append32(out, 0);
            Append32(out, 0);
            Append32(out, table.Value(row, 3));
            Append32(out, table.Value(row, 0));
            Append32(out, table.Value(row, 2));
            Append32(out, table.Value(row, 1));

            ++written;
        }

        g_status.rows = written;
        g_status.loaded = true;

        MODERN_ASSETS_LOG_INFO("animationdata: built %u row(s) from AnimationData.db2 (%zu bytes, schema v%u)",
                               written, file.size(), version);
        return written;
    }

    const Status& LastStatus()
    {
        return g_status;
    }
}
