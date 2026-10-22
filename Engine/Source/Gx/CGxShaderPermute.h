#ifndef ENGINE_SOURCE_GX_CGXSHADERPERMUTE_H
#define ENGINE_SOURCE_GX_CGXSHADERPERMUTE_H

#include <cstddef>
#include <cstdint>
class CGxShader;

#pragma pack(push, 2)

class CGxShaderPermute {
    public:
    /* 0000 */ void* m_apiSpecific;
    /* 0004 */ CGxShader* m_shader;
    /* 0008 */ uint32_t m_index;
    /* 000C */ uint32_t unk_000C;
    /* 0010 */ uint32_t unk_0010;
    /* 0014 */ uint16_t unk_0014;
    /* 0016 */ uint32_t unk_0016 [4];
    /* 0026 */ uint8_t m_target;
    /* 0028 */ uint32_t m_valid : 1;
    /* 0028 */ uint32_t m_loaded : 1;
    /* 0028 */ uint32_t m_flagsUnused : 30;

    int32_t Valid();
    uint8_t* Load();
    // NOT IN BINARY: adopt an already-formed device shader (bytecode compiled at run time, which has no
    // .bls container to decode). m_apiSpecific receives the bytecode and the permute becomes valid without
    // the backend creating anything; unk_000C is the input-signature mask and unk_0014 the vertex-constant
    // size, both read by the D3d11 backend when it builds the input layout and constant buffer.
    void SetInterface(uint32_t bytecodeSize, uint32_t inputMask, uint32_t vertexConstantSize);
};

#pragma pack(pop)

#endif
