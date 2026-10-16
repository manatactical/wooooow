#ifndef ENGINE_SOURCE_GX_CGXTEXFLAGS_H
#define ENGINE_SOURCE_GX_CGXTEXFLAGS_H

#include <cstddef>
#include <cstdint>
#include "Engine/Source/Gx/Types.h"

class CGxTexFlags {
    public:
    uint32_t m_filter : 3;
    uint32_t m_wrapU : 1;
    uint32_t m_wrapV : 1;
    uint32_t m_forceMipTracking : 1;
    uint32_t m_generateMipMaps : 1;
    uint32_t m_renderTarget : 1;
    uint32_t m_bit8 : 1;
    uint32_t m_maxAnisotropy : 5;
    uint32_t m_bit14 : 1;
    uint32_t m_bit15 : 1;
    // Not in the original binary. Ported from 15595 (Cataclysm), which needs them for the new water system:
    // m_wrapW wraps the noise volume texture, m_bit20 marks the procedural water-depth textures. Carved out of
    // m_unk16 (15 bits, never read or written anywhere in this tree) so no existing field moves - 15595 widens
    // wrapU/wrapV to 2 bits instead, which would shift every field after them and change the meaning of every
    // stored flag. s_wrapModes stays 2 entries here; add MIRROR to it if a 3rd mode is ever needed.
    uint32_t m_wrapW : 1;
    uint32_t m_bit20 : 1;
    uint32_t m_unk16 : 13;
    uint32_t m_bit31 : 1;

    bool operator!=(const CGxTexFlags& flags) const;
    bool operator==(const CGxTexFlags& flags) const;
    CGxTexFlags(EGxTexFilter filter = GxTex_Linear, uint32_t wrapU = 0, uint32_t wrapV = 0, uint32_t forceMipTracking = 0, uint32_t generateMipMaps = 0, uint32_t renderTarget = 0, uint32_t maxAnisotropy = 1, uint32_t bit8 = 0, uint32_t bit14 = 0, uint32_t bit15 = 0);
};

#endif
