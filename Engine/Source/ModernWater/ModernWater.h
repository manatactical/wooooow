#ifndef ENGINE_SOURCE_MODERNWATER_MODERNWATER_H
#define ENGINE_SOURCE_MODERNWATER_MODERNWATER_H

#include "Engine/Source/ModernWater/water_renderer.h"
#include "Engine/Source/ModernWater/water_types.h"

namespace Liquid {
class CInstance;
class CMaterialSettings;
}  // namespace Liquid

// Not in the original 3.3.5a binary. Modern water: the water of a modern client -- GPU FFT waves, foam, sun
// highlights, scene reflections, refraction, depth-aware absorption and per-zone colours -- inside this client.
//
// Ported from the WarcraftXL extension module wxl-modern-water, which is itself derived from CoAVolFog. The
// shading is that module's, unchanged; what changed is how the pass is reached.
//
// The module could not patch this client's code, so it drew a second pass over the finished frame and found the
// water pixels by catching the client's liquid draws through a D3D9 device wrapper and tagging them with a
// stencil reference. That is what forced the wrapper, the NVAPI/RESZ multisampled depth copy, and a table of
// absolute addresses guarded by byte signatures.
//
// This tree compiles the client, so the same pass is reached by calling it: Liquid::CRenderer::Flush arms the
// pass before the transparent liquid draws and shades afterwards. Everything the module read out of Wow.exe is
// read out of the structures that own it (see water_inputs.cpp), and the liquid class comes from the settings
// object the client already holds instead of from a signature-checked memory walk.
namespace ModernWater {

// Registers the device-restored callback that frees the pass's default-pool textures on a device reset.
void Init();

// The tuning the WXL module read from its ini. Hardcoded to that file's documented defaults: this client has no
// overlay panel and no second ini, so the numbers live here where they can be found and changed.
WaterTimeInputs WaterSettings();

// The class of one liquid instance, or WaterClass::None for a liquid this pass does not shade. None means the
// client's own material keeps the draw -- slime and ooze are deliberately left alone, because they are not water
// and the water model would turn them into a slightly wrong lake.
WaterClass ClassifyLiquidInstance(const Liquid::CInstance* instance);

// True when the instance should be tagged for the modern pass.
bool TagLiquidInstance(const Liquid::CInstance* instance);

// True while the modern water pass has taken over this frame's liquid draws. A liquid material asks this before
// deciding whether to write depth: the pass finds its pixels by comparing the frame's depth before and after the
// liquid draws, so the water has to write depth even though the client's own water deliberately does not.
bool WaterPassArmed();

// True when the modern pass shaded this frame's water, so the client's own splash and wake rings -- decals the old water
// drew its ripples with -- are redundant and are skipped. Set by Liquid::CRenderer::Flush, read by
// CMap::RenderWaterRipples right after it; false whenever the pass did not run, which brings the stock rings back.
bool ClientRipplesSuppressed();
void SetClientRipplesSuppressed(bool suppressed);

}  // namespace ModernWater

#endif
