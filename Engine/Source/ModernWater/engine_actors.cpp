#include "Engine/Source/ModernWater/engine_actors.h"

#include "Engine/Source/ModernWater/log.h"

#include "Engine/Source/Tempest/C3Vector.h"
#include "WoW/Source/Object/Movement.h"
#include "WoW/Source/Object/ObjectClient/Unit_C.h"
#include "WoW/Source/Object/Types.h"
#include "WoW/Source/ObjectMgrClient/ObjectMgrClient.h"
#include "WoW/Source/WorldClient/MapEntity.h"
#include "WoW/Source/WorldClient/MapLoad.h"
#include "WoW/Source/WorldClient/World.h"

#include <algorithm>
#include <cmath>
#include <limits>

// Not in the original binary. Which units are standing in water, for the ripples.
//
// The WXL module walked the client's visible-object list through absolute offsets and guarded every one of them
// against a byte signature, because it was reading a binary it could not compile. This tree compiles the client,
// so the same walk is expressed in types: the object manager enumerates its own visible objects, and each unit's
// position, collision size, movement flags and liquid surface are ordinary fields.
//
// The rules are the module's, kept because they were tuned against the game:
//   * only units whose body crosses the liquid surface count, so someone standing beside a lake and someone
//     flying over it do not ripple;
//   * a contact deeper than the client's own ripple depth limit is not a contact (the client uses the same cut
//     for its splash sprites, so the two agree about what "in the water" means);
//   * the 32 nearest to the camera centre win, sorted by distance, which is what the shader's fixed-size
//     disturbance table expects.

namespace {

constexpr float kMaxWorldCoordinate = 100000.0f;
constexpr float kMaxCollisionRadius = 50.0f;
constexpr float kMaxCollisionHeight = 100.0f;
constexpr float kMinClientRippleDepthLimit = 1.0f;
constexpr float kClientRippleDepthLimitPerHeight = 2.0f;

bool FiniteWithin(float value, float minimum, float maximum) {
    return std::isfinite(value) && value >= minimum && value <= maximum;
}

bool ValidCentre(const float centre[3]) {
    for (int axis = 0; axis < 3; ++axis) {
        if (!FiniteWithin(centre[axis], -kMaxWorldCoordinate, kMaxWorldCoordinate)) {
            return false;
        }
    }

    return true;
}

bool PlausibleContact(const WaterContact& contact) {
    for (float axis : contact.position) {
        if (!FiniteWithin(axis, -kMaxWorldCoordinate, kMaxWorldCoordinate)) {
            return false;
        }
    }

    return FiniteWithin(contact.surface, -kMaxWorldCoordinate, kMaxWorldCoordinate) && contact.radius > 0.0f &&
           FiniteWithin(contact.radius, 0.0f, kMaxCollisionRadius) && contact.height > 0.0f &&
           FiniteWithin(contact.height, 0.0f, kMaxCollisionHeight) &&
           WaterContactDepth(contact) < ClientRippleDepthLimit(contact.height);
}

double SquaredGroundDistance(const WaterContact& contact, const float centre[3]) {
    const double dx = static_cast<double>(contact.position[0]) - centre[0];
    const double dy = static_cast<double>(contact.position[1]) - centre[1];
    return dx * dx + dy * dy;
}

bool Precedes(const WaterContact& a, const WaterContact& b, const float centre[3]) {
    const double aDistance = SquaredGroundDistance(a, centre);
    const double bDistance = SquaredGroundDistance(b, centre);

    if (aDistance != bDistance) {
        return aDistance < bDistance;
    }

    return a.guid < b.guid;
}

struct GatherContext {
    const float* centre = nullptr;
    WaterContactFrame* out = nullptr;
};

// One visible object. Returns non-zero to keep enumerating. The cast is guarded: ClntObjMgrEnumVisibleObjects
// hands back every visible object type, and only units that report a liquid surface become contacts.
int32_t GatherContact(WOWGUID guid, void* param) {
    auto* context = static_cast<GatherContext*>(param);
    CGObject_C* object = ClntObjMgrObjectPtr(guid, HEIR_TYPE_UNIT, __FILE__, __LINE__);

    if (!object) {
        return 1;
    }

    auto* unit = static_cast<CGUnit_C*>(object);
    CMapEntity* entity = unit->m_worldObject;

    if (!entity) {
        return 1;
    }

    // QueryObjectLiquid is the client's own answer, and it is the one the camera and the sound engine use. It
    // returns 0 for a unit that is not over liquid at all, which is most of them.
    int32_t liquidType = 0;
    int32_t liquidFlags = 0;
    float surface = 0.0f;

    if (!World::QueryObjectLiquid(entity, &liquidType, &surface, &liquidFlags)) {
        return 1;
    }

    // Only a body that crosses the surface disturbs it. Someone standing on the bottom of a shallow pool with
    // their head out still counts; someone entirely under does not, and the flag says which.
    int32_t crossesBody = 0;
    int32_t unusedFlag = 0;

    if (!World::QueryObjectLiquidFlags(entity, &crossesBody, &unusedFlag) || !crossesBody) {
        return 1;
    }

    WaterContact contact;
    const C3Vector position = unit->GetPosition();
    contact.guid = guid.m_guid;
    contact.position[0] = position.x;
    contact.position[1] = position.y;
    contact.position[2] = position.z;
    contact.surface = surface;
    contact.radius = unit->m_movementData.m_collisionRadius;
    contact.height = unit->m_movementData.m_collisionHeight;
    contact.swimming = (unit->m_movementData.m_flags & MOVEMENTFLAG_SWIMMING) != 0;
    contact.onTransport = (unit->m_movementData.m_flags & MOVEMENTFLAG_ONTRANSPORT) != 0;
    engine::SelectWaterContact(*context->out, contact, context->centre);
    return 1;
}

}  // namespace

float WaterContactDepth(const WaterContact& contact) {
    return contact.surface - contact.position[2];
}

float ClientRippleDepthLimit(float height) {
    return std::max(kMinClientRippleDepthLimit, kClientRippleDepthLimitPerHeight * height);
}

namespace engine {

bool SelectWaterContact(WaterContactFrame& frame, const WaterContact& contact, const float centre[3]) {
    if (frame.count > kMaxWaterContacts || !ValidCentre(centre) || !PlausibleContact(contact) ||
        SquaredGroundDistance(contact, centre) > static_cast<double>(kWaterContactRange) * kWaterContactRange) {
        return false;
    }

    // Sorted insertion, nearest first: the shader's disturbance slots are allocated in this order, so the units
    // closest to the camera always get one even when more than 32 are standing in the water.
    uint32_t index = 0;

    while (index < frame.count && !Precedes(contact, frame.contacts[index], centre)) {
        ++index;
    }

    if (index == kMaxWaterContacts) {
        return false;
    }

    const uint32_t count = std::min(frame.count + 1, kMaxWaterContacts);

    for (uint32_t i = count - 1; i > index; --i) {
        frame.contacts[i] = frame.contacts[i - 1];
    }

    frame.contacts[index] = contact;
    frame.count = count;
    return true;
}

bool CaptureWaterContacts(const float centre[3], WaterContactFrame& out) {
    out = {};

    if (!ValidCentre(centre)) {
        return false;
    }

    GatherContext context{centre, &out};

    // The enumerator walks the client's own visible list, so nothing here depends on where that list lives in
    // memory or how it is linked. A truncation is reported rather than hidden: past the limit the ripple pattern
    // is simply missing its farthest actors.
    ClntObjMgrEnumVisibleObjects(GatherContact, &context);
    return true;
}

}  // namespace engine
