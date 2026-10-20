#ifndef ENGINE_SOURCE_MODERNASSETS_DBC_ANIMATIONDATA_H
#define ENGINE_SOURCE_MODERNASSETS_DBC_ANIMATIONDATA_H

#include <cstdint>
#include <vector>

// AnimationData for this client, built from the modern AnimationData.db2.
//
// The stock table is a 3.3.5a artefact and stops at id 505. Modern content -- the retail mounts in this
// pack among it -- uses animation ids far past that, so GetRecord()'s minId/maxId range check rejects
// every one of them: no behaviour id, no fallback chain, no animation. AnimationData.db2 is the same
// table at a later revision and covers every stock id as well, so it is the authority here.
namespace modern::db2::animation
{
    struct Status
    {
        bool     loaded = false;
        bool     failed = false;
        uint32_t rows = 0;
        char     error[200] = {};
    };

    /**
     * @brief Builds a WDBC image of AnimationData from AnimationData.db2.
     *
     * @param out receives the WDBC image on success.
     * @return how many rows were written; 0 means "use the stock table instead", with the reason in
     *         LastStatus().
     */
    uint32_t BuildWdbc(std::vector<uint8_t>& out);

    /// Outcome of the last BuildWdbc() call, for the log.
    const Status& LastStatus();
}

#endif
