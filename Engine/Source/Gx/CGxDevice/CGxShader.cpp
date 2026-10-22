#include <cstdint>
#include "Engine/Source/Gx/CGxDevice/CGxShader.h"
#include "Engine/Source/Storm/H/storm.h"

// The .bls container, ported from woooowcata/Engine/Source/Gx/CGxDevice/CGxShader.cpp.
//
// Two versions ship in this client's data and both use the same header:
//
//   0x10003  the "Classic" container: one fully self-contained shader, used by
//            Shaders/Classic/{Vertex,Pixel} and by the effect sets (ShaderEffect) that the
//            game's own shader compiler produced.
//   0x10004  the permuted container: several permutations over shared zlib blocks.
//
// Only version 0x10004 appears in the stock client. 0x10003 is accepted as well because it costs one
// comparison -- CGxShaderPermute::Load then simply copies byte range 0 .. oneblock out of the single
// block, which is what the generic code below already does when m_blockCount == 1.
//
// The payload of a 0x10004 shader is a block table, not a flat blob: CGxShaderPermute::Load()
// decompresses the blocks and slices the permutation out of them, then reads the per-permutation
// header (%u8 mask at +0, ... vertex-constant size at +8, DXBC size at +28, DXBC at +32). That is why
// this loader hands over the raw file and leaves the decoding to the permute.
static const uint32_t s_supportedVersions[] = { 0x10003, 0x10004 };

// INLINED
CGxShader::CGxShader() : m_target(0), m_refCount(0), m_data(nullptr), m_permuteOffsets(nullptr), m_blockOffsets(nullptr), m_blockData(nullptr), m_blockCount(0), m_permuteCount(0) {
}

// OFFSET: 0x00848E30/0x140576A30
CGxShader::~CGxShader() {
    SMemFree(this->m_data, __FILE__, __LINE__, 0);
}

// OFFSET: 0x00848E70/0x140576A90
bool CGxShader::Load(uint8_t* data, uint32_t size) {
    // Ownership only transfers on success. On every rejection the caller (IShaderLoad) frees the buffer
    // itself, so m_data must not alias it or ~CGxShader would free it a second time.
    this->m_data = nullptr;

    if (!this->Adopt(data, size)) {
        return false;
    }

    this->m_data = data;
    return true;
}

bool CGxShader::Adopt(uint8_t* data, uint32_t size) {

    if (size < sizeof(CGxShaderHeader)) {
        return false;
    }

    const CGxShaderHeader* header = reinterpret_cast<const CGxShaderHeader*>(data);

    if (header->m_magic != 'GXSH') {
        return false;
    }

    bool supported = false;

    for (uint32_t i = 0; i < sizeof(s_supportedVersions) / sizeof(s_supportedVersions[0]); i++) {
        if (header->m_version == s_supportedVersions[i]) {
            supported = true;
            break;
        }
    }

    if (!supported) {
        return false;
    }

    if (header->m_permuteOffsetsOffset > size || header->m_blockOffsetsOffset > size || header->m_blockDataOffset > size) {
        return false;
    }

    if (!header->m_permuteCount || !header->m_blockCount) {
        return false;
    }

    // The offsets are in entries, the counts bound how many the region may hold. Bound the reads so a
    // truncated or hostile file cannot walk off the buffer.
    // 64-bit arithmetic: a hostile count near UINT32_MAX would otherwise wrap the sum and pass the check.
    if (static_cast<uint64_t>(header->m_permuteOffsetsOffset) + (static_cast<uint64_t>(header->m_permuteCount) + 1) * sizeof(uint32_t) > size) {
        return false;
    }

    if (static_cast<uint64_t>(header->m_blockOffsetsOffset) + (static_cast<uint64_t>(header->m_blockCount) + 1) * sizeof(uint32_t) > size) {
        return false;
    }

    if (header->m_blockDataOffset > size) {
        return false;
    }

    this->m_permuteOffsets = reinterpret_cast<const uint32_t*>(data + header->m_permuteOffsetsOffset);
    this->m_blockOffsets = reinterpret_cast<const uint32_t*>(data + header->m_blockOffsetsOffset);
    this->m_blockData = data + header->m_blockDataOffset;
    this->m_blockCount = header->m_blockCount;
    this->m_permuteCount = header->m_permuteCount;

    // The last block offset must leave room for the block data inside the file.
    if (this->m_blockOffsets[header->m_blockCount] > size - header->m_blockDataOffset) {
        return false;
    }

    // Block offsets index into m_blockData, and CGxShaderPermute::Load derives each block's compressed size as
    // the difference between consecutive entries. A non-monotonic table underflows that subtraction to a huge
    // value which then reaches uncompress()/memcpy, so the table must be ordered and in bounds.
    for (uint32_t i = 0; i < header->m_blockCount; i++) {
        if (this->m_blockOffsets[i] > this->m_blockOffsets[i + 1]) {
            return false;
        }
    }

    // Same for the permutation offsets, which Load() also differences.
    for (uint32_t i = 0; i < header->m_permuteCount; i++) {
        if (this->m_permuteOffsets[i] > this->m_permuteOffsets[i + 1]) {
            return false;
        }
    }

    this->m_permutes.SetCount(header->m_permuteCount);

    for (uint32_t i = 0; i < header->m_permuteCount; i++) {
        CGxShaderPermute& permute = this->m_permutes[i];
        permute.m_index = i;
        permute.m_apiSpecific = NULL;
        permute.m_shader = this;
        permute.m_valid = 0;
        permute.m_loaded = 0;
        permute.m_target = static_cast<uint8_t>(this->m_target);
    }

    return true;
}
