// Not in the original 3.3.5a binary. See GrassMotion.h for the pipeline and for the three seams this port
// uses instead of the reference module's three detours. This file is the implementation, and says where it
// differs from `seyris-living-azeroth`'s GrassMotion.cpp / GrassDoodads.cpp / ShaderPatch.cpp.
#include "WoW/Source/WorldClient/GrassMotion.h"

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include <d3dcompiler.h>
#include <windows.h>

#include "Engine/Source/ConsoleBase/ConsoleVar.h"
#include "Engine/Source/ConsoleBase/Types.h"
#include "Engine/Source/Gx/CGxBuf.h"
#include "Engine/Source/Gx/CGxDevice/CGxDevice.h"
#include "Engine/Source/Gx/CGxDevice/CGxShader.h"
#include "Engine/Source/Gx/CGxPool.h"
#include "Engine/Source/Gx/Gx.h"
#include "Engine/Source/Model2/M2Data.h"
#include "Engine/Source/Model2/M2Model.h"
#include "Engine/Source/Os/TimeManager.h"
#include "Engine/Source/Tempest/C3Vector.h"
#include "WoW/Source/Object/ObjectClient/Object_C.h"
#include "WoW/Source/Object/ObjectClient/Player_C.h"
#include "WoW/Source/Object/ObjectClient/Unit_C.h"
#include "WoW/Source/Object/Unit.h"
#include "WoW/Source/ObjectMgrClient/ObjectMgrClient.h"
#include "WoW/Source/WorldClient/CDetailDoodadData.h"
#include "WoW/Source/WorldClient/DetailDoodad.h"
#include "WoW/Source/WorldClient/World.h"

const float CGrassMotion::kMaxViewDistance = 196.0f;
const float CGrassMotion::kActorRange = 45.0f;

namespace {

// ---------------------------------------------------------------------------------------------
// The look. Anything a player tunes is a cvar; these are the module's own constants, named here beside the
// shader text with the reference module's value as the source for each.
// ---------------------------------------------------------------------------------------------
constexpr float kWindSpacing = 4.0f;       // yards per wind wave
constexpr float kStiffAnchor = 0.15f;      // fraction of a blade's height that never moves
constexpr float kFlutterPerYardX = 0.61f;
constexpr float kFlutterPerYardY = 0.37f;
constexpr float kMinPushRadius = 0.5f;

// The wind direction, radians. The newer module reads this from WindProfile.cdbc and blends profiles; a
// constant is the lazy version of one steady wind.
// shortcut: a fixed wind direction until a WindProfile reader exists.
constexpr float kWindAngle = 0.7f;

// The height band around an actor's own Z that its parting reaches, in yards, so a tall actor does not
// push the grass behind its feet.
constexpr float kPushHeightWindow = 3.0f;

// The lowest a doodad can be and still sway. Flatter than this is a pebble or a shell: the reference's own
// `autoFlat` test, and the reason stock decoration does not shiver.
constexpr float kMinSwayHeight = 0.15f;

// The UV band that counts as root or tip, as a fraction of the doodad's height, and the smallest
// root-to-tip UV span worth bending over.
constexpr float kBandFraction = 0.12f;
constexpr float kMinUvSpan = 0.01f;

// ---------------------------------------------------------------------------------------------
// CVars. Registered from CWorldParam::Initialize as well as from Init() below, for the same reason
// unitOutline and antiAlias are: the Physics panel asks for their defaults while the login screen is up,
// which is before any world frame exists, and GetCVarDefault raises on an unregistered name. Register
// reuses an existing var, so Init order cannot be got wrong.
// ---------------------------------------------------------------------------------------------
CVar* s_wind;
CVar* s_windSway;
CVar* s_windFlutter;
CVar* s_windSpeed;
CVar* s_parting;
CVar* s_partingStrength;
CVar* s_partingRadiusScale;

// Whether the vertex shader came out patched at all. False means every entry point below is a no-op and
// the grass is stock, which is the safe direction.
bool s_patched;

// Row 14 of the shader constant block is m_pointLights[0].m_position, and rows 14..22 are the three
// dormant point lights. Nothing on the lit path reads them; see GrassMotion.h.
constexpr uint32_t kFrameRow = 14;

// Where the actor array and the per-doodad row land inside the engine's own block, so they ride its upload.
constexpr uint32_t kActorRegister = 40;
constexpr uint32_t kDoodadRegister = 37;

// The actor array, and the gather that fills it once per frame.
float s_actors[CGrassMotion::kActorSlots][4];

// ---------------------------------------------------------------------------------------------
// d3dcompiler. Both entry points are exported by d3dcompiler_47, but D3DAssemble is not declared in the SDK
// header, so it is resolved by name.
// ---------------------------------------------------------------------------------------------
using AssembleFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, UINT, ID3DBlob**, ID3DBlob**);
using DisassembleFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, UINT, LPCSTR, ID3DBlob**);

HMODULE Compiler() {
    HMODULE module = GetModuleHandleA("d3dcompiler_47.dll");
    return module ? module : LoadLibraryA("d3dcompiler_47.dll");
}

bool Disassemble(const uint8_t* code, uint32_t length, std::string& text, std::string& error) {
    HMODULE compiler = Compiler();
    DisassembleFn disassemble = compiler ? reinterpret_cast<DisassembleFn>(GetProcAddress(compiler, "D3DDisassemble")) : nullptr;

    if (!disassemble) {
        error = "d3dcompiler_47.dll has no D3DDisassemble";
        return false;
    }

    ID3DBlob* blob = nullptr;

    if (FAILED(disassemble(code, length, 0, nullptr, &blob)) || !blob) {
        error = "disassembly failed";
        return false;
    }

    text.assign(static_cast<const char*>(blob->GetBufferPointer()));
    blob->Release();

    return true;
}

bool Assemble(const std::string& source, std::vector<uint8_t>& code, std::string& error) {
    HMODULE compiler = Compiler();
    AssembleFn assemble = compiler ? reinterpret_cast<AssembleFn>(GetProcAddress(compiler, "D3DAssemble")) : nullptr;

    if (!assemble) {
        error = "d3dcompiler_47.dll has no D3DAssemble";
        return false;
    }

    ID3DBlob* blob = nullptr;
    ID3DBlob* errors = nullptr;
    HRESULT result = assemble(source.c_str(), source.size(), "DetailDoodadGrass", nullptr, nullptr, 0, &blob, &errors);

    if (FAILED(result) || !blob) {
        error = "assembly failed: ";
        error += errors ? static_cast<const char*>(errors->GetBufferPointer()) : "(no compiler message)";

        if (errors) {
            errors->Release();
        }

        if (blob) {
            blob->Release();
        }

        return false;
    }

    if (errors) {
        errors->Release();
    }

    const uint8_t* bytes = static_cast<const uint8_t*>(blob->GetBufferPointer());
    code.assign(bytes, bytes + blob->GetBufferSize());
    blob->Release();

    return true;
}

float SecondsNow() {
    return static_cast<float>(OsGetAsyncTimeMs() % 3600000u) * 0.001f;
}

float CvarFloat(CVar* var, float fallback) {
    return var ? var->m_floatValue : fallback;
}

int32_t CvarInt(CVar* var, int32_t fallback) {
    return var ? var->m_intValue : fallback;
}

// ---------------------------------------------------------------------------------------------
// The actor gather: once per frame, nearest first, at most kActorSlots. Unused slots end up 1e5 yards away
// with a zero radius, which the shader's fixed 16-iteration loop reads as a no-op.
// ---------------------------------------------------------------------------------------------
struct ActorSlot {
    C3Vector m_position;
    float m_radius;
    float m_distanceSqr;
};

ActorSlot s_nearby[CGrassMotion::kActorSlots];
int32_t s_nearbyCount;
C3Vector s_playerPosition;
WOWGUID s_viewerGuid;

float EffectivePushRadius(CGUnit_C* unit) {
    if (!unit || !unit->m_unit) {
        return kMinPushRadius;
    }

    float radius = unit->m_unit->m_unitFieldBoundingradius;

    if (radius <= 0.0f) {
        radius = kMinPushRadius;
    }

    radius *= CvarFloat(s_partingRadiusScale, 4.0f);

    // A mount is a wider animal than what the player is riding on top of it, but its bounding radius already
    // carries most of that width; 1.35 is the rest, not a second full body.
    if (unit->IsMounted()) {
        radius *= 1.35f;
    }

    return radius < kMinPushRadius ? kMinPushRadius : radius;
}

// Keeps the nearest kActorSlots actors. A handful of units is the common case, so an insertion sort is
// shorter than anything that would avoid it.
void AddActor(const C3Vector& position, float radius, float distanceSqr) {
    int32_t at = s_nearbyCount;

    while (at > 0 && s_nearby[at - 1].m_distanceSqr > distanceSqr) {
        if (at < CGrassMotion::kActorSlots) {
            s_nearby[at] = s_nearby[at - 1];
        }

        at--;
    }

    if (at >= CGrassMotion::kActorSlots) {
        return;
    }

    s_nearby[at].m_position = position;
    s_nearby[at].m_radius = radius;
    s_nearby[at].m_distanceSqr = distanceSqr;

    if (s_nearbyCount < CGrassMotion::kActorSlots) {
        s_nearbyCount++;
    }
}

int32_t ActorEnumProc(WOWGUID guid, void* param) {
    (void)param;

    CGUnit_C* unit = static_cast<CGUnit_C*>(ClntObjMgrObjectPtr(guid, TYPE_UNIT, __FILE__, __LINE__));

    if (!unit || !unit->m_unit) {
        return 1;
    }

    // The viewer was added by hand in GatherActors, and the visible-object walk includes it. Counting it
    // twice would spend two slots and push the grass under it twice as hard.
    if (guid == s_viewerGuid) {
        return 1;
    }
    C3Vector position = unit->GetPosition();
    float dx = position.x - s_playerPosition.x;
    float dy = position.y - s_playerPosition.y;
    float dz = position.z - s_playerPosition.z;
    float distanceSqr = dx * dx + dy * dy + dz * dz;

    if (distanceSqr > CGrassMotion::kActorRange * CGrassMotion::kActorRange) {
        return 1;
    }

    AddActor(position, EffectivePushRadius(unit), distanceSqr);

    return 1;
}

void GatherActors() {
    s_viewerGuid = WOWGUID();
    s_playerPosition = C3Vector(0.0f, 0.0f, 0.0f);
    s_nearbyCount = 0;

    CGPlayer_C* player = ClntObjMgrGetActivePlayerObj();

    if (!player) {
        return;
    }

    // The unit the player actually controls: a possess or a vehicle moves the camera to another unit, and
    // that is the one whose grass should part under it.
    CGUnit_C* viewer = player->GetPossessedUnit();

    if (!viewer) {
        viewer = player;
    }

    if (!viewer->m_unit) {
        return;
    }

    s_viewerGuid = viewer->m_key.m_guid;
    s_playerPosition = viewer->GetPosition();

    AddActor(s_playerPosition, EffectivePushRadius(viewer), 0.0f);

    ClntObjMgrEnumVisibleObjects(ActorEnumProc, nullptr);
}

void BuildActorConstants() {
    for (int32_t i = 0; i < CGrassMotion::kActorSlots; i++) {
        if (i < s_nearbyCount) {
            s_actors[i][0] = s_nearby[i].m_position.x;
            s_actors[i][1] = s_nearby[i].m_position.y;
            s_actors[i][2] = s_nearby[i].m_position.z;
            s_actors[i][3] = s_nearby[i].m_radius;
        } else {
            // Far away with zero radius, which the shader's own guard makes a no-op.
            s_actors[i][0] = 1.0e5f;
            s_actors[i][1] = 1.0e5f;
            s_actors[i][2] = 1.0e5f;
            s_actors[i][3] = 0.0f;
        }
    }
}

// ---------------------------------------------------------------------------------------------
// The shader. Inserted after the stock "add r0, r0, c3" (view-space position complete in r0), with the
// def block after the version line. c0..c13 stay the engine's; everything from c14 up is this module's.
//
//   c14 = camera world xyz, time (s)
//   c15 = anchor, 1/(1-anchor), sway (yards), flutter (yards)
//   c16 = 1/spacing, 1/spacing, wind direction x, wind direction y
//   c17 = wind speed (rad/s), flutter per yard x, flutter per yard y, push strength (yards)
//   c18 = player xyz, wind on (0/1)
//   c35 = def { 0, 1, 2pi, 1/2pi }
//   c36 = def { push height window, actor range^2, 0.5, 0 }
//   c37 = { rootV, 1/(tipV-rootV), windScale, pushScale } -- per doodad, set immediately before its
//         sub-draw. This is the row the reference has to decode back out of a tagged colour instead.
//   c40..c55 = actors, 16 x { x, y, z, pushRadius }; unused slots are 1e5 away with radius 0
// ---------------------------------------------------------------------------------------------
const char* const kPrologue =
    "    def c35, 0.0, 1.0, 6.2831853, 0.15915494\n"
    "    def c36, 3.0, 2025.0, 0.5, 0.0\n"
    "    defi i0, 16, 0, 1, 0\n";

std::string BuildBody() {
    std::string s;

    // World position. A grass batch is unrotated, so c0..c2 are the pure view rotation and there is no
    // translation to undo.
    s += "    dp3 r2.x, c0, r0\n";
    s += "    dp3 r2.y, c1, r0\n";
    s += "    dp3 r2.z, c2, r0\n";
    s += "    add r2.xyz, r2, c14\n";

    // The clock, hoisted once. vs_3_0 allows a single constant register per instruction, so c14.w cannot
    // be read in the same mad that already reads c17.x -- the assembler rejects that outright (X5752) and
    // the whole permutation is left stock, which is what silently disabled this module.
    s += "    mov r8.x, c14.w\n";

    // Bend weight: 0 at the doodad's root V, 1 at its tip V, both measured from the model's own geometry
    // rather than guessed from uv.y. r13 carries the doodad's four values in full: {rootV, invSpan,
    // windScale, pushScale}. A .zw write would have put rootV in windScale's place.
    s += "    mov r13, c37\n";
    s += "    add r3.w, v3.y, -c37.x\n";
    s += "    mul_sat r3.w, r3.w, c37.y\n";

    // Anchored, squared falloff, so the base barely moves and the tip does.
    s += "    add r3.w, r3.w, -c15.x\n";
    s += "    mul_sat r3.w, r3.w, c15.y\n";
    s += "    mul r3.w, r3.w, r3.w\n";

    // The sway: one travelling wave, phase from the world position plus the clock, times the amplitude.
    s += "    mul r4.x, r2.x, c16.x\n";
    s += "    mad r4.x, r2.y, c16.y, r4.x\n";
    s += "    mad r4.x, r8.x, c17.x, r4.x\n";
    s += "    sincos r5.xy, r4.x\n";
    s += "    mul r5.x, r5.x, c15.z\n";

    // The flutter: a second wave at a per-blade phase, so a field shimmers rather than moving as one sheet.
    s += "    mul r4.y, r2.x, c17.y\n";
    s += "    mad r4.y, r2.y, c17.z, r4.y\n";
    s += "    mad r4.y, r8.x, c17.x, r4.y\n";
    s += "    sincos r6.xy, r4.y\n";
    s += "    mad r6.x, r6.x, c15.w, r5.x\n";

    // Along the wind direction, scaled by the doodad's own wind weight.
    s += "    mul r7.x, r6.x, c16.z\n";
    s += "    mul r7.y, r6.x, c16.w\n";
    s += "    mul r7.xy, r7, r13.z\n";

    // Parting. Every gathered actor within its own radius pushes blades away, but only near that actor's
    // own height. Grass farther from the player than any actor can be gathered skips the loop outright, so
    // distant blades pay nothing for it.
    s += "    mov r14.xy, c35.x\n";
    s += "    add r15.xy, r2, -c18\n";
    s += "    mul r15.z, r15.x, r15.x\n";
    s += "    mad r15.z, r15.y, r15.y, r15.z\n";
    s += "    if_lt r15.z, c36.y\n";
    s += "    loop aL, i0\n";
    s += "    add r9.xy, r2, -c40[aL]\n";
    s += "    mul r9.z, r9.x, r9.x\n";
    s += "    mad r9.z, r9.y, r9.y, r9.z\n";
    s += "    add r9.z, r9.z, c36.z\n";
    s += "    rsq r9.w, r9.z\n";
    s += "    mul r9.z, r9.z, r9.w\n";
    s += "    rcp r7.w, c40[aL].w\n";
    s += "    mul r9.z, r9.z, r7.w\n";
    s += "    add r9.z, c35.y, -r9.z\n";
    s += "    max r9.z, r9.z, c35.x\n";
    s += "    add r7.w, r2.z, -c40[aL].z\n";
    s += "    abs r7.w, r7.w\n";
    s += "    add r7.w, c36.x, -r7.w\n";
    s += "    max r7.w, r7.w, c35.x\n";
    s += "    min r7.w, r7.w, c35.y\n";
    s += "    mul r9.z, r9.z, r7.w\n";
    s += "    mul r9.z, r9.z, r9.w\n";
    s += "    mad r14.xy, r9, r9.z, r14\n";
    s += "    endloop\n";
    s += "    endif\n";
    s += "    mul r14.xy, r14, c17.w\n";
    s += "    mul r14.xy, r14, r13.w\n";
    s += "    add r7.xy, r7, r14\n";

    // Apply the bend weight, rotate the world-space offset back into view space, and add it on.
    s += "    mul r7.xy, r7, r3.w\n";
    s += "    mul r4.xyz, c0, r7.x\n";
    s += "    mad r4.xyz, c1, r7.y, r4\n";
    s += "    add r0.xyz, r0, r4\n";

    return s;
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// Endpoints
// ---------------------------------------------------------------------------------------------

// The cvars. Registering them here as well as in CWorldParam::Initialize is deliberate: the panel asks for
// their defaults at the login screen, before a world frame exists.
void CGrassMotion::Init() {
    s_wind = CVar::Register("grassWind", "Grass and clutter sway in the wind", 1, "1", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
    // Defaults are a percentage of each slider's range (min is 0 for all three), so the thumb opens where the
    // tuning wants it rather than pinned to an end.
    s_windSway = CVar::Register("grassWindSway", "Wind sway at the tip of a blade, in yards", 1, "0.15", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
    s_windFlutter = CVar::Register("grassWindFlutter", "Extra per-blade shimmer, in yards", 1, "0.025", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
    s_windSpeed = CVar::Register("grassWindSpeed", "Wind speed, radians per second", 1, "1.0", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
    s_parting = CVar::Register("grassParting", "Grass parts around nearby characters", 1, "1", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
    // Halfway up the panel's 0..2 and 0..8 sliders, and the step is fine enough that the first notch no
    // longer arrives at full strength on its own.
    // shortcut: the radius default (4.0) is numerically the midpoint but reads as a very wide push, because
    // it multiplies the bounding radius and then a mount's 1.35 on top; lower it if the field looks combed.
    s_partingStrength = CVar::Register("grassPartingStrength", "How far blades lean away from a character, in yards", 1, "1.2", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
    s_partingRadiusScale = CVar::Register("grassPartingRadiusScale", "Push radius as a multiple of a character's bounding radius", 1, "4.0", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
}

bool CGrassMotion::Active() {
    return s_patched;
}

// Once per frame, from CDetailDoodad::InitializeShaderConstants, which has just zeroed the block this
// writes into. The rows are the dormant point-light rows, so the engine's own per-chunk upload carries them
// and no separate upload is needed for c14..c18.
void CGrassMotion::BeginFrame() {
    if (!s_patched) {
        return;
    }

    GatherActors();
    BuildActorConstants();

    float time = SecondsNow();
    const C3Vector& camera = CWorldScene::s_activeWorldView;

    bool wind = CvarInt(s_wind, 1) != 0;
    bool parting = CvarInt(s_parting, 1) != 0;

    float* row = reinterpret_cast<float*>(&CDetailDoodad::s_shaderConstants) + kFrameRow * 4;

    // c14 = camera world xyz, time.
    row[0] = camera.x;
    row[1] = camera.y;
    row[2] = camera.z;
    row[3] = time;
    row += 4;

    // c15 = anchor, 1/(1-anchor), sway, flutter.
    row[0] = kStiffAnchor;
    row[1] = 1.0f / (1.0f - kStiffAnchor);
    row[2] = wind ? CvarFloat(s_windSway, 0.15f) : 0.0f;
    row[3] = wind ? CvarFloat(s_windFlutter, 0.025f) : 0.0f;
    row += 4;

    // c16 = 1/spacing, 1/spacing, wind direction x, wind direction y.
    row[0] = 1.0f / kWindSpacing;
    row[1] = 1.0f / kWindSpacing;
    row[2] = cosf(kWindAngle);
    row[3] = sinf(kWindAngle);
    row += 4;

    // c17 = wind speed, flutter per yard x, flutter per yard y, push strength.
    row[0] = CvarFloat(s_windSpeed, 1.0f);
    row[1] = kFlutterPerYardX;
    row[2] = kFlutterPerYardY;
    row[3] = parting ? CvarFloat(s_partingStrength, 1.2f) : 0.0f;
    row += 4;

    // c18 = player xyz, wind on.
    row[0] = s_playerPosition.x;
    row[1] = s_playerPosition.y;
    row[2] = s_playerPosition.z;
    row[3] = wind ? 1.0f : 0.0f;
}

// Per chunk, right after the engine's own c0..c22 upload in CDetailDoodad::SetupVertexShader, so nothing
// drawn between chunks can clobber the actor array.
void CGrassMotion::UploadChunkConstants() {
    if (!s_patched) {
        return;
    }

    GxShaderConstantsSet(GxSh_Vertex, kActorRegister, &s_actors[0][0], kActorSlots);
}

// One doodad's row for c37, immediately before that doodad's sub-draw.
void CGrassMotion::DoodadRow(CDetailDoodadData* data, float& rootV, float& invRange, float& windScale, float& pushScale) {
    AnalyzeDoodad(data);

    rootV = data->m_grassRootV;
    invRange = data->m_grassInvSpan;
    windScale = data->m_grassSways ? 1.0f : 0.0f;
    pushScale = data->m_grassSways ? 1.0f : 0.0f;
}

// The c37 upload. This is what replaces the reference module's colour tagging: the batch is baked sorted by
// doodad, so the caller just binds the right row before each sub-draw and the shader needs no lookup.
void CGrassMotion::UploadDoodadRow(CDetailDoodadData* data) {
    if (!s_patched) {
        return;
    }

    float rootV = 0.0f;
    float invRange = 1.0f;
    float windScale = 0.0f;
    float pushScale = 0.0f;

    DoodadRow(data, rootV, invRange, windScale, pushScale);

    float row[4] = { rootV, invRange, windScale, pushScale };
    GxShaderConstantsSet(GxSh_Vertex, kDoodadRegister, row, 1);
}

// The reference's Analyze/ScanModel, using this tree's own structs rather than client addresses. These
// doodad models are atlas-packed and their UV layout varies per model, so a per-vertex `1 - uv.y` is wrong
// for every doodad whose UVs run the other way; the band has to come from the vertices the skin uses.
void CGrassMotion::AnalyzeDoodad(CDetailDoodadData* data) {
    if (!data || data->m_grassAnalyzed) {
        return;
    }

    CM2Model* model = data->m_model;

    if (!model) {
        return;
    }

    if (!(model->m_fFlags & 1)) {
        model->WaitForLoad(nullptr);
    }

    if (!model->m_shared || !model->m_shared->m_data || !model->m_shared->m_skinData) {
        // Not loaded yet: stay unanalysed and try again on the next bake, rather than caching a wrong
        // answer for the session.
        return;
    }

    data->m_grassAnalyzed = 1;
    data->m_grassSways = 0;
    data->m_grassRootV = 0.0f;
    data->m_grassInvSpan = 1.0f;
    M2SkinProfile* skin = model->m_shared->m_skinData;

    if (!skin->m_vertices.m_count) {
        return;
    }

    float minZ = model->m_shared->Vertices()[skin->m_vertices[0]].m_position.z;
    float maxZ = minZ;

    for (uint32_t i = 1; i < skin->m_vertices.m_count; i++) {
        float z = model->m_shared->Vertices()[skin->m_vertices[i]].m_position.z;

        if (z < minZ) {
            minZ = z;
        }

        if (z > maxZ) {
            maxZ = z;
        }
    }

    float height = maxZ - minZ;

    if (height < kMinSwayHeight) {
        // A pebble, a shell or a flower dot: too flat to bend over anything.
        return;
    }

    float rootBand = minZ + height * kBandFraction;
    float tipBand = maxZ - height * kBandFraction;

    float rootSum = 0.0f;
    float tipSum = 0.0f;
    uint32_t rootCount = 0;
    uint32_t tipCount = 0;

    for (uint32_t i = 0; i < skin->m_vertices.m_count; i++) {
        const M2Vertex& source = model->m_shared->Vertices()[skin->m_vertices[i]];

        if (source.m_position.z <= rootBand) {
            rootSum += source.m_texcoord[0].y;
            rootCount++;
        } else if (source.m_position.z >= tipBand) {
            tipSum += source.m_texcoord[0].y;
            tipCount++;
        }
    }

    if (!rootCount || !tipCount) {
        return;
    }

    float rootV = rootSum / static_cast<float>(rootCount);
    float tipV = tipSum / static_cast<float>(tipCount);

    float span = tipV - rootV;

    if (fabsf(span) < kMinUvSpan) {
        // The UVs do not run up the blade, so there is no bend this could honestly describe.
        return;
    }

    data->m_grassRootV = rootV;
    data->m_grassInvSpan = 1.0f / span;
    data->m_grassSways = 1;

    // One line per doodad, as the reference logs it: a band or a length that is wrong shows up here rather
    // than as grass that quietly does not move.
    CGxDevice::Log("grass wind: doodad height %.3f, root v %.3f, tip v %.3f", height, rootV, tipV);
}


// Insert the def block after the version line and the body after the anchor, then reassemble. A shader that
// misses either line, or that fails to assemble, is left exactly stock: a half-patched shader is a crash or
// silent corruption, where stock grass is a visible and diagnosable outcome.
void CGrassMotion::PatchVertexShader(CGxShader* shader, uint32_t permutation) {
    // D3d11 only. The former implementation disassembled the vs_3_0 bytecode, inserted a prologue and
    // body, reassembled it and wrote it back. There is no vs_3_0 shader to patch any more: the D3d11
    // backend loads the vs_4_0/vs_5_0 DXBC permutations out of the same .bls sets, and those are compiled
    // DXBC with no assembler round-trip, so this module cannot rewrite them.
    //
    // The wind displacement is therefore not applied in the vertex shader. Everything else this module
    // does (the per-frame constants in c14..c22, the sway data) still runs; only this last rewrite cannot.
    // Reported once so the behaviour is diagnosable from the log rather than silent.
    static bool s_reported;

    if (!s_reported && shader) {
        s_reported = true;
        CGxDevice::Log("grass wind: vertex-shader patching is not available on D3d11 (permutation %u left stock)", permutation);
    }
}
