#include "Engine/Source/ModernWater/water_inputs.h"

#include "Engine/Source/Gx/CGxDevice/CGxDevice.h"
#include "Engine/Source/Gx/Gx.h"
#include "Engine/Source/ModernWater/log.h"

#include "WoW/Source/ObjectMgrClient/ObjectMgrClient.h"
#include "WoW/Source/Ui/WorldFrame.h"
#include "WoW/Source/UIUtil/Camera.h"
#include "WoW/Source/WorldClient/DayNight.h"
#include "WoW/Source/WorldClient/DayNight__DNInfo.h"
#include "WoW/Source/WorldClient/World.h"

#include <cmath>
#include <cstring>

namespace ModernWater {

namespace {

// The sun is above the horizon for this fraction of the day; outside it the moon is the light. The client's own
// bands carry the same split, and the shader only needs to know which body is lighting the water so it can dim a
// moonlit highlight instead of drawing a second sun.
bool LightIsMoon(float dayFraction) {
    return !(dayFraction >= 0.25f && dayFraction <= 0.75f);
}

uint32_t PackColour(const CImVector& colour) {
    return (static_cast<uint32_t>(colour.a) << 24) | (static_cast<uint32_t>(colour.r) << 16) |
           (static_cast<uint32_t>(colour.g) << 8) | static_cast<uint32_t>(colour.b);
}

}  // namespace

void ReadSkyAndWaterColours(WaterFrameInputs& out) {
    // DayNight::g_dnInfo.m_bands is the blended current light: the zone's own bands already interpolated by time
    // of day and by the area lights the player is standing in. It is exactly what the client's sky uses, so the
    // water's reflections match the sky above it instead of guessing at a colour.
    const DayNight::CurrentLight& bands = DayNight::g_dnInfo.m_bands;

    for (int index = 0; index < 6; ++index) {
        out.skyColors[index] = PackColour(bands.m_skyColors[index]);
    }

    // The water bands are the zone's intended shallow and deep water colours. The shader tints its physical
    // scattering towards these, which is what makes a lake in Nagrand read differently from one in Tirisfal.
    out.riverColors[0] = PackColour(bands.m_waterColors[0]);
    out.riverColors[1] = PackColour(bands.m_waterColors[1]);
}

bool BuildFrameInputs(WaterFrameInputs& out) {
    out = {};

    CGCamera* camera = CGWorldFrame::GetActiveCamera();

    if (!camera) {
        return false;
    }

    // The camera's world position and the point it looks at, which the ripple window is centred on and which the
    // shading pass needs to place the water volume in the world.
    const C3Vector& cameraPosition = camera->m_position;
    out.camPos[0] = cameraPosition.x;
    out.camPos[1] = cameraPosition.y;
    out.camPos[2] = cameraPosition.z;

    // The device holds both matrices already, in the convention every other shader in this client consumes them,
    // so the water cannot disagree with the terrain about where the camera is or how wide the frustum is.
    C44Matrix view;
    GxXformView(view);
    std::memcpy(out.cameraRelativeView, &view, sizeof(out.cameraRelativeView));

    // The native projection, i.e. the matrix the device actually used to write the depth this pass decodes. The
    // device remaps z into [0,1] before storing it (CGxDeviceD3d9Ex::XformSetProjection), and MapWorldDepth is written
    // against that same [0,1] form -- so the two agree by construction. Feeding it the pre-remap m_projection would
    // put the two a half-range apart and invert the sign of every decoded distance.
    //
    // GxXformProjection, not GxXformViewProj: the latter returns view * projection, which is not a projection at all.
    std::memcpy(out.glProjection, &g_theGxDevicePtr->m_projNative, sizeof(out.glProjection));

    D3DVIEWPORT9 viewport = {};
    const CRect& window = g_theGxDevicePtr->DeviceCurWindow();
    viewport.X = 0;
    viewport.Y = 0;
    viewport.Width = static_cast<DWORD>(window.maxX);
    viewport.Height = static_cast<DWORD>(window.maxY);
    viewport.MinZ = 0.0f;
    viewport.MaxZ = 1.0f;
    out.viewport = viewport;

    // The camera's look direction. A C44Matrix's a0..a3 is its first row, which in the view matrix is the
    // camera's right vector, so the forward axis is the negated third row (c0..c2). The ripple window is centred
    // on a point a few yards ahead of the camera, because that is where the player is looking.
    const float forward[3] = {-view.c0, -view.c1, -view.c2};
    out.camTarget[0] = out.camPos[0] + forward[0] * 8.0f;
    out.camTarget[1] = out.camPos[1] + forward[1] * 8.0f;
    out.camTarget[2] = out.camPos[2];

    const DayNight::DNInfo& info = DayNight::g_dnInfo;
    out.inLiquid = camera->m_underwater != 0;
    out.mapId = static_cast<int>(ClntObjMgrGetMapID());

    // The light direction and colour. m_light0's direction is the client's fixed lighting vector, which points down
    // (its z is negative all day) and does not follow the sun, so the shader read the sun as always below the horizon
    // and drew no sun light or glint. The direction to the light is taken from the sun or moon the sky actually draws.
    const DNLightState& light = info.m_light0;
    out.lightIsMoon = LightIsMoon(info.m_dayProgression);
    const DayNight::DNPlanet& planet = out.lightIsMoon ? DayNight::g_planets.m_moon : DayNight::g_planets.m_sun;
    float toLight[3] = {static_cast<float>(planet.m_position.x - info.m_cameraPos.x),
                        static_cast<float>(planet.m_position.y - info.m_cameraPos.y),
                        static_cast<float>(planet.m_position.z - info.m_cameraPos.z)};
    const float toLightLength = std::sqrt(toLight[0] * toLight[0] + toLight[1] * toLight[1] + toLight[2] * toLight[2]);

    if (toLightLength > 1e-4f) {
        for (int axis = 0; axis < 3; ++axis) {
            out.toLight[axis] = toLight[axis] / toLightLength;
        }
    } else {
        out.toLight[0] = -light.m_dir.x;
        out.toLight[1] = -light.m_dir.y;
        out.toLight[2] = -light.m_dir.z;
    }

    out.sunColor = PackColour(info.m_bands.m_sunCloudColors[0]);
    out.directColor = PackColour(light.m_diffuse);
    out.ambientColor = PackColour(light.m_ambient);

    // Fog: the group fog is what the frame is drawn with. The client draws its water through this same fog, so
    // the water's own fog has to be the same or the shoreline would not match the lake behind it.
    const DNFogInfo& fog = info.m_fogGroup.m_end > 0.0f ? info.m_fogGroup : info.m_fog;
    out.fogColor = PackColour(fog.m_color);
    out.fogStart = fog.m_start;
    out.fogEnd = fog.m_end;
    out.stockFogApplies = fog.m_end > fog.m_start;

    ReadSkyAndWaterColours(out);

    return std::isfinite(out.camPos[0]) && std::isfinite(out.camPos[1]) && std::isfinite(out.camPos[2]) &&
           std::isfinite(out.fogEnd);
}

void ReadContacts(const WaterFrameInputs& in, WaterContactFrame& out) {
    engine::CaptureWaterContacts(in.camTarget, out);
}

}  // namespace ModernWater
