#include "WoW/Source/ModernAssetsCommands.h"

#include "Engine/Source/ConsoleBase/ConsoleCommand.h"
#include "Engine/Source/ConsoleBase/IConsole.h"

#if defined(WOWDECOMP_MODERN_ASSETS)

#include "Engine/Source/ModernAssets/Db2/Db2File.h"
#include "Engine/Source/ModernAssets/Db2/LightTables.h"
#include "Engine/Source/ModernAssets/FdidResolver.h"
#include "Engine/Source/ModernAssets/M2/M2Modern.h"
#include "Engine/Source/ModernAssets/ModernAssetsLog.h"

#include "Engine/Source/Model2/M2Model.h"
#include "Engine/Source/Model2/M2Scene.h"
#include "WoW/Source/WorldClient/World.h"

#include <cstdlib>
#include <vector>

namespace
{
    // A table this command reports on: name plus the filename the client's archives hold it under.
    // Loaded on demand, never at startup: browsing a database costs a full table decode.
    struct Probe
    {
        const char* name;
        const char* file;
    };

    const Probe s_probes[] = {
        { "TextureFilePath", "TextureFilePath.db2" },
        { "ModelFilePath",   "ModelFilePath.db2" },
    };

    int32_t CCommand_ModernInfo(const char* /*command*/, const char* /*arguments*/)
    {
        ConsolePrintf("modern-assets: decoders compiled in (WOWDECOMP_MODERN_ASSETS)");
        ConsolePrintf("modern-assets: FileDataID tables loaded: %s",
                      modern::db2::fdid::Ready() ? "yes" : "no");

        for (const Probe& probe : s_probes)
        {
            modern::db2::DB2File table(4);
            if (!table.Load(probe.file))
            {
                ConsolePrintf("  %-16s not present", probe.name);
                continue;
            }
            ConsolePrintf("  %-16s %u rows, id %d..%d", probe.name,
                          table.RowCount(), table.MinId(), table.MaxId());
        }

        ConsolePrintf("modern-assets: detail in Logs\\modern-assets.log");
        return 1;
    }

    int32_t CCommand_ModernLight(const char* /*command*/, const char* arguments)    {
        const modern::db2::light::StoreStatus& status = modern::db2::light::Status();
        if (status.failed)
        {
            ConsolePrintf("modernlight: the lighting tables did not load (%s)",
                          status.error.empty() ? "no reason given" : status.error.c_str());
            return 1;
        }

        ConsolePrintf("modernlight: light=%u lightdata=%u lightparams=%u",
                      status.lights, status.data, status.params);

        if (!arguments || !*arguments) return 1;

        const uint32_t lightId = static_cast<uint32_t>(std::strtoul(arguments, nullptr, 10));
        modern::db2::light::LightPoint point;
        if (!modern::db2::light::FindLight(lightId, point))
        {
            ConsolePrintf("modernlight: no light %u", lightId);
            return 1;
        }

        ConsolePrintf("light %u: coords=(%.1f, %.1f, %.1f) falloff %.1f..%.1f params=%u",
                      point.lightId, point.coords[0], point.coords[1], point.coords[2],
                      point.falloffStart, point.falloffEnd, point.paramsId);

        std::vector<const modern::db2::wdc5::Row*> curve;
        if (modern::db2::light::RowsForParams(point.paramsId, curve))
            ConsolePrintf("  %zu time stamp(s) for params %u", curve.size(), point.paramsId);
        else
            ConsolePrintf("  no time stamps for params %u", point.paramsId);

        return 1;
    }

    int32_t CCommand_ModernPath(const char* /*command*/, const char* arguments)
    {
        if (!arguments || !*arguments)
        {
            ConsolePrintf("usage: modernpath <fileDataId>");
            return 1;
        }

        const uint32_t fileDataId = static_cast<uint32_t>(std::strtoul(arguments, nullptr, 0));
        if (!fileDataId)
        {
            ConsolePrintf("modernpath: '%s' is not a file data id", arguments);
            return 1;
        }

        const char* path = modern::db2::fdid::ResolveTexture(fileDataId);
        if (!path)
        {
            ConsolePrintf("modernpath: %u does not resolve (unknown id, or no path tables mounted)",
                          fileDataId);
            return 1;
        }

        ConsolePrintf("modernpath: %u -> %s", fileDataId, path);
        return 1;
    }

    /// Loads a model through the real client path and reports what actually happened: whether the
    /// modern repack ran, and whether the resulting model has geometry the client can draw. This is
    /// the only way to tell "the bytes are well formed" from "the client can use it".
    int32_t CCommand_ModernM2(const char* /*command*/, const char* arguments)
    {
        if (!arguments || !*arguments)
        {
            ConsolePrintf("usage: modernm2 <model path>");
            return 1;
        }

        if (!World::s_m2Scene)
        {
            ConsolePrintf("modernm2: no model scene yet (enter the world or character select first)");
            MODERN_ASSETS_LOG_WARN("modernm2: no model scene yet");
            return 1;
        }

        const modern::m2::Stats before = modern::m2::GetStats();

        // Logged before the load: if CreateModel crashes or refuses, the log still names what was
        // being attempted, which is the only trace left after the client exits.
        MODERN_ASSETS_LOG_INFO("modernm2: loading '%s'", arguments);

        CM2Model* model = World::s_m2Scene->CreateModel(arguments, 0);
        if (!model)
        {
            ConsolePrintf("modernm2: the client could not create a model for '%s'", arguments);
            MODERN_ASSETS_LOG_WARN("modernm2: '%s' produced no model", arguments);
            return 1;
        }

        // Loading is asynchronous: CreateModel only schedules it. This call blocks until the model
        // (and its skin) are resident, which is also what makes the repack run, so it has to happen
        // BEFORE the stats are read -- otherwise every model looks like a stock load.
        const int32_t loaded = model->IsLoaded(1, 0);

        const modern::m2::Stats after = modern::m2::GetStats();
        const bool converted = after.modelsConverted != before.modelsConverted;

        ConsolePrintf("modernm2: '%s' loaded%s", arguments,
                      converted ? " via the modern repack" : " as a stock model");

        if (converted)
        {
            ConsolePrintf("  particles=%u cameras=%u textures=%u/%u gate=%u",
                          after.particlesNormalized - before.particlesNormalized,
                          after.camerasNormalized - before.camerasNormalized,
                          after.texturesResolved - before.texturesResolved,
                          (after.texturesResolved - before.texturesResolved) +
                              (after.texturesUnresolved - before.texturesUnresolved),
                          after.shadowGateForced - before.shadowGateForced);
        }

        // GetNumBatches and GetNumGeometryBatches both count only the batches whose skin section is
        // currently VISIBLE, and a freshly created model starts with every geoset hidden -- so both
        // report 0 for a perfectly good model. The meaningful figure is what the attached skin
        // actually carries, read straight off the shared skin profile.
        const M2SkinProfile* skin = model->m_shared ? model->m_shared->m_skinData : nullptr;
        const uint32_t drawable = skin ? skin->m_batches.m_count : 0;
        const uint32_t sections = skin ? skin->m_skinSections.m_count : 0;
        const uint32_t batches = model->GetNumBatches();
        const CAaBox bounds = model->GetBoundingBox();

        ConsolePrintf("  skinBatches=%u sections=%u visible=%u loaded=%s bounds=(%.2f %.2f %.2f)..(%.2f %.2f %.2f)",
                      drawable, sections, batches, loaded ? "yes" : "no",
                      bounds.b.x, bounds.b.y, bounds.b.z,
                      bounds.t.x, bounds.t.y, bounds.t.z);

        // Every finding goes to the log as well: a console line scrolls away, and this is the result a
        // remote reader (or a later session) has to be able to check.
        MODERN_ASSETS_LOG_INFO("modernm2: '%s' %s: skinBatches=%u sections=%u visible=%u loaded=%s bounds=(%.2f %.2f %.2f)..(%.2f %.2f %.2f)",
                               arguments, converted ? "repacked" : "stock load",
                               drawable, sections, batches, loaded ? "yes" : "no",
                               bounds.b.x, bounds.b.y, bounds.b.z,
                               bounds.t.x, bounds.t.y, bounds.t.z);

        if (!drawable)
        {
            ConsolePrintf("modernm2: the model carries no batches; see Logs\\modern-assets.log");
            return 1;
        }

        model->Release();
        return 1;
    }
}

void ModernAssetsRegisterCommands()
{
    ConsoleCommandRegister("moderninfo", CCommand_ModernInfo, CATEGORY_DEBUG,
                           "Report the modern-asset readers and the DB2 tables they can see.");
    ConsoleCommandRegister("modernpath", CCommand_ModernPath, CATEGORY_DEBUG,
                           "Resolve a FileDataID to the client path it names: modernpath <id>.");
    ConsoleCommandRegister("modernlight", CCommand_ModernLight, CATEGORY_DEBUG,
                           "Report the retail lighting tables, or one light point: modernlight [id].");
    ConsoleCommandRegister("modernm2", CCommand_ModernM2, CATEGORY_DEBUG,
                           "Load a model and report whether the modern repack ran: modernm2 <path>.");
}

#else

void ModernAssetsRegisterCommands() {}

#endif
