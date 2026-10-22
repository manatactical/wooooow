#ifndef ENGINE_SOURCE_GX_CGXDEVICE_CGXSHADER_H
#define ENGINE_SOURCE_GX_CGXDEVICE_CGXSHADER_H

#include <cstddef>
#include <cstdint>
// NOT IN BINARY: cata's CGxShader.h does not include this, but several 3.3.5a translation units relied on it
// arriving through this header for SFile/SFileReadFile. Included here rather than in each caller.
#include "Engine/Source/BlizzardCore/SFile2-Core.h"
#include "Engine/Source/Gx/CGxShaderPermute.h"
#include "Engine/Source/Storm/H/stpl.h"

struct CGxShaderHeader {
    /* 0000 */ uint32_t m_magic;
    /* 0004 */ uint32_t m_version;
    /* 0008 */ uint32_t m_permuteOffsetsOffset;
    /* 000C */ uint32_t m_permuteCount;
    /* 0010 */ uint32_t m_blockOffsetsOffset;
    /* 0014 */ uint32_t m_blockCount;
    /* 0018 */ uint32_t m_blockDataOffset;
};

class CGxShader : public TSHashObject<CGxShader, HASHKEY_STRI> {
    public:
    /* 0018 */ int32_t m_target;
    /* 001C */ uint32_t m_refCount;
    /* 0020 */ uint8_t* m_data;
    /* 0024 */ const uint32_t* m_permuteOffsets;
    /* 0028 */ const uint32_t* m_blockOffsets;
    /* 002C */ const uint8_t* m_blockData;
    /* 0030 */ TSGrowableArray<CGxShaderPermute> m_permutes;
    // NOT IN BINARY: the block and permutation counts, kept so CGxShaderPermute::Load can bound its
    // block-table reads against the loaded file instead of trusting the offsets.
    /* 0044 */ uint32_t m_blockCount;
    /* 0048 */ uint32_t m_permuteCount;

    CGxShader();
    ~CGxShader();
    bool Load(uint8_t* data, uint32_t size);
    // Validates the container without taking ownership of data. Load() calls this and only adopts the
    // buffer once it succeeds, so a rejected file stays the caller's to free.
    bool Adopt(uint8_t* data, uint32_t size);
};

#endif
