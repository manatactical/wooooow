#ifndef ENGINE_SOURCE_MODERNWATER_FULLSCREEN_TRIANGLE_H
#define ENGINE_SOURCE_MODERNWATER_FULLSCREEN_TRIANGLE_H

#include <d3d9.h>

// One oversized triangle covering the viewport, for every fullscreen water pass: the FFT, the ripples, the
// depth copy and the shading pass all draw exactly this.
//
// A triangle rather than a quad because the diagonal of a quad splits the target into two shaded work sets and
// every pixel costs a varying interpolation on the seam; the oversized triangle has no seam and one primitive.
// The vertex shader only passes the position through, so the coordinates are already in clip space.

inline constexpr float kFullscreenTriangleLow = -1.5f;
inline constexpr float kFullscreenTriangleHigh = 4.5f;
inline constexpr float kFullscreenTriangle[3][4] = {
    {kFullscreenTriangleLow, kFullscreenTriangleLow, 0.0f, 1.0f},
    {kFullscreenTriangleLow, kFullscreenTriangleHigh, 0.0f, 1.0f},
    {kFullscreenTriangleHigh, kFullscreenTriangleLow, 0.0f, 1.0f},
};

inline HRESULT DrawFullscreenTriangle(IDirect3DDevice9* dev) {
    return dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, kFullscreenTriangle, sizeof(kFullscreenTriangle[0]));
}

#endif
