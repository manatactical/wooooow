#include <cstdint>
#include <cstring>
#include "Engine/Source/Gx/CGxShaderPermute.h"
#include "Engine/Source/Gx/CGxDevice/CGxDevice.h"
#include "Engine/Source/Gx/CGxDevice/CGxShader.h"
#include "Engine/Source/Storm/H/storm.h"
#include "ThirdParty/zlib/zlib.h"

// NOT IN BINARY: adopt an already-formed device shader. A shader compiled at run time (AntiAlias) has no
// .bls container, so there is nothing for Load() to decode; the caller hands over the bytecode and the
// permute is valid from the start. m_loaded stays set so Valid() does not try to create it again.
void CGxShaderPermute::SetInterface(uint32_t bytecodeSize, uint32_t inputMask, uint32_t vertexConstantSize) {
    // Describes the permutation only. m_apiSpecific stays whatever the backend put there (a device shader);
    // this used to be handed the raw bytecode, which the bind paths would then have handed to D3D as if it
    // were a shader object.
    this->unk_000C = inputMask;
    this->unk_0010 = bytecodeSize;
    this->unk_0014 = static_cast<uint16_t>(vertexConstantSize);

    for (uint32_t i = 0; i < 4; i++) {
        this->unk_0016[i] = 0;
    }

    this->m_valid = 0;
    this->m_loaded = 0;
}

// OFFSET: 0x00848B80/0x1405766C0
int32_t CGxShaderPermute::Valid() {    if (!this->m_loaded) {
        g_theGxDevicePtr->IShaderCreate(this);
    }

    return this->m_valid;
}

// OFFSET: 0x008489F0/0x1405764F0
//
// Decodes permutation m_index out of the shader's zlib block table. Everything here is bounded against the
// header the loader validated: a truncated or hostile .bls must fail rather than read or write past an
// allocation, because this runs straight from the game's data files.
uint8_t* CGxShaderPermute::Load() {
    CGxShader* shader = this->m_shader;

    if (!shader || !shader->m_permuteOffsets || !shader->m_blockOffsets || !shader->m_blockData) {
        return nullptr;
    }

    // m_index comes from the caller; the +1 read needs the table to actually have that many entries,
    // or this walks off the end of the offset array.
    if (this->m_index + 1 >= shader->m_permuteCount) {
        return nullptr;
    }

    uint32_t start = shader->m_permuteOffsets[this->m_index];
    uint32_t next = shader->m_permuteOffsets[this->m_index + 1];

    // Offsets must be monotonic or size underflows to ~4GB.
    if (next < start) {
        return nullptr;
    }

    uint32_t size = next - start;

    // The permutation must hold the 32-byte header this function reads below (and that IShaderCreate and
    // IUpdateInputLayout read from the returned buffer).
    if (size < 32) {
        return nullptr;
    }

    // A permutation cannot be larger than the decompressed block table could hold. Without this a hostile
    // .bls can ask for a ~4GB allocation (and an equally large memset) that no block data backs.
    if (static_cast<uint64_t>(size) > static_cast<uint64_t>(shader->m_blockCount) * 0x4000) {
        return nullptr;
    }

    uint32_t blockCount = shader->m_blockCount;

    // A permutation header is 32 bytes and Load() reads it unconditionally below.
    uint8_t* code = static_cast<uint8_t*>(SMemAlloc(size, __FILE__, __LINE__, 0));

    if (!code) {
        return nullptr;
    }

    memset(code, 0, size);

    uint32_t block = start >> 14;

    if (block >= blockCount) {
        SMemFree(code, __FILE__, __LINE__, 0);
        return nullptr;
    }

    uint32_t blockOffset = start - (block << 14);
    uint8_t* dest = code;

    if (blockOffset) {
        uint32_t compressedOffset = shader->m_blockOffsets[block];
        uint32_t compressedSize = shader->m_blockOffsets[block + 1] - compressedOffset;
        uLongf blockSize = blockOffset + size >= 0x4000 ? 0x4000 : blockOffset + size;
        const uint8_t* source = shader->m_blockData + compressedOffset;
        uint32_t copied;

        if (compressedSize == 0x4000) {
            if (blockSize < blockOffset) {
                SMemFree(code, __FILE__, __LINE__, 0);
                return nullptr;
            }

            copied = blockSize - blockOffset;
            memcpy(code, source + blockOffset, copied);
        } else {
            uint8_t buffer[0x4000];
            uLongf outSize = sizeof(buffer);

            if (uncompress(buffer, &outSize, source, compressedSize) != Z_OK || outSize < blockOffset) {
                SMemFree(code, __FILE__, __LINE__, 0);
                return nullptr;
            }

            blockSize = outSize;
            copied = blockSize - blockOffset;
            memcpy(code, buffer + blockOffset, copied);
        }

        dest = code + copied;
        size += blockOffset - blockSize;
        block++;
    }

    while (size) {
        if (block >= blockCount) {
            SMemFree(code, __FILE__, __LINE__, 0);
            return nullptr;
        }

        uint32_t compressedOffset = shader->m_blockOffsets[block];
        uint32_t compressedSize = shader->m_blockOffsets[block + 1] - compressedOffset;
        uLongf blockSize = size >= 0x4000 ? 0x4000 : size;
        const uint8_t* source = shader->m_blockData + compressedOffset;

        if (compressedSize == 0x4000) {
            // Uncompressed block: copy only what is left, not a full block, or a short final block
            // writes past the allocation.
            memcpy(dest, source, blockSize);
        } else {
            uLongf outSize = blockSize;

            if (uncompress(dest, &outSize, source, compressedSize) != Z_OK || !outSize) {
                SMemFree(code, __FILE__, __LINE__, 0);
                return nullptr;
            }

            blockSize = outSize;
        }

        dest += blockSize;
        size -= blockSize;
        block++;
    }

    // Reads the permutation header (mask, sizes, DXBC length). Guarded above by the size check.
    const uint32_t* header = reinterpret_cast<const uint32_t*>(code);
    this->unk_000C = header[0];
    // header[1] is the permutation byte length as recorded in the file; clamp it to what was actually
    // decoded, so callers may use it as a bound on the buffer without trusting the file.
    this->unk_0010 = header[1] <= (next - start) ? header[1] : (next - start);
    this->unk_0014 = reinterpret_cast<const uint16_t*>(code)[4];
    this->unk_0016[0] = header[3];
    this->unk_0016[1] = header[4];
    this->unk_0016[2] = header[5];
    this->unk_0016[3] = header[6];

    return code;
}
