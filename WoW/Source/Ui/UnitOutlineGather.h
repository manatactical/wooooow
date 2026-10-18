#ifndef WOW_SOURCE_UI_UNITOUTLINEGATHER_H
#define WOW_SOURCE_UI_UNITOUTLINEGATHER_H

// Not in the original 3.3.5a binary. The game side of the unit outline: picks the units this frame should
// be outlined, works out their reaction colour, and hands the model chains to CUnitOutline. See
// UnitOutlineGather.cpp for the two places the port differs from wxl-mana-unit-outline.
//
// Called once per frame, right before the world draws, because a target's silhouette is stamped from the
// batches of the frame it is drawn in and must be recorded before those batches go by.
class CUnitOutlineGather {
    public:
    static void Update();
};

#endif