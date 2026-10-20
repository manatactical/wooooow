#ifndef ENGINE_SOURCE_ANTIALIAS_ANTIALIAS_H
#define ENGINE_SOURCE_ANTIALIAS_ANTIALIAS_H

#include <cstddef>
#include <cstdint>

class CGxShader;
class CGxTex;
struct CVar;

namespace FFX {
struct Tex;
}  // namespace FFX

// Not in the original 3.3.5a binary. A post-process anti-aliasing pass over the finished world frame, the
// engine-side counterpart of the WXL extension module wxl-vignette-grain (a full-screen lens driven from the
// core's OnWorldRenderEnd event). Where that module sampled the finished frame through D3D9 by hand, this one
// goes through the engine's own Gx facade, so it stays a normal engine module and needs no extension host.
//
// One frame, in order:
//
//   1. The world has just finished drawing to the default back buffer. That surface is copied whole into a
//      scratch texture, because a surface cannot be sampled and written in the same pass.
//   2. One fullscreen pass reads the scratch and writes a smoothed image back over the frame, edge-directed:
//      the local luminance range decides whether a pixel sits on an edge, and if it does the colour is taken
//      from a blur along the edge's own direction. Flat interiors are copied through untouched, so detail
//      that is already sharp stays sharp while the 1-pixel stair step on a silhouette is removed.
//   3. The render target, render states and matrices are put back exactly as the world left them.
//
// The pass runs at the world -> interface boundary, immediately after the world's own geometry and before
// the outline is composited. That placement is the whole point: the outline, the interface, the text and the
// icons are drawn afterwards and are therefore never filtered, which is the difference between a smoother 3D
// view and a blurry font.
//
// It stands down and does nothing when the option is off, on any device but D3D9 (the shader is D3D9
// bytecode), when the device already has hardware MSAA running (the frame it would be filtering has already
// been resolved; filtering it again only costs frame rate), when the world is drawing into an off-screen
// buffer (the capture reads the default surface and would take the wrong image), and when the world drew
// into a sub-rectangle of the target (the pass is built for the whole target, and filtering a viewport inset
// would smooth whatever is beside it).
class CAntiAlias {
    public:
    // Not in the original binary. Registers the antiAlias cvar and the device-restored callback. Called once
    // from the world frame constructor, which happens again after a UI reload, so it is idempotent.
    static void Init();

    // Not in the original binary. Runs the pass over the frame the world just finished. Only ever called
    // while the default back buffer is bound, immediately after the world geometry and before the outline.
    static void Apply();

    // Not in the original binary. Whether the option is on and the device can run the pass at all.
    static bool Enabled();

    private:
    static bool EnsureResources();
    static void OnDeviceRestored();

    // The scratch the frame is captured into, and the cached gx texture the pass samples from it. The type
    // is FFX's because it carries both sizes -- the frame's own and the one the device actually allocated --
    // and the pass needs both: the capture copies the requested size, the shader steps by the allocated one.
    static CVar* s_cvarAntiAlias;
    static CVar* s_cvarFxaaQuality;
    static CGxShader* s_shader;
    static FFX::Tex s_sceneTex;
    static CGxTex* s_scene;
};

#endif