#ifndef WOW_SOURCE_UI_LOOTBEAM_H
#define WOW_SOURCE_UI_LOOTBEAM_H

#include <cstddef>
#include <cstdint>

// Not in the original 3.3.5a binary. A beam of light that rises from every NPC corpse that can still be
// looted, ported from the WXL module wxl-loot-beam (WarcraftXL), where it was a client extension drawn
// from the core's render events.
//
// Identity: the beacon is a looted body's marker, so it is looked for, never hidden. It is drawn additively
// straight onto the finished scene, through terrain and walls, at OnWorldRender one step past the world
// geometry -- see the .cpp for why that slot and not the depth-tested one.
//
// The colour is the quality of the rarest item in the corpse's loot. The stock client only learns a
// corpse's loot when loot is requested for it, so a body colours itself when the loot window opens; the
// companion AzerothCore module mod-loot-beam additionally writes the quality onto the corpse's own
// UNIT_FIELD_PADDING when it dies, and that hint is merged in so the beam is right the moment the body
// falls. See UnitLootBeamField.
//
// Switch: the lootBeam cvar, registered by CWorldParam::Initialize so the Advanced video panel can read
// its default before any world frame exists, and read here from the render path. The look itself is
// compile-time (BeamStyle), which is what keeps the port free of file I/O.
namespace LootBeam
{
    // Registers the device-restored callback. Called once from the world frame's constructor; idempotent,
    // because a world frame is built again after a UI reload and the device's callback list is append-only.
    void Init();

    // Called once per frame from the world render, after the scene's geometry and before the interface.
    // Scans the resident units for lootable corpses, advances their beacons and draws them.
    void Update(float elapsedSec);
}

#endif
