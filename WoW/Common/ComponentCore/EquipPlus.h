#ifndef WOW_COMMON_COMPONENTCORE_EQUIPPLUS_H
#define WOW_COMMON_COMPONENTCORE_EQUIPPLUS_H

#include <cstdint>
#include "WoW/Common/ComponentCore/Types.h"

class CCharacterComponent;
class CM2Model;
class CreatureDisplayInfoRec;
class ItemDisplayInfoRec;

// Not in the original binary. A port of the WarcraftXL "Equip Plus" extension (wxl-equip-plus, GPL-3.0-or-later) into the
// client itself. See docs/EQUIP-PLUS.md.
//
// A stock 3.3.5a client draws a model only for the head, the shoulders and held items. Every other armour slot is a
// texture painted on the body. Equip Plus lets an ItemDisplayInfo row, or a row in a CSV sidecar, attach up to two M2
// models to any slot, including retail "collection" pieces with a geoset filter and a skeleton to follow.
//
// It runs from CCharacterComponent::AddItem and RemoveItem, so it covers the player, other players, NPCs wearing gear,
// the character screens and the dress-up window alike.
class EquipPlus {
    public:
    // Registers the cvars. Called from CCharacterComponent::Initialize.
    static void Init();

    // Whether the system is on (the equipPlus cvar).
    static bool Enabled();

    // Re-reads every CSV sidecar. Items already worn keep what they have until they are equipped again.
    static void Reload();

    // Head and shoulders have a model of their own in the client. True when Equip Plus draws this display's model for the
    // slot instead: a CSV row, a collection name, or an Icon2 setting says so.
    static bool OwnsNativeModel(ITEM_SLOT slot, const ItemDisplayInfoRec* displayRec);

    // Attaches the models this display asks for on this slot, replacing whatever Equip Plus attached there before.
    static void Apply(CCharacterComponent* component, ITEM_SLOT slot, const ItemDisplayInfoRec* displayRec);

    // Detaches what Equip Plus attached on this slot.
    static void Remove(CCharacterComponent* component, ITEM_SLOT slot);

    // The component's model was replaced: forget the old attachments without touching the old model.
    static void Reset(CCharacterComponent* component);

    // A weapon or shield has just been attached to `link` by the client. If WXLWeaponModels.csv has a row for the display,
    // draws that instead: another model, a geoset filter, textures of any type, a second model for effects.
    static void ApplyWeapon(CM2Model* parent, int32_t link, const ItemDisplayInfoRec* displayRec, bool offhand);

    // A creature model has just been created for this display. Applies WXLCreatureTextures.csv and the geoset list of
    // WXLCreatureModels.csv.
    static void ApplyCreature(CM2Model* model, const CreatureDisplayInfoRec* displayRec);

    // The equipplus console command.
    static void RegisterCommands();

    // The component is going away.
    static void Destroy(CCharacterComponent* component);
};

#endif
