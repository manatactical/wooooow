// Not in the original binary. Port of wxl-equip-plus (https://github.com/TheDaveCalaz/wxl-equip-plus, GPL-3.0-or-later),
// itself a continuation of Morfium-G/wxl-equip-extension and Atraxian123/WXL-Equip-Creature-Extension-CSV-WXL11.
//
// The original is a WarcraftXL extension: it hooks the 32-bit client's functions and, to change what a model looks like,
// rewrites the model's bytes on their way out of the archive. Inside the client's own source none of that is needed. The
// model API already has per-geoset visibility, texture replacement by type and attachment, so this file only decides
// WHAT to attach (the same rules as the extension) and attaches it. The one thing the model API lacks, a model whose
// bones follow another model's, is M2BoneFollow in the Model2 module.

#include "WoW/Common/ComponentCore/EquipPlus.h"

#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include <windows.h>

#include "Engine/Source/Base/Handle.h"
#include "Engine/Source/Base/Status.h"
#include "Engine/Source/BlizzardCore/SFile2-Core.h"
#include "Engine/Source/ConsoleBase/ConsoleCommand.h"
#include "Engine/Source/ConsoleBase/ConsoleVar.h"
#include "Engine/Source/ConsoleBase/IConsole.h"
#include "Engine/Source/Gx/CGxTexFlags.h"
#include "Engine/Source/ConsoleBase/Types.h"
#include "Engine/Source/Model2/M2BoneFollow.h"
#include "Engine/Source/Model2/M2Model.h"
#include "Engine/Source/Model2/M2Scene.h"
#include "Engine/Source/Model2/Types.h"
#include "Engine/Source/Services/Texture.h"
#include "Engine/Source/Storm/H/storm.h"
#include "WoW/Common/ComponentCore/CharacterComponent.h"
#include "WoW/Source/DB/DBClient/DBClient.h"
#include "WoW/Source/DB/Rec/ChrRacesRec.h"
#include "WoW/Source/DB/Rec/CreatureDisplayInfoRec.h"
#include "WoW/Source/DB/Rec/ItemDisplayInfoRec.h"

// The slot an attachment hangs from when a collection piece is skinned to the whole body. Not a real attachment of any
// model: the follow draws the piece from the parent's root, so it only has to be an id nothing else uses.
static const uint32_t kRootAttach = 0xFFFF;

// The extension called this attachment 19 (Ground_Base) and used it as the marker for "skinned to the body".
static const uint32_t kCollectionAttach = 19;

static const uint32_t kNone = 0xFFFFFFFF;
static const uint32_t kFlagForceModelRaceGender = 0x80;
static const char* kFallbackTexture = "createcrappygreentexture.blp";

namespace {

// ---- data ---------------------------------------------------------------------------------------------------------

struct GeosetFilter {
    uint16_t ids[16];
    uint32_t count;
};

// One row of WXLItemDisplayModels.csv.
struct ModelRow {
    uint32_t slot = kNone;
    uint32_t attach = kNone;
    uint32_t modelFlags = 0x2 | 0x4;
    uint32_t textureFlags = 0;
    char folder[32] = {};
    char model[264] = {};
    char texture[264] = {};
    GeosetFilter geo = {};
};

// One row of WXLItemDisplayModelMaterials.csv.
struct MaterialRow {
    uint32_t modelIndex = kNone;
    uint32_t modelColumn = kNone;
    uint32_t textureType = kNone;
    char folder[32] = {};
    char model[264] = {};
    char texture[264] = {};
    char targetMode[32] = {};
};

// One row of WXLHelmOffsets.csv.
struct OffsetRule {
    char model[128] = {};
    char raceSex[8] = {};
    float x = 0.0f, y = 0.0f, z = 0.0f, scale = 0.0f;
    bool hasX = false, hasY = false, hasZ = false, hasScale = false;
    bool add = true;
    bool disable = false;
    bool isHelm = false;
};

// What the planner decides to attach.
struct Planned {
    char path[264] = {};
    char texture[264] = {};
    uint32_t attach = 0;
    GeosetFilter geo = {};
    std::vector<std::pair<uint32_t, std::string>> materials;
    bool collection = false;
    bool rootSkinned = false;
    bool animSync = false;
    uint32_t slot = 0;
};

struct SlotState {
    uint32_t displayId = 0;
    uint32_t raceSex = 0;
    CM2Model* parent = nullptr;
    uint64_t tag = 0;
    std::vector<CM2Model*> models;
};

struct SlotConfig {
    const char* folder;
    uint32_t attach1;
    uint32_t attach2;
    bool useRace;
    bool useGender;
};

// Indexed by ITEM_SLOT.
const SlotConfig kSlotConfig[11] = {
    {"Head", 11, 55, true, true},
    {"Shoulder", 6, 5, false, false},
    {"Shirt", 34, 34, false, false},
    {"Chest", 34, 34, false, false},
    {"Waist", 53, 53, false, false},
    {"Leg", 9, 10, false, false},
    {"Foot", 47, 48, false, false},
    {"Bracer", 3, 4, false, false},
    {"Glove", 1, 2, false, false},
    {"Tabard", 34, 34, false, false},
    {"Cape", 12, 12, false, false},
};

CVar* s_cvarEnable;
CVar* s_cvarPivots;
CVar* s_cvarScale;
CVar* s_cvarLog;

// One row of WXLWeaponModels.csv: the weapon a display draws instead of the one the client would pick.
struct WeaponRow {
    std::string path[2];
    std::string offhandPath[2];
    GeosetFilter geo[2] = {};
    GeosetFilter offhandGeo[2] = {};
};

struct TypedTexture {
    uint32_t column = 0;
    uint32_t type = 0;
    std::string path;
};

// One row of WXLCreatureModels.csv.
struct CreatureRow {
    std::string path;
    GeosetFilter geo = {};
};

bool s_loaded;
std::unordered_map<uint32_t, WeaponRow> s_weapons;
std::unordered_map<uint32_t, std::vector<TypedTexture>> s_weaponTextures;
std::unordered_map<uint32_t, CreatureRow> s_creatures;
std::unordered_map<uint32_t, std::vector<TypedTexture>> s_creatureTextures;
std::unordered_map<uint32_t, std::vector<ModelRow>> s_models;
std::unordered_map<uint32_t, std::vector<MaterialRow>> s_materials;
std::unordered_map<uint32_t, bool> s_anim;
std::vector<OffsetRule> s_offsets;
std::unordered_map<std::string, bool> s_exists;
uint64_t s_nextTag = 1;

// ---- log ----------------------------------------------------------------------------------------------------------

void Log(const char* format, ...) {
    if (!s_cvarLog || s_cvarLog->m_intValue <= 0) {
        return;
    }

    FILE* file = fopen("EquipPlus.log", "a");

    if (!file) {
        return;
    }

    va_list args;
    va_start(args, format);
    vfprintf(file, format, args);
    va_end(args);
    fputc('\n', file);
    fclose(file);
}

// ---- strings ------------------------------------------------------------------------------------------------------

bool StartsWithCI(const char* s, const char* prefix) {
    if (!s || !prefix) {
        return false;
    }

    while (*prefix) {
        if (tolower(static_cast<unsigned char>(*s++)) != tolower(static_cast<unsigned char>(*prefix++))) {
            return false;
        }
    }

    return true;
}

bool ContainsCI(const char* s, const char* needle) {
    if (!s || !needle || !*needle) {
        return false;
    }

    for (const char* p = s; *p; p++) {
        const char* a = p;
        const char* b = needle;

        while (*a && *b && tolower(static_cast<unsigned char>(*a)) == tolower(static_cast<unsigned char>(*b))) {
            a++;
            b++;
        }

        if (!*b) {
            return true;
        }
    }

    return false;
}

void CopyString(char* out, size_t size, const char* value) {
    if (!out || !size) {
        return;
    }

    out[0] = '\0';

    if (value) {
        strncpy(out, value, size - 1);
        out[size - 1] = '\0';
    }
}

std::string Trim(const std::string& value) {
    size_t first = 0;

    while (first < value.size() && (value[first] == ' ' || value[first] == '\t')) {
        first++;
    }

    size_t last = value.size();

    while (last > first && (value[last - 1] == ' ' || value[last - 1] == '\t' || value[last - 1] == '\r' || value[last - 1] == '\n')) {
        last--;
    }

    return value.substr(first, last - first);
}

std::string NormalizeName(const std::string& value) {
    std::string out;

    for (char c : value) {
        c = static_cast<char>(tolower(static_cast<unsigned char>(c)));

        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            out.push_back(c);
        }
    }

    return out;
}

std::vector<std::string> ParseCsvLine(const std::string& line) {
    std::vector<std::string> fields;
    std::string field;
    bool quoted = false;

    for (size_t i = 0; i < line.size(); i++) {
        char c = line[i];

        if (quoted) {
            if (c == '"') {
                if (i + 1 < line.size() && line[i + 1] == '"') {
                    field.push_back('"');
                    i++;
                } else {
                    quoted = false;
                }
            } else {
                field.push_back(c);
            }
        } else if (c == '"') {
            quoted = true;
        } else if (c == ',') {
            fields.push_back(Trim(field));
            field.clear();
        } else if (c != '\r' && c != '\n') {
            field.push_back(c);
        }
    }

    fields.push_back(Trim(field));

    return fields;
}

int FindColumn(const std::vector<std::string>& header, const char* name) {
    const std::string wanted = NormalizeName(name);

    for (size_t i = 0; i < header.size(); i++) {
        if (NormalizeName(header[i]) == wanted) {
            return static_cast<int>(i);
        }
    }

    return -1;
}

const char* Field(const std::vector<std::string>& row, int column) {
    if (column < 0 || static_cast<size_t>(column) >= row.size()) {
        return "";
    }

    return row[column].c_str();
}

bool ParseU32(const char* text, uint32_t* out) {
    if (!text || !*text) {
        return false;
    }

    char* end = nullptr;
    unsigned long value = strtoul(text, &end, 10);

    if (end == text) {
        return false;
    }

    *out = static_cast<uint32_t>(value);

    return true;
}

GeosetFilter ParseGeosets(const char* spec) {
    GeosetFilter filter = {};

    if (!spec) {
        return filter;
    }

    const char* p = spec;

    while (*p && filter.count < 16) {
        uint32_t value = 0;

        while (*p >= '0' && *p <= '9') {
            value = value * 10 + static_cast<uint32_t>(*p++ - '0');
        }

        filter.ids[filter.count++] = static_cast<uint16_t>(value);

        if (*p == ',') {
            p++;
        } else {
            break;
        }
    }

    return filter;
}

// "a;b;c" -> the index-th part, trimmed.
bool ListPart(const char* list, uint32_t index, char* out, size_t size) {
    out[0] = '\0';

    if (!list) {
        return false;
    }

    const char* part = list;

    for (uint32_t current = 0;; current++) {
        const char* end = part;

        while (*end && *end != ';') {
            end++;
        }

        if (current == index) {
            while (part < end && (*part == ' ' || *part == '\t')) {
                part++;
            }

            while (end > part && (end[-1] == ' ' || end[-1] == '\t')) {
                end--;
            }

            size_t length = static_cast<size_t>(end - part);

            if (length >= size) {
                length = size - 1;
            }

            memcpy(out, part, length);
            out[length] = '\0';

            return true;
        }

        if (!*end) {
            break;
        }

        part = end + 1;
    }

    return false;
}

// ---- sidecar parsing ----------------------------------------------------------------------------------------------

uint32_t ParseModelColumn(const char* value) {
    if (!value || !*value) {
        return kNone;
    }

    uint32_t number = 0;

    if (ParseU32(value, &number)) {
        if (number == 0 || number == 1) {
            return number;
        }

        return number == 2 ? 1 : kNone;
    }

    if (ContainsCI(value, "ModelName_1") || ContainsCI(value, "ModelTexture_1") || ContainsCI(value, "Model_1") || ContainsCI(value, "Texture_1")) {
        return 0;
    }

    if (ContainsCI(value, "ModelName_2") || ContainsCI(value, "ModelTexture_2") || ContainsCI(value, "Model_2") || ContainsCI(value, "Texture_2")) {
        return 1;
    }

    return kNone;
}

uint32_t ParseSlot(const char* slot) {
    if (!slot || !*slot) {
        return kNone;
    }

    uint32_t number = 0;

    if (ParseU32(slot, &number) && number < 14) {
        return number;
    }

    if (StartsWithCI(slot, "Head")) return 0;
    if (StartsWithCI(slot, "Shoulder")) return 1;
    if (StartsWithCI(slot, "Shirt")) return 2;
    if (StartsWithCI(slot, "Chest") || StartsWithCI(slot, "Robe")) return 3;
    if (StartsWithCI(slot, "Waist") || StartsWithCI(slot, "Belt")) return 4;
    if (StartsWithCI(slot, "Leg") || StartsWithCI(slot, "Pant")) return 5;
    if (StartsWithCI(slot, "Foot") || StartsWithCI(slot, "Feet") || StartsWithCI(slot, "Boot")) return 6;
    if (StartsWithCI(slot, "Bracer") || StartsWithCI(slot, "Wrist")) return 7;
    if (StartsWithCI(slot, "Glove") || StartsWithCI(slot, "Hand")) return 8;
    if (StartsWithCI(slot, "Cape") || StartsWithCI(slot, "Back") || StartsWithCI(slot, "Cloak")) return 10;
    if (StartsWithCI(slot, "Tabard")) return 9;

    // The weapon names exist so a row meant for a weapon is not read as an armour row.
    if (StartsWithCI(slot, "MainHand") || StartsWithCI(slot, "Main")) return 15;
    if (StartsWithCI(slot, "OffHand") || StartsWithCI(slot, "Off") || StartsWithCI(slot, "Shield")) return 16;
    if (StartsWithCI(slot, "Ranged") || StartsWithCI(slot, "Bow") || StartsWithCI(slot, "Gun") || StartsWithCI(slot, "Quiver") || StartsWithCI(slot, "Ammo")) return 17;

    return kNone;
}

// 0xFFFFFFFF means "use what the DBC row says".
uint32_t ModelFlagsForPolicy(const char* policy) {
    if (policy && (StartsWithCI(policy, "DBC") || StartsWithCI(policy, "Slot"))) return 0xFFFFFFFF;
    if (!policy || !*policy || StartsWithCI(policy, "Exact") || StartsWithCI(policy, "None")) return 0x2 | 0x4;
    if (StartsWithCI(policy, "Retail") || StartsWithCI(policy, "New")) return 0x40;
    if (StartsWithCI(policy, "Legacy") || StartsWithCI(policy, "Race")) return 0;

    return 0x2 | 0x4;
}

uint32_t TextureFlagsForPolicy(const char* policy) {
    if (policy && (StartsWithCI(policy, "DBC") || StartsWithCI(policy, "Slot"))) return 0xFFFFFFFF;
    if (!policy || !*policy || StartsWithCI(policy, "Exact") || StartsWithCI(policy, "None")) return 0;
    if (StartsWithCI(policy, "Retail") || StartsWithCI(policy, "New")) return 0x8 | 0x10 | 0x40;
    if (StartsWithCI(policy, "Legacy") || StartsWithCI(policy, "Race")) return 0x8 | 0x10;

    return 0;
}

std::vector<std::string> SplitLines(const std::string& text) {
    std::vector<std::string> lines;
    size_t start = 0;

    for (size_t i = 0; i <= text.size(); i++) {
        if (i != text.size() && text[i] != '\n') {
            continue;
        }

        std::string line = text.substr(start, i - start);

        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        lines.push_back(line);
        start = i + 1;
    }

    return lines;
}

void LoadModelCsv(const std::string& text) {
    const std::vector<std::string> lines = SplitLines(text);

    if (lines.empty()) {
        return;
    }

    const std::vector<std::string> header = ParseCsvLine(lines[0]);
    const int cDisplay = FindColumn(header, "DisplayID");
    const int cSlot = FindColumn(header, "Slot");
    int cModel = FindColumn(header, "Model");

    if (cModel < 0) cModel = FindColumn(header, "ModelStem");
    if (cModel < 0) cModel = FindColumn(header, "ModelName");

    int cTexture = FindColumn(header, "Texture");

    if (cTexture < 0) cTexture = FindColumn(header, "TextureStem");
    if (cTexture < 0) cTexture = FindColumn(header, "ModelTexture");

    const int cFolder = FindColumn(header, "Folder");
    const int cGeosets = FindColumn(header, "Geosets");
    const int cAttach = FindColumn(header, "Attach");
    const int cSuffix = FindColumn(header, "SuffixPolicy");
    const int cTexturePolicy = FindColumn(header, "TexturePolicy");
    const int cFlags = FindColumn(header, "Flags");

    if (cDisplay < 0 || cModel < 0) {
        return;
    }

    for (size_t n = 1; n < lines.size(); n++) {
        if (lines[n].empty()) {
            continue;
        }

        const std::vector<std::string> row = ParseCsvLine(lines[n]);
        uint32_t displayId = 0;

        if (!ParseU32(Field(row, cDisplay), &displayId) || !displayId) {
            continue;
        }

        ModelRow e;
        e.slot = ParseSlot(Field(row, cSlot));
        e.modelFlags = ModelFlagsForPolicy(Field(row, cSuffix));
        e.textureFlags = TextureFlagsForPolicy(Field(row, cTexturePolicy));

        uint32_t attach = kNone;

        if (ParseU32(Field(row, cAttach), &attach)) {
            e.attach = attach;
        }

        uint32_t extra = 0;

        if (ParseU32(Field(row, cFlags), &extra)) {
            e.modelFlags |= extra;
        }

        CopyString(e.folder, sizeof(e.folder), Field(row, cFolder));
        CopyString(e.model, sizeof(e.model), Field(row, cModel));
        CopyString(e.texture, sizeof(e.texture), Field(row, cTexture));

        char* colon = strchr(e.model, ':');

        if (colon) {
            *colon = '\0';
            e.geo = ParseGeosets(colon + 1);
        }

        const char* geosets = Field(row, cGeosets);

        if (*geosets) {
            e.geo = ParseGeosets(geosets);
        }

        if (!e.model[0]) {
            continue;
        }

        // The same CSV is reachable under two names and a patch folder can repeat it; an identical row adds nothing.
        std::vector<ModelRow>& list = s_models[displayId];
        bool duplicate = false;

        for (const ModelRow& o : list) {
            if (o.slot == e.slot && o.attach == e.attach && o.modelFlags == e.modelFlags && o.textureFlags == e.textureFlags && o.geo.count == e.geo.count && !memcmp(o.geo.ids, e.geo.ids, sizeof(e.geo.ids)) && !_stricmp(o.folder, e.folder) && !_stricmp(o.model, e.model) && !_stricmp(o.texture, e.texture)) {
                duplicate = true;
                break;
            }
        }

        if (!duplicate) {
            list.push_back(e);
        }
    }
}

void LoadMaterialCsv(const std::string& text) {
    const std::vector<std::string> lines = SplitLines(text);

    if (lines.empty()) {
        return;
    }

    const std::vector<std::string> header = ParseCsvLine(lines[0]);
    const int cDisplay = FindColumn(header, "DisplayID");
    const int cModelIndex = FindColumn(header, "ModelIndex");
    const int cModelColumn = FindColumn(header, "ModelColumn");
    const int cModel = FindColumn(header, "Model");
    const int cTextureType = FindColumn(header, "TextureType");
    const int cFolder = FindColumn(header, "Folder");
    const int cTexture = FindColumn(header, "Texture");
    const int cTargetMode = FindColumn(header, "TargetMode");

    if (cDisplay < 0 || cTexture < 0) {
        return;
    }

    for (size_t n = 1; n < lines.size(); n++) {
        if (lines[n].empty()) {
            continue;
        }

        const std::vector<std::string> row = ParseCsvLine(lines[n]);
        uint32_t displayId = 0;

        if (!ParseU32(Field(row, cDisplay), &displayId) || !displayId) {
            continue;
        }

        MaterialRow e;
        ParseU32(Field(row, cModelIndex), &e.modelIndex);
        e.modelColumn = ParseModelColumn(Field(row, cModelColumn));

        if (e.modelColumn == kNone) {
            e.modelColumn = ParseModelColumn(Field(row, cModelIndex));
        }

        if (cTextureType >= 0) {
            ParseU32(Field(row, cTextureType), &e.textureType);
        }

        CopyString(e.folder, sizeof(e.folder), Field(row, cFolder));
        CopyString(e.model, sizeof(e.model), Field(row, cModel));
        CopyString(e.texture, sizeof(e.texture), Field(row, cTexture));
        CopyString(e.targetMode, sizeof(e.targetMode), Field(row, cTargetMode));

        if (e.texture[0]) {
            s_materials[displayId].push_back(e);
        }
    }
}

void LoadAnimationCsv(const std::string& text) {
    for (const std::string& line : SplitLines(text)) {
        const size_t comma = line.find(',');

        if (comma == std::string::npos) {
            continue;
        }

        std::string id = Trim(line.substr(0, comma));
        std::string value = line.substr(comma + 1);
        const size_t next = value.find(',');

        if (next != std::string::npos) {
            value = value.substr(0, next);
        }

        value = Trim(value);

        // A non-numeric first field is a header or a comment.
        if (id.empty() || value.empty() || !isdigit(static_cast<unsigned char>(id[0]))) {
            continue;
        }

        s_anim[static_cast<uint32_t>(strtoul(id.c_str(), nullptr, 10))] = value[0] == '1';
    }
}

std::string NormalizeOffsetKey(const std::string& in) {
    std::string out;

    for (char c : in) {
        c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
        out.push_back(c == '/' ? '\\' : c);
    }

    if (out.size() >= 3 && !out.compare(out.size() - 3, 3, ".m2")) {
        out.resize(out.size() - 3);
    } else if (out.size() >= 4 && !out.compare(out.size() - 4, 4, ".mdx")) {
        out.resize(out.size() - 4);
    }

    return out;
}

bool IsRaceCode(const char* race) {
    static const char* codes[] = {"be", "dr", "dw", "gn", "hu", "ni", "or", "sc", "ta", "tr", "sk", "go"};

    for (const char* code : codes) {
        if (!strcmp(race, code)) {
            return true;
        }
    }

    return false;
}

// Strips a trailing "_<race><m|f>" or "_<race>_<m|f>" from a model stem.
void StripRaceGender(std::string& stem) {
    const size_t n = stem.size();

    auto lower = [](char c) { return static_cast<char>(tolower(static_cast<unsigned char>(c))); };

    if (n >= 5 && stem[n - 5] == '_' && stem[n - 2] == '_' && (stem[n - 1] == 'm' || stem[n - 1] == 'f')) {
        const char race[3] = {lower(stem[n - 4]), lower(stem[n - 3]), '\0'};

        if (IsRaceCode(race)) {
            stem.resize(n - 5);
            return;
        }
    }

    if (n >= 4 && stem[n - 4] == '_' && (stem[n - 1] == 'm' || stem[n - 1] == 'f')) {
        const char race[3] = {lower(stem[n - 3]), lower(stem[n - 2]), '\0'};

        if (IsRaceCode(race)) {
            stem.resize(n - 4);
        }
    }
}

// The trailing "<race><m|f>" of a model name, as three lowercase characters, or false.
bool ModelRaceId(const char* name, char* id) {
    const char* base = name;

    for (const char* p = name; *p; p++) {
        if (*p == '\\' || *p == '/') {
            base = p + 1;
        }
    }

    const char* dot = nullptr;

    for (const char* p = base; *p; p++) {
        if (*p == '.') {
            dot = p;
        }
    }

    const char* end = dot ? dot : base + strlen(base);

    if (end - base < 3) {
        return false;
    }

    char race[3] = {};
    char gender;

    if (end - base >= 4 && end[-2] == '_') {
        race[0] = end[-4];
        race[1] = end[-3];
    } else {
        race[0] = end[-3];
        race[1] = end[-2];
    }

    gender = end[-1];
    race[0] = static_cast<char>(tolower(static_cast<unsigned char>(race[0])));
    race[1] = static_cast<char>(tolower(static_cast<unsigned char>(race[1])));
    gender = static_cast<char>(tolower(static_cast<unsigned char>(gender)));

    if ((gender != 'm' && gender != 'f') || !IsRaceCode(race)) {
        return false;
    }

    id[0] = race[0];
    id[1] = race[1];
    id[2] = gender;
    id[3] = '\0';

    return true;
}

void LoadOffsetCsv(const std::string& text) {
    const std::vector<std::string> lines = SplitLines(text);

    if (lines.empty()) {
        return;
    }

    const std::vector<std::string> header = ParseCsvLine(lines[0]);
    const int cModel = FindColumn(header, "Model");
    const int cRaceSex = FindColumn(header, "RaceSex");
    const int cMode = FindColumn(header, "Mode");
    const int cKind = FindColumn(header, "Kind");
    const int cX = FindColumn(header, "X");
    const int cY = FindColumn(header, "Y");
    const int cZ = FindColumn(header, "Z");
    const int cScale = FindColumn(header, "Scale");

    if (cModel < 0) {
        return;
    }

    auto lowered = [](std::string s) {
        for (char& c : s) {
            c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
        }

        return s;
    };

    auto parseFloat = [](const char* text, float& out) {
        if (!*text) {
            return false;
        }

        char* end = nullptr;
        float value = strtof(text, &end);

        if (end == text) {
            return false;
        }

        out = value;

        return true;
    };

    for (size_t n = 1; n < lines.size(); n++) {
        if (lines[n].empty()) {
            continue;
        }

        const std::vector<std::string> row = ParseCsvLine(lines[n]);
        OffsetRule rule;
        const std::string key = NormalizeOffsetKey(Field(row, cModel));

        if (key.empty()) {
            continue;
        }

        CopyString(rule.model, sizeof(rule.model), key.c_str());

        std::string raceSex = lowered(Field(row, cRaceSex));

        if (raceSex == "all") {
            raceSex = "*";
        }

        CopyString(rule.raceSex, sizeof(rule.raceSex), raceSex.c_str());

        const std::string mode = lowered(Field(row, cMode));
        rule.disable = mode == "disable" || mode == "disabled" || mode == "none" || mode == "skip";
        rule.add = !(mode == "set" || mode == "absolute" || mode == "override");
        rule.isHelm = lowered(Field(row, cKind)) == "helm";

        rule.hasX = parseFloat(Field(row, cX), rule.x);
        rule.hasY = parseFloat(Field(row, cY), rule.y);
        rule.hasZ = parseFloat(Field(row, cZ), rule.z);
        rule.hasScale = parseFloat(Field(row, cScale), rule.scale);

        if (!rule.disable && !rule.hasX && !rule.hasY && !rule.hasZ && !rule.hasScale && !rule.isHelm) {
            continue;
        }

        s_offsets.push_back(rule);
    }
}

// A Geoset cell of the weapon and creature sheets: empty means every geoset; otherwise only the listed ones draw, and the
// base geoset (0) is always among them.
GeosetFilter ParseImpliedBaseGeosets(const char* spec) {
    GeosetFilter filter = {};

    if (!spec || !*spec) {
        return filter;
    }

    filter.ids[filter.count++] = 0;
    const char* p = spec;

    while (*p && filter.count < 16) {
        while (*p == ' ' || *p == '\t' || *p == ',') {
            p++;
        }

        if (*p < '0' || *p > '9') {
            break;
        }

        uint32_t value = 0;

        while (*p >= '0' && *p <= '9') {
            value = value * 10 + static_cast<uint32_t>(*p++ - '0');
        }

        bool duplicate = false;

        for (uint32_t i = 0; i < filter.count; i++) {
            if (filter.ids[i] == static_cast<uint16_t>(value)) {
                duplicate = true;
            }
        }

        if (!duplicate) {
            filter.ids[filter.count++] = static_cast<uint16_t>(value);
        }
    }

    return filter;
}

void LoadWeaponModelCsv(const std::string& text) {
    const std::vector<std::string> lines = SplitLines(text);

    if (lines.empty()) {
        return;
    }

    const std::vector<std::string> header = ParseCsvLine(lines[0]);
    const int cDisplay = FindColumn(header, "DisplayID");
    int cModel1 = FindColumn(header, "Model1Path");

    if (cModel1 < 0) cModel1 = FindColumn(header, "ModelPath");

    const int cModel2 = FindColumn(header, "Model2Path");
    const int cGeoset1 = FindColumn(header, "Geoset1");
    const int cGeoset2 = FindColumn(header, "Geoset2");
    const int cOffhand1 = FindColumn(header, "Model1OffhandPath");
    const int cOffhand2 = FindColumn(header, "Model2OffhandPath");
    const int cOffhandGeoset1 = FindColumn(header, "Geoset1Offhand");
    const int cOffhandGeoset2 = FindColumn(header, "Geoset2Offhand");

    if (cDisplay < 0 || cModel1 < 0) {
        return;
    }

    for (size_t n = 1; n < lines.size(); n++) {
        if (lines[n].empty()) {
            continue;
        }

        const std::vector<std::string> row = ParseCsvLine(lines[n]);
        uint32_t displayId = 0;

        // First seen wins: the same file can be reachable under more than one name.
        if (!ParseU32(Field(row, cDisplay), &displayId) || !displayId || !*Field(row, cModel1) || s_weapons.count(displayId)) {
            continue;
        }

        WeaponRow weapon;
        weapon.path[0] = Field(row, cModel1);
        weapon.path[1] = Field(row, cModel2);
        weapon.offhandPath[0] = Field(row, cOffhand1);
        weapon.offhandPath[1] = Field(row, cOffhand2);
        weapon.geo[0] = ParseImpliedBaseGeosets(Field(row, cGeoset1));
        weapon.geo[1] = ParseImpliedBaseGeosets(Field(row, cGeoset2));
        weapon.offhandGeo[0] = ParseImpliedBaseGeosets(Field(row, cOffhandGeoset1));
        weapon.offhandGeo[1] = ParseImpliedBaseGeosets(Field(row, cOffhandGeoset2));
        s_weapons.emplace(displayId, std::move(weapon));
    }
}

void LoadTypedTextureCsv(const std::string& text, bool hasColumn, std::unordered_map<uint32_t, std::vector<TypedTexture>>& out) {
    const std::vector<std::string> lines = SplitLines(text);

    if (lines.empty()) {
        return;
    }

    const std::vector<std::string> header = ParseCsvLine(lines[0]);
    const int cDisplay = FindColumn(header, "DisplayID");
    const int cColumn = FindColumn(header, "ModelColumn");
    const int cType = FindColumn(header, "TextureType");
    const int cPath = FindColumn(header, "TexturePath");

    if (cDisplay < 0 || cType < 0 || cPath < 0 || (hasColumn && cColumn < 0)) {
        return;
    }

    for (size_t n = 1; n < lines.size(); n++) {
        if (lines[n].empty()) {
            continue;
        }

        const std::vector<std::string> row = ParseCsvLine(lines[n]);
        uint32_t displayId = 0;
        TypedTexture texture;

        if (!ParseU32(Field(row, cDisplay), &displayId) || !displayId || !ParseU32(Field(row, cType), &texture.type) || !*Field(row, cPath)) {
            continue;
        }

        if (hasColumn && (!ParseU32(Field(row, cColumn), &texture.column) || texture.column > 1)) {
            continue;
        }

        texture.path = Field(row, cPath);

        // One texture per display, column and type; the first one wins.
        std::vector<TypedTexture>& list = out[displayId];
        bool duplicate = false;

        for (const TypedTexture& o : list) {
            if (o.column == texture.column && o.type == texture.type) {
                duplicate = true;
            }
        }

        if (!duplicate) {
            list.push_back(texture);
        }
    }
}

void LoadWeaponTextureCsv(const std::string& text) {
    LoadTypedTextureCsv(text, true, s_weaponTextures);
}

void LoadCreatureTextureCsv(const std::string& text) {
    LoadTypedTextureCsv(text, false, s_creatureTextures);
}

void LoadCreatureModelCsv(const std::string& text) {
    const std::vector<std::string> lines = SplitLines(text);

    if (lines.empty()) {
        return;
    }

    const std::vector<std::string> header = ParseCsvLine(lines[0]);
    const int cDisplay = FindColumn(header, "DisplayID");
    const int cPath = FindColumn(header, "CreatureModelPath");
    const int cGeoset = FindColumn(header, "Geoset");

    if (cDisplay < 0 || cPath < 0) {
        return;
    }

    for (size_t n = 1; n < lines.size(); n++) {
        if (lines[n].empty()) {
            continue;
        }

        const std::vector<std::string> row = ParseCsvLine(lines[n]);
        uint32_t displayId = 0;

        if (!ParseU32(Field(row, cDisplay), &displayId) || !displayId || !*Field(row, cPath) || s_creatures.count(displayId)) {
            continue;
        }

        CreatureRow creature;
        creature.path = Field(row, cPath);
        creature.geo = ParseImpliedBaseGeosets(Field(row, cGeoset));
        s_creatures.emplace(displayId, std::move(creature));
    }
}

// ---- loading ------------------------------------------------------------------------------------------------------

bool ReadArchiveFile(const char* path, std::string& out) {
    out.clear();

    if (!SFile::FileExists(path)) {
        return false;
    }

    void* data = nullptr;
    uint32_t size = 0;

    if (!SFile::LoadFile(path, &data, &size, 1, nullptr) || !data) {
        return false;
    }

    out.assign(static_cast<const char*>(data), size);
    SFile::Unload(data);

    return true;
}

bool ReadLooseFile(const char* path, std::string& out) {
    out.clear();
    FILE* file = fopen(path, "rb");

    if (!file) {
        return false;
    }

    char buffer[4096];
    size_t got;

    while ((got = fread(buffer, 1, sizeof(buffer), file)) > 0) {
        out.append(buffer, got);
    }

    fclose(file);

    return !out.empty();
}

// Feeds `load` every copy of a sidecar the client can see: the one the archive layer resolves, a loose copy beside the
// executable, and the copy inside every open-folder patch under Data (so several patches can each add rows).
void ForEachSidecar(const char* name, void (*load)(const std::string&)) {
    std::string text;
    char path[MAX_PATH];

    if (ReadArchiveFile(name, text)) load(text);

    snprintf(path, sizeof(path), "DBFilesClient\\%s", name);

    if (ReadArchiveFile(path, text)) load(text);
    if (ReadLooseFile(name, text)) load(text);
    if (ReadLooseFile(path, text)) load(text);

    WIN32_FIND_DATAA find = {};
    HANDLE handle = FindFirstFileA("Data\\*.MPQ", &find);

    if (handle == INVALID_HANDLE_VALUE) {
        return;
    }

    do {
        if (!(find.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            continue;
        }

        snprintf(path, sizeof(path), "Data\\%s\\DBFilesClient\\%s", find.cFileName, name);

        if (ReadLooseFile(path, text)) load(text);
    } while (FindNextFileA(handle, &find));

    FindClose(handle);
}

void LoadSidecars() {
    if (s_loaded) {
        return;
    }

    s_loaded = true;

    ForEachSidecar("WXLItemDisplayModels.csv", LoadModelCsv);
    ForEachSidecar("WXLItemDisplayModelMaterials.csv", LoadMaterialCsv);
    ForEachSidecar("WXLEquipAnimations.csv", LoadAnimationCsv);
    ForEachSidecar("WXLHelmOffsets.csv", LoadOffsetCsv);
    ForEachSidecar("WXLWeaponModels.csv", LoadWeaponModelCsv);
    ForEachSidecar("WXLWeaponTextures.csv", LoadWeaponTextureCsv);
    ForEachSidecar("WXLCreatureModels.csv", LoadCreatureModelCsv);
    ForEachSidecar("WXLCreatureTextures.csv", LoadCreatureTextureCsv);

    Log("sidecars: %zu displays with models, %zu with materials, %zu animation rows, %zu offset rules, %zu weapons, %zu creatures", s_models.size(), s_materials.size(), s_anim.size(), s_offsets.size(), s_weapons.size(), s_creatures.size());
}

// ---- path rules ---------------------------------------------------------------------------------------------------

bool IsKnownFolder(const char* folder, size_t length) {
    static const char* known[] = {"Head", "Shoulder", "Shirt", "Chest", "Waist", "Leg", "Foot", "Bracer", "Glove", "Cape", "Tabard", "Collections", "Weapon"};

    for (const char* name : known) {
        if (strlen(name) == length && !_strnicmp(folder, name, length)) {
            return true;
        }
    }

    return false;
}

const char* InferFolder(const char* name, bool collection, const char* customFolder, const char* slotFolder) {
    if (customFolder && *customFolder) return customFolder;
    if (collection) return "Collections";

    // A "collections_" prefix is a naming convention for the set, not a folder, so it is not read as one.
    if (StartsWithCI(name, "lshoulder_") || StartsWithCI(name, "rshoulder_") || StartsWithCI(name, "shoulder_") || ContainsCI(name, "_shoulder_l") || ContainsCI(name, "_shoulder_r")) return "Shoulder";
    if (StartsWithCI(name, "cape_")) return "Cape";
    if (StartsWithCI(name, "tabard_")) return "Tabard";

    return slotFolder;
}

uint32_t InferAttach(const char* name, bool collection, bool explicitAttach, uint32_t attach) {
    if (explicitAttach || collection) return attach;
    if (ContainsCI(name, "lshoulder_") || ContainsCI(name, "_shoulder_l")) return 6;
    if (ContainsCI(name, "rshoulder_") || ContainsCI(name, "_shoulder_r")) return 5;

    if (ContainsCI(name, "shoulder")) {
        if (ContainsCI(name, "_l.") || ContainsCI(name, "_l_")) return 6;
        if (ContainsCI(name, "_r.") || ContainsCI(name, "_r_")) return 5;
    }

    if (ContainsCI(name, "collections_")) {
        if (ContainsCI(name, "_shoulder_l")) return 6;
        if (ContainsCI(name, "_shoulder_r")) return 5;
        if (ContainsCI(name, "collections_belt_") || ContainsCI(name, "_belt")) return 53;
    }

    if (ContainsCI(name, "cape_")) return 12;
    if (ContainsCI(name, "tabard_")) return 34;

    return attach;
}

// Chest, legs, feet, hands and wrists are painted on the body, so a plain standalone model on them is nearly always a
// leftover in the stock DBC, not something meant to be drawn.
bool SlotAllowsPlainModel(uint32_t slot) {
    return slot == ITEMSLOT_HEAD || slot == ITEMSLOT_SHOULDER || slot == ITEMSLOT_BELT || slot == ITEMSLOT_TABARD || slot == ITEMSLOT_CAPE;
}

// Stock 3.3.5a ItemDisplayInfo rows were never cleaned of model names copied between rows: several hundred glove, boot,
// belt and chest rows still name a helm or a shoulder. The base game ignored them. Reading them now would hang a
// pauldron from a belt buckle.
bool IsStrayLegacyName(const char* stem) {
    const char* base = stem;

    for (const char* s = stem; *s; s++) {
        if (*s == '\\' || *s == '/') {
            base = s + 1;
        }
    }

    return StartsWithCI(base, "lshoulder_") || StartsWithCI(base, "rshoulder_") || StartsWithCI(base, "shoulder_") || StartsWithCI(base, "helm_");
}

bool IsGloveBodyModel(uint32_t slot, const char* stem) {
    return slot == ITEMSLOT_GLOVES && stem && ContainsCI(stem, "_glove");
}

void BuildModelPath(char* buffer, const char* modelName, const char* raceCode, const char* gender, uint32_t flags, const char* slotFolder, bool collection, const char* customFolder) {
    const bool appendRace = !(flags & 0x4);
    const bool appendGender = !(flags & 0x2);
    const bool subFolder = (flags & 0x20) != 0;
    const bool newSuffix = (flags & 0x40) != 0;
    const char* folder = InferFolder(modelName, collection, customFolder, slotFolder);

    const char* lastDot = nullptr;

    for (const char* s = modelName; *s; s++) {
        if (*s == '.') {
            lastDot = s;
        }
    }

    const size_t stemLength = lastDot ? static_cast<size_t>(lastDot - modelName) : strlen(modelName);
    std::string path = "Item\\ObjectComponents\\";
    path += folder;
    path += '\\';

    if (subFolder) {
        path.append(modelName, stemLength);
        path += '\\';
    }

    path.append(modelName, stemLength);

    if (appendRace || appendGender) {
        path += '_';

        if (appendRace) path += raceCode;
        if (newSuffix && appendRace && appendGender) path += '_';
        if (appendGender) path += gender;
    }

    path += ".mdx";
    CopyString(buffer, 264, path.c_str());
}

void BuildTexturePath(char* buffer, const char* textureName, const char* raceCode, const char* gender, uint32_t flags, const char* slotFolder, bool collection, const char* customFolder, const char* modelStem) {
    const bool appendRace = (flags & 0x8) != 0;
    const bool appendGender = (flags & 0x10) != 0;
    const bool subFolder = (flags & 0x20) != 0;
    const bool newSuffix = (flags & 0x40) != 0;

    // A texture name may carry its own folder ("Head\\name").
    char explicitFolder[32] = {};
    const char* textureStem = textureName;

    for (const char* s = textureName; *s; s++) {
        if (*s != '\\' && *s != '/') {
            continue;
        }

        const size_t length = static_cast<size_t>(s - textureName);

        if (length > 0 && length < sizeof(explicitFolder) && IsKnownFolder(textureName, length)) {
            memcpy(explicitFolder, textureName, length);
            explicitFolder[length] = '\0';
            textureStem = s + 1;
        }

        break;
    }

    const char* subBase = modelStem && *modelStem ? modelStem : textureStem;
    const char* folder = InferFolder(subBase, collection, customFolder, slotFolder);

    if (explicitFolder[0]) {
        folder = explicitFolder;
    }

    size_t subBaseLength = strlen(subBase);

    for (const char* s = subBase; *s; s++) {
        if (*s == '.') {
            subBaseLength = static_cast<size_t>(s - subBase);
            break;
        }
    }

    std::string path = "Item\\ObjectComponents\\";
    path += folder;
    path += '\\';

    if (subFolder) {
        path.append(subBase, subBaseLength);
        path += '\\';
    }

    path += textureStem;

    if (appendRace || appendGender) {
        path += '_';

        if (appendRace) path += raceCode;
        if (newSuffix && appendRace && appendGender) path += '_';
        if (appendGender) path += gender;
    }

    path += ".blp";
    CopyString(buffer, 264, path.c_str());
}

// Compares the model stem alone: lower case, without folder, extension or geoset list.
std::string StemKey(const char* value) {
    const char* base = value;

    for (const char* p = value; *p; p++) {
        if (*p == '\\' || *p == '/') {
            base = p + 1;
        }
    }

    std::string out;

    for (const char* p = base; *p && *p != ':' && *p != '.'; p++) {
        char c = static_cast<char>(tolower(static_cast<unsigned char>(*p)));

        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_') {
            out.push_back(c);
        }
    }

    return out;
}

bool MaterialMatches(const MaterialRow& e, uint32_t modelColumn, uint32_t partIndex, const char* modelName) {
    const bool hasModel = e.model[0] != '\0';

    if (hasModel) {
        const std::string a = StemKey(e.model);
        const std::string b = StemKey(modelName);

        if (a.empty() || b.empty() || a != b) {
            return false;
        }
    }

    if (!hasModel && e.modelColumn != kNone && e.modelColumn != modelColumn) {
        return false;
    }

    if (hasModel && modelColumn != kNone && e.modelColumn != kNone && e.modelColumn != modelColumn) {
        return false;
    }

    if (!hasModel && e.modelColumn == kNone && e.modelIndex != kNone) {
        const bool columnKnown = modelColumn != kNone;
        const bool columnMatch = columnKnown && (e.modelIndex == modelColumn || e.modelIndex == modelColumn + 1);
        const bool partMatch = e.modelIndex == partIndex || e.modelIndex == partIndex + 1;

        if (!columnMatch && !partMatch) {
            return false;
        }
    }

    return e.texture[0] != '\0';
}

void BuildMaterials(Planned& out, uint32_t displayId, uint32_t modelColumn, uint32_t partIndex, const char* modelName, const char* raceCode, const char* gender, const char* slotFolder, bool collection, const char* customFolder, const char* modelStem) {
    const auto it = s_materials.find(displayId);

    if (it == s_materials.end()) {
        return;
    }

    for (const MaterialRow& m : it->second) {
        if (!MaterialMatches(m, modelColumn, partIndex, modelName) || m.textureType == kNone) {
            continue;
        }

        // There is no way to hide a batch any more, so a hide row has nothing to do.
        if (ContainsCI(m.targetMode, "Hide") || StartsWithCI(m.texture, "__hide__") || StartsWithCI(m.texture, "hide")) {
            continue;
        }

        char path[264] = {};
        BuildTexturePath(path, m.texture, raceCode, gender, 0, slotFolder, collection, m.folder[0] ? m.folder : customFolder, modelStem);

        if (path[0]) {
            out.materials.emplace_back(m.textureType, path);
        }
    }
}

bool ModelExists(const char* path) {
    std::string key(path);

    for (char& c : key) {
        c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    }

    // The client loads models as .m2 whatever the DBC names.
    if (key.size() > 4 && !key.compare(key.size() - 4, 4, ".mdx")) {
        key.replace(key.size() - 4, 4, ".m2");
    }

    const auto it = s_exists.find(key);

    if (it != s_exists.end()) {
        return it->second;
    }

    const bool exists = SFile::FileExists(key.c_str()) != 0;
    s_exists.emplace(key, exists);

    if (!exists) {
        Log("model not found, skipped: '%s'", key.c_str());
    }

    return exists;
}

// ---- Icon2 --------------------------------------------------------------------------------------------------------

// "<attach1>:<attach2>:<flags>:<folder>", each part optional; "a|b" gives a left and a right attachment (the left is used).
struct Icon2 {
    bool configured = false;
    bool attachAExplicit = false;
    bool attachBExplicit = false;
    uint32_t attachA = 0;
    uint32_t attachB = 0;
    uint32_t flags = 0;
    char folder[64] = {};
};

bool Icon2FieldHasValue(const char* text, uint32_t index) {
    const char* start = text;

    for (uint32_t i = 0; i < index; i++) {
        while (*start && *start != ':') {
            start++;
        }

        if (*start != ':') {
            return false;
        }

        start++;
    }

    const char* end = start;

    while (*end && *end != ':') {
        end++;
    }

    return end > start;
}

Icon2 ParseIcon2(const char* text, uint32_t defaultA, uint32_t defaultB) {
    Icon2 icon;
    icon.attachA = defaultA;
    icon.attachB = defaultB;

    // The column is a string block pointer in a real row; anything small or not starting like a config is no config.
    if (!text || reinterpret_cast<uintptr_t>(text) <= 0x10000 || !*text || !(*text == ':' || (*text >= '0' && *text <= '9'))) {
        return icon;
    }

    icon.configured = true;
    icon.attachAExplicit = Icon2FieldHasValue(text, 0);
    icon.attachBExplicit = Icon2FieldHasValue(text, 1);

    const char* f2 = text;

    while (*f2 && *f2 != ':') {
        f2++;
    }

    if (f2 != text) {
        icon.attachA = static_cast<uint32_t>(atoi(text));
    }

    if (*f2 != ':') {
        return icon;
    }

    const char* f3 = f2 + 1;

    while (*f3 && *f3 != ':') {
        f3++;
    }

    if (f3 != f2 + 1) {
        icon.attachB = static_cast<uint32_t>(atoi(f2 + 1));
    }

    if (*f3 != ':') {
        return icon;
    }

    const char* f4 = f3 + 1;

    while (*f4 && *f4 != ':') {
        f4++;
    }

    if (f4 != f3 + 1) {
        icon.flags = static_cast<uint32_t>(atoi(f3 + 1));
    }

    if (*f4 == ':') {
        CopyString(icon.folder, sizeof(icon.folder), f4 + 1);
    }

    return icon;
}

// ---- planning -----------------------------------------------------------------------------------------------------

bool HasAnimSync(uint32_t displayId) {
    const auto it = s_anim.find(displayId);

    return it != s_anim.end() && it->second;
}

void FinishPlanned(Planned& p, uint32_t slot, uint32_t displayId) {
    p.slot = slot;
    p.rootSkinned = p.attach == kCollectionAttach && (p.collection || slot == ITEMSLOT_CAPE);

    // Shape alone is not enough: WXLEquipAnimations.csv opts each display in.
    const bool shapeEligible = p.rootSkinned || p.attach == 6 || p.attach == 5 || p.attach == 53 || p.attach == 11;
    p.animSync = shapeEligible && HasAnimSync(displayId);
}

struct RaceInfo {
    char code[8];
    char gender[2];
};

// Fills `out` with what this display wants attached on this slot. `nativeDrawsFirst`: the client already draws the
// display's first model (a head not owned by Equip Plus), so only the second is planned.
void Plan(uint32_t slot, const ItemDisplayInfoRec* rec, const RaceInfo& race, bool nativeDrawsFirst, std::vector<Planned>& out) {
    const SlotConfig& cfg = kSlotConfig[slot];
    const uint32_t displayId = static_cast<uint32_t>(rec->m_id);
    const Icon2 icon = ParseIcon2(rec->m_inventoryIcon[1], cfg.attach1, cfg.attach2);

    // A row owned by a CSV is described by the CSV alone.
    const auto sidecar = s_models.find(displayId);
    bool sidecarUsed = false;

    // Whether the row's own model lists mix collection pieces and plain ones.
    bool rowHasCollection = false;
    bool rowHasNormal = false;

    for (int column = 0; column < 2; column++) {
        char part[264];

        for (uint32_t i = 0; i < 16 && ListPart(rec->m_modelName[column], i, part, sizeof(part)); i++) {
            if (!part[0]) continue;

            if (strchr(part, ':')) rowHasCollection = true;
            else rowHasNormal = true;
        }
    }

    const bool mixedRow = rowHasCollection && rowHasNormal;

    auto effectiveFlags = [&](bool collection) {
        uint32_t flags = icon.flags;

        if (collection && !(flags & 0x40)) {
            if (!(flags & 0x4) && !(flags & 0x2)) {
                flags |= 0x40;
            }
        }

        if (!collection) {
            const bool force = (flags & kFlagForceModelRaceGender) || ((flags & 0x40) && !mixedRow);

            if (!force) {
                if (!cfg.useRace) flags |= 0x4;
                if (!cfg.useGender) flags |= 0x2;
            }
        }

        return flags;
    };

    if (sidecar != s_models.end()) {
        uint32_t index = 0;

        for (const ModelRow& sc : sidecar->second) {
            if (sc.slot != kNone && sc.slot != slot) {
                continue;
            }

            uint32_t attach = sc.attach != kNone ? sc.attach : cfg.attach1;
            const bool collection = (sc.folder[0] && StartsWithCI(sc.folder, "Collections")) || sc.geo.count > 0 || StartsWithCI(sc.model, "collections_");

            // A collection row that names no attachment hangs from the character root, as the DBC path does.
            if (collection && sc.attach == kNone) attach = kCollectionAttach;
            if (collection && IsGloveBodyModel(slot, sc.model)) attach = kCollectionAttach;

            if (!collection && !SlotAllowsPlainModel(slot)) {
                index++;
                continue;
            }

            const uint32_t modelFlags = sc.modelFlags == 0xFFFFFFFF ? effectiveFlags(collection) : sc.modelFlags;
            const uint32_t textureFlags = sc.textureFlags == 0xFFFFFFFF ? effectiveFlags(collection) : sc.textureFlags;
            const char* folder = sc.folder[0] ? sc.folder : nullptr;

            Planned p;
            BuildModelPath(p.path, sc.model, race.code, race.gender, modelFlags, cfg.folder, collection, folder);

            if (!ModelExists(p.path)) {
                index++;
                continue;
            }

            if (sc.texture[0]) {
                BuildTexturePath(p.texture, sc.texture, race.code, race.gender, textureFlags, cfg.folder, collection, folder, sc.model);
            }

            BuildMaterials(p, displayId, kNone, index, sc.model, race.code, race.gender, cfg.folder, collection, folder, sc.model);
            p.attach = attach;
            p.geo = sc.geo;
            p.collection = collection;
            FinishPlanned(p, slot, displayId);
            out.push_back(std::move(p));
            sidecarUsed = true;
            index++;
        }
    }

    if (sidecarUsed) {
        return;
    }

    for (int column = nativeDrawsFirst ? 1 : 0; column < 2; column++) {
        const char* label = column == 0 ? "M1" : "M2";
        const bool explicitAttach = column == 0 ? icon.attachAExplicit : icon.attachBExplicit;
        const uint32_t columnAttach = column == 0 ? icon.attachA : icon.attachB;
        const uint32_t defaultAttach = column == 0 ? cfg.attach1 : cfg.attach2;
        char modelPart[264];

        for (uint32_t i = 0; i < 16 && ListPart(rec->m_modelName[column], i, modelPart, sizeof(modelPart)); i++) {
            if (!modelPart[0]) {
                continue;
            }

            char texturePart[264] = {};

            if (!ListPart(rec->m_modelTexture[column], i, texturePart, sizeof(texturePart))) {
                ListPart(rec->m_modelTexture[column], 0, texturePart, sizeof(texturePart));
            }

            // "name:1201,2301" is a collection piece showing only those geosets.
            char stem[264];
            GeosetFilter geo = {};
            const char* colon = strchr(modelPart, ':');

            if (colon) {
                size_t length = static_cast<size_t>(colon - modelPart);

                if (length >= sizeof(stem)) length = sizeof(stem) - 1;

                memcpy(stem, modelPart, length);
                stem[length] = '\0';
                geo = ParseGeosets(colon + 1);
            } else {
                CopyString(stem, sizeof(stem), modelPart);
            }

            if (!stem[0]) {
                continue;
            }

            const bool collection = geo.count > 0;

            if (!collection && !SlotAllowsPlainModel(slot)) {
                continue;
            }

            // Head and shoulders are the only slots the base game read, so only they can have a model by design.
            if (!collection && slot != ITEMSLOT_HEAD && slot != ITEMSLOT_SHOULDER && !icon.configured && IsStrayLegacyName(stem)) {
                Log("%s[%u]: stale stock model '%s' on slot %u (display %u) ignored", label, i, stem, slot, displayId);
                continue;
            }

            const uint32_t flags = effectiveFlags(collection);
            uint32_t attach;

            if (mixedRow) attach = collection ? kCollectionAttach : defaultAttach;
            else if (collection && !explicitAttach) attach = kCollectionAttach;
            else attach = columnAttach;

            attach = InferAttach(stem, collection, explicitAttach, attach);

            if (collection && IsGloveBodyModel(slot, stem)) attach = kCollectionAttach;

            const char* folder = icon.folder[0] ? icon.folder : nullptr;

            Planned p;
            BuildModelPath(p.path, stem, race.code, race.gender, flags, cfg.folder, collection, folder);

            if (!ModelExists(p.path)) {
                continue;
            }

            if (texturePart[0]) {
                BuildTexturePath(p.texture, texturePart, race.code, race.gender, flags, cfg.folder, collection, folder, stem);
            }

            BuildMaterials(p, displayId, static_cast<uint32_t>(column), i, stem, race.code, race.gender, cfg.folder, collection, folder, stem);
            p.attach = attach;
            p.geo = geo;
            p.collection = collection;
            FinishPlanned(p, slot, displayId);
            out.push_back(std::move(p));
        }
    }
}

// ---- WXLHelmOffsets.csv -------------------------------------------------------------------------------------------

struct HelmDefault {
    const char* id;
    float x, z;
};

// Calibrated by eye against each race's head proportions in wxl-modern-assets.
const HelmDefault kHelmDefaults[] = {
    {"drf", -0.0587258f, -0.195f}, {"drm", -0.0587258f, -0.245f}, {"taf", -0.13f, -0.1f}, {"tam", -0.2f, -0.1f},
    {"nim", -0.09f, -0.18f}, {"nif", -0.08f, -0.195f}, {"orf", -0.08f, -0.171f}, {"orm", -0.13f, -0.21f},
    {"trf", -0.0887258f, -0.08623257f}, {"trm", -0.13f, -0.16f}, {"bef", 0.01f, -0.2f}, {"bem", -0.08f, -0.165f},
    {"huf", -0.09f, -0.18f}, {"scm", -0.12f, -0.12623256f}, {"scf", -0.01f, -0.15f}, {"gnf", -0.015f, -0.263f},
    {"gnm", -0.009f, -0.23f}, {"dwm", -0.0227258f, -0.1725f}, {"dwf", 0.01f, -0.195f},
};

struct ModelOffset {
    bool haveOffset = false;
    bool haveScale = false;
    float x = 0.0f, y = 0.0f, z = 0.0f;
    float scale = 1.0f;
};

int RuleScore(const OffsetRule& rule, const std::string& pathStem, const std::string& pathBase, const std::string& fileStem, const std::string& fileBase, const char* raceSex) {
    if (rule.raceSex[0] && strcmp(rule.raceSex, "*") && strcmp(rule.raceSex, raceSex)) {
        return -1;
    }

    int score = -1;

    if (strchr(rule.model, '\\')) {
        if (pathStem == rule.model) score = 40;
        else if (pathBase == rule.model) score = 30;
    } else {
        if (fileStem == rule.model) score = 20;
        else if (fileBase == rule.model) score = 10;
    }

    if (score >= 0 && rule.raceSex[0] && strcmp(rule.raceSex, "*")) {
        score += 5;
    }

    return score;
}

ModelOffset FindOffset(const char* path) {
    ModelOffset result;

    if (s_offsets.empty()) {
        return result;
    }

    char id[8] = {};
    const bool hasRaceId = ModelRaceId(path, id);
    const std::string pathStem = NormalizeOffsetKey(path);
    std::string pathBase = pathStem;
    StripRaceGender(pathBase);

    const size_t slash = pathStem.find_last_of('\\');
    const std::string fileStem = slash == std::string::npos ? pathStem : pathStem.substr(slash + 1);
    std::string fileBase = fileStem;
    StripRaceGender(fileBase);

    const OffsetRule* best = nullptr;
    int bestScore = -1;

    for (const OffsetRule& rule : s_offsets) {
        const int score = RuleScore(rule, pathStem, pathBase, fileStem, fileBase, hasRaceId ? id : "");

        if (score > bestScore) {
            best = &rule;
            bestScore = score;
        }
    }

    if (!best || best->disable) {
        return result;
    }

    if (best->isHelm && hasRaceId) {
        // The human default is the table's fall-through.
        float x = -0.0587258f;
        float z = -0.18623257f;

        for (const HelmDefault& d : kHelmDefaults) {
            if (!strcmp(d.id, id)) {
                x = d.x;
                z = d.z;
                break;
            }
        }

        result.x = x;
        result.z = z;
        result.haveOffset = true;
    }

    if (best->add) {
        if (best->hasX) { result.x += best->x; result.haveOffset = true; }
        if (best->hasY) { result.y += best->y; result.haveOffset = true; }
        if (best->hasZ) { result.z += best->z; result.haveOffset = true; }
    } else {
        if (best->hasX) { result.x = best->x; result.haveOffset = true; }
        if (best->hasY) { result.y = best->y; result.haveOffset = true; }
        if (best->hasZ) { result.z = best->z; result.haveOffset = true; }
    }

    if (best->hasScale) {
        result.scale = best->scale;
        result.haveScale = true;
    }

    return result;
}

// ---- attaching ----------------------------------------------------------------------------------------------------

CStatus s_status;

void ShowOnlyGeosets(CM2Model* model, const GeosetFilter& filter);

CTexture* LoadTexture(const char* path) {
    CTexture* texture = CreateTexture(path, &s_status);

    if (!texture) {
        // Same as a missing texture anywhere else in the client: the built-in checker, not an invisible item.
        Log("texture load failed '%s', using the fallback", path);
        texture = CreateTexture(kFallbackTexture, &s_status);
    }

    return texture;
}

void ReplaceTexture(CM2Model* model, uint32_t type, const char* path) {
    CTexture* texture = LoadTexture(path);

    if (!texture) {
        return;
    }

    model->ReplaceTexture(type, texture);
    HandleClose(reinterpret_cast<HOBJECT>(texture));
}

CM2Model* AttachPlanned(CM2Model* parent, const Planned& p, uint64_t tag) {
    CM2Model* model = parent->m_scene->CreateModel(p.path, 0);

    if (!model) {
        return nullptr;
    }

    if (p.texture[0]) {
        ReplaceTexture(model, M2TEX_OBJECT_SKIN, p.texture);
    }

    for (const auto& material : p.materials) {
        ReplaceTexture(model, material.first, material.second.c_str());
    }

    ShowOnlyGeosets(model, p.geo);

    const ModelOffset offset = FindOffset(p.path);
    uint32_t followFlags = 0;

    if (p.rootSkinned) followFlags |= M2BoneFollow::COPY_BONES;
    if (p.animSync) followFlags |= M2BoneFollow::SYNC_SEQUENCE;

    // Every model Equip Plus attaches carries a follow, even one with no flags, because its tag is how the model is
    // recognised again.
    M2BoneFollow* follow = M2BoneFollowCreate(model, followFlags);
    follow->tag = tag;
    follow->retargetPivots = s_cvarPivots && s_cvarPivots->m_intValue > 0;
    follow->retargetScale = s_cvarScale && s_cvarScale->m_intValue > 0;

    if (offset.haveOffset) {
        follow->offset[0] = offset.x;
        follow->offset[1] = offset.y;
        follow->offset[2] = offset.z;
    }

    if (offset.haveScale) {
        follow->userScale = offset.scale;
    }

    if (p.rootSkinned) {
        model->AttachToParent(parent, kRootAttach, nullptr, 1);
    } else {
        model->AttachToParent(parent, p.attach, nullptr, 0);

        // A plain accessory rides one attachment point, so a nudge and a size are just its own transform.
        if ((offset.haveOffset || offset.haveScale) && model->m_attachParent) {
            model->SetWorldTransform(C3Vector(offset.x, offset.y, offset.z), 0.0f, offset.haveScale ? offset.scale : 1.0f);
        }
    }

    // The parent may have refused: a loaded character with no such attachment point takes nothing there.
    const bool attached = model->m_attachParent == parent;

    // The attachment holds its own reference.
    model->Release();

    Log("%s '%s' slot=%u attach=%u geo=%u root=%d sync=%d", attached ? "attached" : "refused", p.path, p.slot, p.attach, p.geo.count, p.rootSkinned, p.animSync);

    return attached ? model : nullptr;
}

bool IsAttached(CM2Model* parent, CM2Model* model, uint64_t tag) {
    for (CM2Model* child = parent->m_attachList; child; child = child->m_attachNext) {
        if (child == model) {
            return child->m_boneFollow && child->m_boneFollow->tag == tag;
        }
    }

    return false;
}

// Detaches the models on this attachment that Equip Plus did not put there.
void RemoveForeign(CM2Model* parent, uint32_t id) {
    for (;;) {
        CM2Model* found = nullptr;

        for (CM2Model* child = parent->m_attachList; child; child = child->m_attachNext) {
            if (child->m_attachmentId == id && !(child->m_boneFollow && child->m_boneFollow->tag)) {
                found = child;
                break;
            }
        }

        if (!found) {
            return;
        }

        found->DetachFromParent();
    }
}

void DetachSlot(CM2Model* parent, SlotState& state) {
    if (parent && parent == state.parent) {
        for (CM2Model* model : state.models) {
            if (IsAttached(parent, model, state.tag)) {
                model->DetachFromParent();
            }
        }
    }

    state.models.clear();
    state.displayId = 0;
    state.parent = nullptr;
    state.tag = 0;
}

// Only the listed geosets draw.
void ShowOnlyGeosets(CM2Model* model, const GeosetFilter& filter) {
    if (!filter.count) {
        return;
    }

    model->SetGeometryVisible(0, 0xFFFF, 0);

    for (uint32_t i = 0; i < filter.count; i++) {
        model->SetGeometryVisible(filter.ids[i], filter.ids[i], 1);
    }
}

// A texture for a creature: the flags the client's own monster skins use.
void ReplaceCreatureTexture(CM2Model* model, uint32_t type, const char* path) {
    CGxTexFlags flags(GxTex_LinearMipLinear, 1, 1, 0, 0, 0, 1);
    CTexture* texture = TextureCreate(path, flags, &s_status, 0);

    if (!texture) {
        return;
    }

    model->ReplaceTexture(type, texture);
    HandleClose(reinterpret_cast<HOBJECT>(texture));
}

// "Creature\X\X_33113.mdx" -> the display id after the last underscore, or 0.
uint32_t TrailingDisplayId(const char* path, std::string& stem) {
    const char* base = path;

    for (const char* p = path; *p; p++) {
        if (*p == '\\' || *p == '/') {
            base = p + 1;
        }
    }

    const char* dot = strrchr(base, '.');
    const size_t length = dot ? static_cast<size_t>(dot - path) : strlen(path);
    const char* underscore = nullptr;

    for (size_t i = 0; i < length; i++) {
        if (path[i] == '_') {
            underscore = path + i;
        }
    }

    if (!underscore || underscore + 1 >= path + length) {
        return 0;
    }

    uint32_t value = 0;

    for (const char* p = underscore + 1; p < path + length; p++) {
        if (*p < '0' || *p > '9') {
            return 0;
        }

        value = value * 10 + static_cast<uint32_t>(*p - '0');
    }

    stem.assign(path, static_cast<size_t>(underscore - path));

    return value;
}

std::string StemOf(const std::string& path) {
    std::string stem = path;
    const size_t dot = stem.find_last_of('.');

    if (dot != std::string::npos && dot > stem.find_last_of('\\') + 1) {
        stem.resize(dot);
    }

    for (char& c : stem) {
        c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    }

    return stem;
}

bool ResolveModelPath(const char* fileName, char* out, size_t size) {
    if (!s_cvarEnable || s_cvarEnable->m_intValue <= 0) {
        return false;
    }

    LoadSidecars();

    if (s_creatures.empty()) {
        return false;
    }

    std::string stem;
    const uint32_t displayId = TrailingDisplayId(fileName, stem);

    if (!displayId) {
        return false;
    }

    const auto it = s_creatures.find(displayId);

    if (it == s_creatures.end()) {
        return false;
    }

    // CreatureModelData names the display's own model with the display id appended. The sheet has to name the same model,
    // or this is a different file that happens to end in digits.
    std::string wanted = StemOf(it->second.path);

    for (char& c : stem) {
        c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    }

    if (wanted != stem) {
        return false;
    }

    CopyString(out, size, it->second.path.c_str());

    return true;
}

}  // namespace

// ---- the state each component carries -----------------------------------------------------------------------------

struct EquipPlusState {
    SlotState slot[11];
};

static EquipPlusState* StateOf(CCharacterComponent* component, bool create) {
    if (!component->m_equipPlus && create) {
        component->m_equipPlus = new EquipPlusState();
    }

    return component->m_equipPlus;
}

// ---- public -------------------------------------------------------------------------------------------------------

void EquipPlus::Init() {
    s_cvarEnable = CVar::Register("equipPlus", "Equip Plus: 3D models on every armour slot, 0 off, 1 on", 1, "1", nullptr, 0, false, nullptr, false);
    s_cvarPivots = CVar::Register("equipPlusPivots", "Equip Plus: move each part of a retail piece to the matching bone of the client body", 1, "0", nullptr, 0, false, nullptr, false);
    s_cvarScale = CVar::Register("equipPlusScale", "Equip Plus: size a retail piece to the client skeleton", 1, "0", nullptr, 0, false, nullptr, false);
    s_cvarLog = CVar::Register("equipPlusLog", "Equip Plus: write EquipPlus.log", 0, "0", nullptr, 0, false, nullptr, false);

    CM2Scene::s_resolveModelPath = ResolveModelPath;
    EquipPlus::RegisterCommands();
}

bool EquipPlus::Enabled() {
    return !s_cvarEnable || s_cvarEnable->m_intValue > 0;
}

void EquipPlus::Reload() {
    s_weapons.clear();
    s_weaponTextures.clear();
    s_creatures.clear();
    s_creatureTextures.clear();
    s_models.clear();
    s_materials.clear();
    s_anim.clear();
    s_offsets.clear();
    s_exists.clear();
    s_loaded = false;
    LoadSidecars();
}

bool EquipPlus::OwnsNativeModel(ITEM_SLOT slot, const ItemDisplayInfoRec* rec) {
    if (!Enabled() || !rec || (slot != ITEMSLOT_HEAD && slot != ITEMSLOT_SHOULDER)) {
        return false;
    }

    LoadSidecars();

    const auto it = s_models.find(static_cast<uint32_t>(rec->m_id));

    if (it != s_models.end()) {
        for (const ModelRow& row : it->second) {
            if (row.slot == kNone || row.slot == static_cast<uint32_t>(slot)) {
                return true;
            }
        }
    }

    if (strchr(rec->m_modelName[0], ':') || strchr(rec->m_modelName[1], ':')) {
        return true;
    }

    const char* icon = rec->m_inventoryIcon[1];

    return icon && reinterpret_cast<uintptr_t>(icon) > 0x10000 && (*icon == ':' || (*icon >= '0' && *icon <= '9'));
}

void EquipPlus::Apply(CCharacterComponent* component, ITEM_SLOT slot, const ItemDisplayInfoRec* rec) {
    if (!Enabled() || !rec || static_cast<uint32_t>(slot) > ITEMSLOT_CAPE) {
        return;
    }

    CM2Model* parent = component->m_data.m_model;

    if (!parent || !parent->m_scene) {
        return;
    }

    ChrRacesRec* racesRec = g_chrRacesDB.GetRecord(component->m_data.m_preferences.m_raceID);

    if (!racesRec || !racesRec->m_clientPrefix) {
        return;
    }

    LoadSidecars();

    static const char* genders[3] = {"M", "F", "U"};
    const uint32_t sex = component->m_data.m_preferences.m_sexID;

    RaceInfo race = {};
    CopyString(race.code, sizeof(race.code), racesRec->m_clientPrefix);
    CopyString(race.gender, sizeof(race.gender), genders[sex < 3 ? sex : 0]);

    EquipPlusState* state = StateOf(component, true);
    SlotState& current = state->slot[slot];
    const uint32_t raceSex = component->m_data.m_preferences.m_raceID * 4 + sex;

    // Items are re-applied often; keep what is there if it is already right.
    if (current.displayId == static_cast<uint32_t>(rec->m_id) && current.raceSex == raceSex && current.parent == parent) {
        bool intact = true;

        for (CM2Model* model : current.models) {
            if (!IsAttached(parent, model, current.tag)) {
                intact = false;
                break;
            }
        }

        if (intact) {
            return;
        }
    }

    DetachSlot(parent, current);

    // A head the client draws itself still gets its second model from here, and a shoulder never does.
    const bool owns = OwnsNativeModel(slot, rec);

    // Taking over from a stock helm or pair of pauldrons: the client's own model is still on the attachment.
    if (owns) {
        if (slot == ITEMSLOT_HEAD) {
            RemoveForeign(parent, ATTACH_HELM);
        } else {
            RemoveForeign(parent, 6);
            RemoveForeign(parent, 5);
        }
    }

    if (slot == ITEMSLOT_SHOULDER && !owns) {
        return;
    }

    std::vector<Planned> plan;
    Plan(slot, rec, race, slot == ITEMSLOT_HEAD && !owns, plan);

    current.displayId = static_cast<uint32_t>(rec->m_id);
    current.raceSex = raceSex;
    current.parent = parent;
    current.tag = s_nextTag++;

    for (const Planned& p : plan) {
        CM2Model* model = AttachPlanned(parent, p, current.tag);

        if (model) {
            current.models.push_back(model);
        }
    }
}

void EquipPlus::Remove(CCharacterComponent* component, ITEM_SLOT slot) {
    EquipPlusState* state = StateOf(component, false);

    if (!state || static_cast<uint32_t>(slot) > ITEMSLOT_CAPE) {
        return;
    }

    DetachSlot(component->m_data.m_model, state->slot[slot]);
}

void EquipPlus::Reset(CCharacterComponent* component) {
    delete component->m_equipPlus;
    component->m_equipPlus = nullptr;
}

void EquipPlus::Destroy(CCharacterComponent* component) {
    EquipPlus::Reset(component);
}

void EquipPlus::ApplyWeapon(CM2Model* parent, int32_t link, const ItemDisplayInfoRec* rec, bool offhand) {
    if (!Enabled() || !parent || !rec || link < 0) {
        return;
    }

    LoadSidecars();

    const auto it = s_weapons.find(static_cast<uint32_t>(rec->m_id));

    if (it == s_weapons.end()) {
        return;
    }

    const WeaponRow& weapon = it->second;

    // The client has just put its own model on this attachment; the sheet's replaces it.
    RemoveForeign(parent, static_cast<uint32_t>(link));

    const auto textures = s_weaponTextures.find(static_cast<uint32_t>(rec->m_id));
    const char* folder = link == ATTACH_SHIELD ? "Item\\ObjectComponents\\Shield\\" : "Item\\ObjectComponents\\Weapon\\";

    for (uint32_t column = 0; column < 2; column++) {
        // A hand-authored mirror for the off hand, if the sheet gives one.
        const bool useOffhand = offhand && !weapon.offhandPath[column].empty();
        const std::string& path = useOffhand ? weapon.offhandPath[column] : weapon.path[column];

        if (path.empty() || !ModelExists(path.c_str())) {
            continue;
        }

        CM2Model* model = parent->m_scene->CreateModel(path.c_str(), 0);

        if (!model) {
            continue;
        }

        // The display's own texture for this model, then the sheet's, which can name any texture type.
        if (*rec->m_modelTexture[column]) {
            std::string texture = folder;
            texture += rec->m_modelTexture[column];
            texture += ".blp";
            ReplaceTexture(model, M2TEX_OBJECT_SKIN, texture.c_str());
        }

        if (textures != s_weaponTextures.end()) {
            for (const TypedTexture& t : textures->second) {
                if (t.column == column) {
                    ReplaceTexture(model, t.type, t.path.c_str());
                }
            }
        }

        ShowOnlyGeosets(model, useOffhand ? weapon.offhandGeo[column] : weapon.geo[column]);

        if (column == 0 && rec->m_itemVisual > 0) {
            CCharacterComponent::ComponentUtilAddItemVisual(model, rec->m_itemVisual);
        }

        model->AttachToParent(parent, static_cast<uint32_t>(link), nullptr, 0);
        model->Release();
    }
}

void EquipPlus::ApplyCreature(CM2Model* model, const CreatureDisplayInfoRec* rec) {
    if (!Enabled() || !model || !rec) {
        return;
    }

    LoadSidecars();

    const auto textures = s_creatureTextures.find(static_cast<uint32_t>(rec->m_id));

    // A sheet row wins over the display's three monster textures, and can set more types than those.
    if (textures != s_creatureTextures.end()) {
        for (const TypedTexture& t : textures->second) {
            ReplaceCreatureTexture(model, t.type, t.path.c_str());
        }
    }

    const auto row = s_creatures.find(static_cast<uint32_t>(rec->m_id));

    if (row != s_creatures.end() && row->second.geo.count) {
        ShowOnlyGeosets(model, row->second.geo);
        model->OptimizeVisibleGeometry();
    }
}

static int32_t CCommand_EquipPlus(const char* /*command*/, const char* arguments) {
    if (arguments && !_stricmp(arguments, "reload")) {
        EquipPlus::Reload();
        ConsolePrintf("equipplus: reloaded the CSV files. Models already worn change when the item is equipped again.");
    } else {
        LoadSidecars();
        ConsolePrintf("equipplus: %s. %zu displays with models, %zu with materials, %zu animation rows, %zu offset rules, %zu weapons, %zu creatures, %zu creature texture sets.", EquipPlus::Enabled() ? "on" : "off", s_models.size(), s_materials.size(), s_anim.size(), s_offsets.size(), s_weapons.size(), s_creatures.size(), s_creatureTextures.size());
        ConsolePrintf("equipplus reload: read the CSV files again. Set the equipPlusLog cvar to 1 to write EquipPlus.log.");
    }

    return 1;
}

void EquipPlus::RegisterCommands() {
    ConsoleCommandRegister("equipplus", CCommand_EquipPlus, CATEGORY_DEBUG, "Equip Plus status; 'equipplus reload' re-reads its CSV files");
}
