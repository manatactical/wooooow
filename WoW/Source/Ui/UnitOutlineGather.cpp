// Not in the original 3.3.5a binary. The game side of the unit outline: it decides what to outline and in
// what colour, and hands that to CUnitOutline, which owns the shaders and the masks. The split is the same
// one the WXL module had between its event handlers and its render script.
//
// Ported from wxl-mana-unit-outline (WarcraftXL). Two things the original did are not here:
//   * the quest giver set came from a paired server module over an addon prefix, which this client has no
//     counterpart for; a unit's own quest flag is used instead, so the colour appears without the link.
//   * the original asked Lua for combat state, because that was the only surface its SDK had. The client's
//     own CGUnit_C::IsInCombat is the same answer without the script call.
#include "WoW/Source/Ui/UnitOutlineGather.h"

#include <cstdint>

#include "Engine/Source/AmbientOcclusion/AmbientOcclusion.h"
#include "Engine/Source/ConsoleBase/ConsoleVar.h"
#include "Engine/Source/Model2/M2Model.h"
#include "Engine/Source/Storm/H/storm.h"
#include "Engine/Source/UnitOutline/UnitOutline.h"
#include "WoW/Source/Object/ObjectClient/GameObject_C.h"
#include "WoW/Source/Object/ObjectClient/Object_C.h"
#include "WoW/Source/Object/ObjectClient/Player_C.h"
#include "WoW/Source/Object/ObjectClient/Unit_C.h"
#include "WoW/Source/Object/GameObject.h"
#include "WoW/Source/Object/Types.h"
#include "WoW/Source/ObjectMgrClient/ObjectMgrClient.h"
#include "WoW/Source/Ui/GameUI.h"
#include "WoW/Source/Ui/PetInfo.h"
#include "WoW/Source/Ui/WorldFrame.h"
#include "WoW/Source/UIUtil/Camera.h"

namespace {

// The look. The WXL module had these in an INI and an overlay panel; the video options tab carries the
// master switch only, so they are constants here. Values match WXL's defaults except where noted.
const float kHostile[3]        = { 0.545f, 0.0f, 0.0f };  // darkened blood red, not the WXL purple
const float kNeutral[3]        = { 1.0f, 1.0f, 0.0f };
const float kFriendly[3]       = { 0.0f, 0.784f, 0.0f };
const float kFriendlyPlayer[3] = { 0.376f, 0.376f, 1.0f };  // the stock selection circle for a friendly player: 0x6060FF
const float kQuestGiver[3]     = { 1.0f, 0.82f, 0.0f };
const float kDoodad[3]         = { 200.0f / 255.0f, 170.0f / 255.0f, 120.0f / 255.0f };

const float kMouseoverBrightness = 1.0f;

// Which map objects get an outline. A curated list rather than the server's own flag, matching the WXL
// module: the interactive furniture the player expects to click -- a door, a chest, a chair, a mailbox --
// and not the purely decorative ones. Type numbers are the GAMEOBJECT_TYPE numbers the client stores in
// CGGameObjectData::m_typeID.
bool InterestingGameObject(CGGameObjectData* data) {
    if (!data) {
        return false;
    }

    switch (data->m_typeID) {
        case 0:   // DOOR
        case 1:   // BUTTON
        case 2:   // QUESTGIVER
        case 3:   // CHEST
        case 5:   // GENERIC
        case 7:   // CHAIR
        case 10:  // GOOBER
        case 19:  // MAILBOX
        case 23:  // MEETINGSTONE
        case 32:  // BARBER_CHAIR
        case 34:  // GUILD_BANK
            return true;
        default:
            return false;
    }
}

// How often the quest-giver walk runs. Only the search is on this timer; a giver stays recorded every frame
// once found, and its occlusion is the depth test's business rather than a trace on a tick.
constexpr uint32_t kQuestGiverMs = 60;

// The first quest status that means "there is something here for this player". The server computes it per
// object into CGObject_C::m_questStatus, where 0 is no quest, 1 unavailable, and 2/3/4 are the low-level
// family: the client's own minimap hides those three unless trivial quests are being tracked, which is the
// same line drawn here.
constexpr uint32_t kQuestStatusLevelAppropriate = 5;

uint32_t s_walkTick;

// Fills the outline colour from a reaction, exactly as the WXL module's ColorForReaction did.
void ColorForReaction(int32_t reaction, float* rgb) {
    const float* src;

    if (reaction < 2) {
        src = kHostile;
    } else if (reaction < 4) {
        src = kNeutral;
    } else {
        src = kFriendly;
    }

    rgb[0] = src[0];
    rgb[1] = src[1];
    rgb[2] = src[2];
}



// Gear -- a weapon, a shield, a helm, the shoulders, a cape -- is its own model hung off the body's
// attachment chain (CCharacterComponent::AddLink), so a match on the body alone leaves it unoutlined. Only
// item models are taken: the same chain carries the quest icon, spell visuals and enchant glows, and those
// are not part of the silhouette. Every one of them lives under Item\ObjectComponents\... .
bool IsGearModel(CM2Model* model) {
    const char* path = (model && model->m_shared) ? model->m_shared->m_fileName : nullptr;
    return path && SStrStrI(path, "ObjectComponents") != nullptr;
}

void AddGearModels(CM2Model* body, const float rgb[3], bool keepFull, CM2Model* footRef) {
    for (CM2Model* child = body ? body->m_attachList : nullptr; child; child = child->m_attachNext) {
        if (IsGearModel(child)) {
            CUnitOutline::AddTarget(child, rgb, keepFull, footRef);
        }
    }
}

// A unit is up to two models: the rider, and the mount it hangs off when mounted. The outline matches
// models by exact pointer (see CUnitOutline::AddTarget), so both have to be recorded; recording only
// GetObjectModel() would outline the mount alone and leave the rider bare. Gear hangs off the rider.
void AddUnitModels(CGUnit_C* unit, const float rgb[3], bool keepFull) {
    // A corpse gets no silhouette: the outline is a selection marker, and a dead unit is not selected.
    if (!unit || unit->IsDeadOrGhost()) {
        return;
    }

    // The ground is under the mount when there is one, and under the body otherwise; the rider and the gear
    // fade from the same feet rather than from their own origins.
    CM2Model* footRef = unit->m_mountModel ? unit->m_mountModel : unit->m_worldModel;

    if (unit->m_mountModel) {
        CUnitOutline::AddTarget(unit->m_mountModel, rgb, keepFull, footRef);
    }

    if (unit->m_worldModel) {
        CUnitOutline::AddTarget(unit->m_worldModel, rgb, keepFull, footRef);
        AddGearModels(unit->m_worldModel, rgb, keepFull, footRef);
    }
}

// The player's own silhouette is subtracted from the line, so its gear has to go into the occluder mask
// too: without it the outline draws across the player's own weapon and shoulders instead of stopping at
// them.
void AddPlayerModels(CGPlayer_C* player) {
    CUnitOutline::SetPlayerModel(player->m_worldModel, player->m_mountModel);

    for (CM2Model* child = player->m_worldModel ? player->m_worldModel->m_attachList : nullptr; child; child = child->m_attachNext) {
        if (IsGearModel(child)) {
            CUnitOutline::AddOccluder(child);
        }
    }
}

// Records one selection. `mouseover` picks the extra brightness the cursor's own target gets. Occlusion is
// not decided here: the stamp depth tests against the scene, so a wall clips the line where it covers the
// unit rather than dropping the outline whole.
void AddSelection(const WOWGUID& guid, CGUnit_C* player, bool mouseover) {
    if (!guid) {
        return;
    }

    // Never the player's own character: hovering or targeting yourself is not a selection the look is for.
    if (guid == ClntObjMgrGetActivePlayer()) {
        return;
    }

    const bool isPlayer = (guid.m_guid >> 32) == 0;
    CGObject_C* object = ClntObjMgrObjectPtr(guid, isPlayer ? TYPE_PLAYER : TYPE_OBJECT, __FILE__, __LINE__);

    if (!object) {
        return;
    }

    // The cursor can be over a chest, a corpse or a herb as easily as a unit, and those have a different
    // layout: everything below reads unit fields, so a non-unit is skipped rather than cast into one.
    if (!(object->m_obj->m_type & TYPE_UNIT)) {
        return;
    }

    CM2Model* model = object->GetObjectModel();

    if (!model) {
        return;
    }

    CGUnit_C* unit = static_cast<CGUnit_C*>(object);
    int32_t reaction = player ? unit->UnitReaction(player) : 5;

    // Faction reaction leaves a neutral unit yellow even after it aggros, and the client's own UI turns it
    // red from the fight instead. Match that.
    if (player && reaction >= 2 && reaction < 4 && player->IsInCombat() && unit->IsInCombat()) {
        reaction = 0;
    }

    float rgb[3];

    // A friendly player reads blue rather than the friendly green, so the tint says "another player" and
    // not "another friendly unit". A player you can attack keeps its hostile or neutral tint.
    if (isPlayer && reaction >= 4) {
        rgb[0] = kFriendlyPlayer[0];
        rgb[1] = kFriendlyPlayer[1];
        rgb[2] = kFriendlyPlayer[2];
    } else {
        ColorForReaction(reaction, rgb);
    }

    // The mouseover multiplier rides on the colour: the edge shader recovers the line colour as rgb/alpha,
    // so scaling the whole entry brightens that one line without touching the mask's shape.
    if (mouseover) {
        rgb[0] *= kMouseoverBrightness;
        rgb[1] *= kMouseoverBrightness;
        rgb[2] *= kMouseoverBrightness;
    }

    // Rider and mount, not the attach chain: an effect hung off the unit is not part of its silhouette.
    AddUnitModels(unit, rgb, false);
}

// A world object the cursor is over: no reaction colour and no mount chain, so it is recorded on its own
// with the doodad colour. The client's mouseover selection for one is the GameObject GUID, and the object
// reaches its M2 instance through the same accessor a unit's model does, so the match is exact and leaves
// other placements of the same model alone.
void AddDoodadSelection(const WOWGUID& guid) {
    if (!guid) {
        return;
    }

    CGGameObject_C* object = static_cast<CGGameObject_C*>(ClntObjMgrObjectPtr(guid, TYPE_GAMEOBJECT, __FILE__, __LINE__));

    if (!object || !InterestingGameObject(object->m_gameObject)) {
        return;
    }

    CM2Model* model = object->GetObjectModel();

    if (!model) {
        return;
    }

    float rgb[3] = { kDoodad[0] * kMouseoverBrightness, kDoodad[1] * kMouseoverBrightness, kDoodad[2] * kMouseoverBrightness };
    CUnitOutline::AddDoodad(model, rgb);
}



// The quest givers the walk found. They are remembered by GUID, not by model: the walk only runs every
// kQuestGiverMs, and a unit that despawns in between frees its model, so a cached pointer would dangle. A
// GUID is resolved again each frame, which is a hash lookup.
//
// The re-record is per frame because a marker has to be in every frame's list -- the list is rebuilt each
// frame and the mask is cleared with it, so a giver left out of one frame blinks off. Only the walk itself
// is on the timer; occlusion comes from the depth test, which needs no tick of its own.
constexpr int32_t kMaxQuestGivers = 24;
WOWGUID s_questGiverGuids[kMaxQuestGivers];
int32_t s_questGiverCount;

int32_t QuestGiverEnumProc(WOWGUID guid, void* param) {
    (void)param;

    if (s_questGiverCount >= kMaxQuestGivers) {
        // Zero stops the walk, which is what bounds the list in a crowded hub.
        return 0;
    }

    CGUnit_C* unit = static_cast<CGUnit_C*>(ClntObjMgrObjectPtr(guid, TYPE_UNIT, __FILE__, __LINE__));

    if (!unit || unit->IsDeadOrGhost()) {
        return 1;
    }

    // A quest giver only counts while the server reports something there for this player: a giver whose
    // quests are all below the player is not a marker, which is what the WXL module got from its server
    // link and this client gets from m_questStatus.
    bool questGiver = unit->IsQuestGiver() && unit->m_questStatus >= kQuestStatusLevelAppropriate;

    // The one marker that is not quest gated, so a flight master keeps its outline whatever its quest status
    // happens to say. Trainers (0x10) are deliberately left out: a profession trainer is a shop the player
    // walks up to deliberately, not a marker worth highlighting from across the hub.
    bool marker = (unit->m_unit->m_unitNpcFlags & 0x2000) != 0;

    if (!questGiver && !marker) {
        return 1;
    }

    s_questGiverGuids[s_questGiverCount] = guid;
    s_questGiverCount++;


    // Non-zero keeps the walk going; the client's own enumerations use the same convention.
    return 1;
}

}  // namespace

// Not in the original binary.
void CUnitOutlineGather::Update() {
    // The list that is about to be filled describes this frame; the engine empties it after compositing.
    CUnitOutline::BeginTargets();

    // The ambient occlusion gives the player character its own close-up pass while the camera is near it. It is
    // told how far the camera is, here, because the scene's batches are what stamp the character and they come
    // after this. Far until proven otherwise, so a frame with no camera or no player runs the frame-wide pass only.
    CGCamera* camera = CGWorldFrame::GetActiveCamera();
    CAmbientOcclusion::SetCameraDistance(camera ? camera->m_distance : 1.0e9f);

    CGPlayer_C* player = static_cast<CGPlayer_C*>(ClntObjMgrObjectPtr(ClntObjMgrGetActivePlayer(), TYPE_PLAYER, __FILE__, __LINE__));

    if (!player) {
        return;
    }

    // The unit the player is actually controlling: a possess or a vehicle moves the camera and the target
    // frame to another unit, and that is the one whose reaction and combat state the outline should use.
    CGUnit_C* viewer = player->GetPossessedUnit();

    if (!viewer) {
        viewer = player;
    }

    AddPlayerModels(player);

    AddSelection(static_cast<CGUnit_C*>(viewer)->m_unit->m_unitFieldTarget, viewer, false);

    // The pet's target, so a hunter's pet reads the same as the player's own.
    CGUnit_C* pet = static_cast<CGUnit_C*>(ClntObjMgrObjectPtr(CGPetInfo::m_pet, TYPE_UNIT, __FILE__, __LINE__));

    if (pet) {
        AddSelection(pet->m_unit->m_unitFieldTarget, viewer, false);
    }

    AddSelection(CGGameUI::m_currentObjectTrack, viewer, true);

    // The cursor can equally be over a chest, a door or a chair, and those are the same mouseover selection
    // answering to a different type. No GUID test here: the object manager's own type mask is the
    // authoritative answer, and AddDoodadSelection asks for it and rejects a decorative object too.
    AddDoodadSelection(CGGameUI::m_currentObjectTrack);

    // Every found quest giver is recorded every frame, whether or not it is selected, so a marker cannot
    // blink off between walks. Only the walk that finds them is on the timer.
    uint32_t now = GetTickCount();

    if (now - s_walkTick >= kQuestGiverMs) {
        s_walkTick = now;
        s_questGiverCount = 0;
        ClntObjMgrEnumVisibleObjects(QuestGiverEnumProc, nullptr);
    }

    for (int32_t i = 0; i < s_questGiverCount; i++) {
        // Resolved again rather than remembered: a unit that despawned since the last walk answers null
        // here instead of being a dangling model pointer, and is simply left out.
        CGUnit_C* unit = static_cast<CGUnit_C*>(ClntObjMgrObjectPtr(s_questGiverGuids[i], TYPE_UNIT, __FILE__, __LINE__));

        if (unit) {
            AddUnitModels(unit, kQuestGiver, true);
        }
    }
}
