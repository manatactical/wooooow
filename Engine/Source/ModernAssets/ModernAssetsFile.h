#ifndef ENGINE_SOURCE_MODERNASSETS_MODERNASSETSFILE_H
#define ENGINE_SOURCE_MODERNASSETS_MODERNASSETSFILE_H

#include <cstdint>
#include <vector>

// The one way these readers get bytes: through the client's own already-mounted archive set.
// Nothing is written to disk and no loose-file override is consulted -- whatever archive the client
// mounted is the authority, exactly as for every other game asset.
namespace modern::assets
{
    /**
     * @brief Reads a whole file out of the client's archives.
     * @param fileName  path as the client spells it (e.g. "DBFilesClient\\Light.db2").
     * @param bytes     receives the contents; cleared on failure.
     * @return true only when the whole file was read.
     */
    bool ReadWholeFile(const char* fileName, std::vector<uint8_t>& bytes);
}

#endif
