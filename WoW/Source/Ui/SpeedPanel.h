#ifndef WOW_SOURCE_UI_SPEEDPANEL_H
#define WOW_SOURCE_UI_SPEEDPANEL_H

#include <cstdint>

// The speedflying HUD and talent panel, drawn by FrameXML.
//
// Everything visual is Lua/XML loaded from Interface\FrameXML\WXL_SpeedPanel.xml, which is listed inside the
// `## add new modules below here` block of custom.MPQ's FrameXML.toc. That toc is safe to extend: with
// DISABLE_SIGNATURE_CHECKS defined, Engine/Source/FrameXML/LoadXML.cpp:FrameXML_CheckSignature returns 3
// immediately without reading FrameXML.toc.sig at all, so neither the toc nor a new file is verified.
//
// This replaces the native C++ HUD (WoW/Source/Ui/SpeedHud.cpp), which could not be made to draw reliably:
// CSimpleStatusBar re-anchors its own texture on a state machine that silently draws nothing when a flag is
// stale, and the frame-unit sizing (fractions of screen, not pixels) kept producing a ~500px panel with a
// smaller bar inside it. FrameXML is the layer this UI was authored in, and it is the same layer the rest of
// the interface uses.
//
// The C++ side keeps owning the DATA: it parses the WXLMS/WXLTT link and publishes plain globals the Lua
// reads (WXL_MountAnimSpeedPct, WXL_MountAnimState, WXL_TalentD, ...). Nothing here draws.
namespace SpeedPanel
{
    // Publishes the talent table and per-frame values for the Lua panel. Called from the active player's
    // PreAnimate, which already runs every frame.
    void Update(uint32_t timeMs);

    // Called once the frame script context exists.
    void Initialize();
}

#endif
