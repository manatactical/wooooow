#ifndef ENGINE_SOURCE_MODERNWATER_ENGINE_ACTORS_H
#define ENGINE_SOURCE_MODERNWATER_ENGINE_ACTORS_H

#include <cstdint>

// Not in the original 3.3.5a binary. The actors around the camera that are standing in liquid, which is what
// drives the ripples. The WXL module read these by walking the client's visible-object list through absolute
// addresses into Wow.exe; this tree compiles the client, so the same information is read out of the live
// objects directly and no layout guard bytes are needed.
//
// A "contact" is a unit whose body crosses a liquid surface: standing in it, swimming in it, or wading through
// it. Doodads, players on transports and units standing on dry ground are not contacts. The struct is the same
// one the ported ripple code consumes, so water_contacts.cpp and water_ripples.cpp are unchanged.

constexpr uint32_t kMaxWaterContacts = 32;
constexpr float kWaterContactRange = 48.0f;

struct WaterContact {
    uint64_t guid = 0;
    float position[3] = {};
    float surface = 0.0f;
    float radius = 0.0f;
    float height = 0.0f;
    bool swimming = false;
    bool onTransport = false;
};

struct WaterContactFrame {
    WaterContact contacts[kMaxWaterContacts] = {};
    uint32_t count = 0;
    bool truncated = false;
};

float WaterContactDepth(const WaterContact& contact);
float ClientRippleDepthLimit(float height);

namespace engine {
bool SelectWaterContact(WaterContactFrame& frame, const WaterContact& contact, const float centre[3]);
bool CaptureWaterContacts(const float centre[3], WaterContactFrame& out);
}  // namespace engine

#endif
