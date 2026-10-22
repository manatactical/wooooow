#ifndef ENGINE_SOURCE_GX_CGXFRAMEBUFFER_H
#define ENGINE_SOURCE_GX_CGXFRAMEBUFFER_H

#include <cstddef>
#include <cstdint>
#include "Engine/Source/Gx/Types.h"

class CGxTex;

class CGxFrameBuffer {
    public:
    struct Target {
        /* 0000 */ CGxTex* m_texture;
        /* 0004 */ uint32_t m_plane;
    };

    /* 0000 */ uint32_t m_version;
    /* 0004 */ Target m_targets [GxBuffers_Last];
    /* 002C */ int32_t m_count;
    /* 0030 */ int32_t m_bound;

    static uint32_t s_nextVersion;

    CGxFrameBuffer();
    int32_t Attach(EGxBuffer buffer, CGxTex* texture, uint32_t plane);
    int32_t Finalize();
};

#endif
