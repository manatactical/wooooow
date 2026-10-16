#ifndef ENGINE_SOURCE_SHADEREFFECT_DEPTHCACHE_H
#define ENGINE_SOURCE_SHADEREFFECT_DEPTHCACHE_H

// Not in the original binary. Ported from 15595 (Cataclysm) so the water shaders can sample the scene depth
// buffer. Scope: the core cache only (create/resolve/validate/release). The DepthRange debug dump and its
// depth-range cvars were left out - they need 15595's per-permutation CGxShader container, which 12340's
// shader system does not have (see CGxShader.h). Re-add them with the shader-format work if ever needed.

#include <cstdint>
#include "Engine/Source/Gx/Types.h"

class CTexture;

namespace DepthCache {
    // 15595 numbering, kept verbatim: only 0 and 3 exist and DM_Texture - 1 is used to index the pixel
    // shader permutations, so the gap is load-bearing.
    enum EDepthMode {
        DM_Unknown = 0,
        DM_Texture = 3,
    };

    void AddRef();
    EDepthMode GetDepthMode();
    CTexture* GetDepthSR();
    CTexture* GetDepthTex();
    int32_t IsSupported(EDepthMode* mode, EGxTexFormat* format, EGxTexFormat* resolveFormat);
    bool IsValid();
    void Release();
    void __fastcall Reset();
    void Resolve();
    void Validate();
}

#endif