#ifndef ENGINE_SOURCE_MODERNASSETS_DBC_DBCMERGE_H
#define ENGINE_SOURCE_MODERNASSETS_DBC_DBCMERGE_H

#include <cstdint>
#include <string>
#include <vector>

// Appends add-on DBC rows to a base table without replacing the file on disk.
//
// A pack ships small files named like `CreatureDisplayInfo.dbc1-mana` next to the original
// `CreatureDisplayInfo.dbc`. The game loads the base table, then this layers the add-on rows on top,
// so a patch can add rows to a table without shipping a modified copy of it. That is how a retail
// content pack delivers model and display data the stock client has no rows for.
//
// Ported from WarcraftXL wxl-extended-dbc (GPL-3.0-or-later, Copyright (C) 2026 WarcraftXL), which
// implements the same merge as a storage transform inside the original binary. Here the merge produces
// the file bytes directly, because this client can be handed an SFileImpl that reads from memory.
namespace modern::dbc
{
    /// One add-on file found for a base table, already ordered by tier then project.
    struct Continuation
    {
        std::string path;
        uint8_t     tier = 0;
    };

    /**
     * @brief Finds the add-on files that belong to one base table, in the order they must be applied.
     *
     * Higher tiers load last and win. A folder patch is enumerated through the client's own archive
     * search, so no manifest is needed; the listing falls back to trying tiers 1..9 named by whatever
     * project slugs the base table's directory already shows.
     *
     * @param basePath  the base table's path, e.g. "DBFilesClient\\CreatureDisplayInfo.dbc".
     */
    std::vector<Continuation> DiscoverContinuations(const char* basePath);

    /**
     * @brief Merges add-on file bytes into a base table, without touching the archive set.
     *
     * Split out from MergeInto so a test can drive the merge with bytes it built itself.
     *
     * @param tableName      base name without extension, e.g. "Spell".
     * @param base           the base table's bytes.
     * @param baseSize       their size.
     * @param continuations  add-on file bytes, in the order they must be applied.
     * @return true when at least one add-on row was applied.
     */
    bool MergeBytes(const char* tableName, const uint8_t* base, uint32_t baseSize,
                    const std::vector<std::vector<uint8_t>>& continuations, std::vector<uint8_t>& out,
                    uint32_t* appended, uint32_t* replaced);

    /**
     * @brief Merges every add-on file for this table into the base bytes.
     *
     * @param tableName  base name without extension, e.g. "CreatureDisplayInfo" (used to pick the
     *                   string columns).
     * @param base       the base table's bytes.
     * @param baseSize   their size.
     * @param out        receives the merged file. Unchanged (and false returned) when there is nothing
     *                   to merge.
     * @return true when at least one add-on row was applied.
     */
    bool MergeInto(const char* tableName, const uint8_t* base, uint32_t baseSize,
                   std::vector<uint8_t>& out, uint32_t* appended, uint32_t* replaced);
}

#endif
