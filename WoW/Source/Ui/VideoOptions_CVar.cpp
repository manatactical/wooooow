// CVars the Advanced video panel (Interface\FrameXML\VideoOptionsPanels.lua, shipped by Data\custom.MPQ)
// expects but this client never registered. GetCVar/SetCVar go through CVar::LookupRegistered and raise a
// Lua error for an unknown name, which is what stopped the panel from loading at all (VideoOptionsPanels.lua
// line 441, GetCVar("shadowMode")).
//
// NOT an engine port: these are compatibility stubs, so there is no // OFFSET: tag. The value is stored and
// the client accepts it; the callback logs the change and applies nothing. The knobs that do exist in this
// tree (CShadowCache::SetShadowMode, shadowLevel, extShadowQuality) are not wired to these names yet, so a
// control here has no visible effect. Wire shadowMode to CShadowCache::SetShadowMode when shadows are next
// touched; waterDetail has a near neighbour in waterLOD, but it is a different scale, so it stays a stub
// until someone checks them against each other.

#include <cstdint>
#include <cstdio>
#include "Engine/Source/Base/Types.h"
#include "Engine/Source/ConsoleBase/ConsoleVar.h"
#include "Engine/Source/ConsoleBase/Types.h"
#include "Engine/Source/Gx/Gx.h"
#include "Engine/Source/Storm/H/storm.h"
#include "VideoOptions.h"

// VERIFY: (new) manual
// One callback for every stub: says which CVar changed, and to what, so the trace answers whether the panel
// reached the client at all. Applying nothing is an unimplemented effect, not a guess -- the line says so
// rather than leaving the change silent.
static bool StubCallback(CVar* var, const char* oldValue, const char* value, void* arg) {
    GxLog("VideoOptions: '%s' '%s' -> '%s' (stub, no engine effect)", var->m_key.m_str, oldValue ? oldValue : "", value ? value : "");
    return true;
}

// VERIFY: (new) manual
void VideoOptions::RegisterClientCVars() {
    // Shadows: the panel drives shadowMode/shadowTextureSize, this tree stores its shadow settings elsewhere.
    CVar::Register("shadowMode", "Shadow quality mode (stub: see CShadowCache::SetShadowMode)", 1, "1", StubCallback, CATEGORY_GRAPHICS, false, nullptr, false);
    CVar::Register("shadowTextureSize", "Shadow map size (stub)", 1, "1024", StubCallback, CATEGORY_GRAPHICS, false, nullptr, false);

    // Effects the panel exposes but this tree does not implement.
    CVar::Register("sunShafts", "Sunshaft and glare effects (stub: not implemented)", 1, "0", StubCallback, CATEGORY_GRAPHICS, false, nullptr, false);
    CVar::Register("postBloom", "Bloom post-process (stub: not implemented)", 1, "0", StubCallback, CATEGORY_GRAPHICS, false, nullptr, false);
    CVar::Register("postAA", "Post-process anti-aliasing (stub: not implemented)", 1, "0", StubCallback, CATEGORY_GRAPHICS, false, nullptr, false);
    CVar::Register("aerialFog", "Aerial fog (stub: not implemented)", 1, "0", StubCallback, CATEGORY_GRAPHICS, false, nullptr, false);
    CVar::Register("reflectionMode", "Water reflection mode (stub: not implemented)", 1, "3", StubCallback, CATEGORY_GRAPHICS, false, nullptr, false);

    // Water: waterDetail and rippleDetail both gate the panel's LiquidDetail slider.
    CVar::Register("waterDetail", "Water detail level (stub: see waterLOD)", 1, "3", StubCallback, CATEGORY_GRAPHICS, false, nullptr, false);
    CVar::Register("rippleDetail", "Water ripple detail (stub: not implemented)", 1, "2", StubCallback, CATEGORY_GRAPHICS, false, nullptr, false);

    // The panel's texture-detail slider reads this one back to pick its position.
    CVar::Register("componentTextureLevel", "Texture detail level as the video panel sees it (8 custom, 9 original)", 1, "8", StubCallback, CATEGORY_GRAPHICS, false, nullptr, false);
}