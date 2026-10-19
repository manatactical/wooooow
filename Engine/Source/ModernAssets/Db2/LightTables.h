#ifndef ENGINE_SOURCE_MODERNASSETS_DB2_LIGHTTABLES_H
#define ENGINE_SOURCE_MODERNASSETS_DB2_LIGHTTABLES_H

#include "Engine/Source/ModernAssets/Db2/Db2Table.h"

#include <cstdint>

// The lighting tables, as the first real consumers of the declarative WDC5 reader.
//
// Ported from WarcraftXL wxl-db2 schemas/LightStore.{hpp,cpp} (GPL-3.0-or-later, Copyright (C) 2026
// WarcraftXL). Only the table declarations and their typed accessors are here; the day-curve
// evaluation and the per-frame cache the upstream project layers on top are not ported yet.
//
// Declaring these tables is what proves the reader: three real retail tables (5, 47 and 32 fields,
// with bitpacked, common-data and palletised columns and a relationship between two of them) decode
// through the same code path a caller's own Definition would.
namespace modern::db2::light
{
    /// One row of Light.db2: a light point in the world and the parameter set it names.
    struct LightPoint
    {
        uint32_t lightId = 0;
        float    coords[3]{};
        float    falloffStart = 0.0f;
        float    falloffEnd = 0.0f;
        uint32_t continentId = 0;
        uint32_t paramsId = 0;
    };

    /// One row of LightData.db2: every colour and fog figure for one parameter set at one time stamp.
    struct LightDataRow
    {
        uint32_t lightParamId = 0;
        float    time = 0.0f;
        uint32_t directColor = 0;
        uint32_t ambientColor = 0;
        float    fogEnd = 0.0f;
        float    fogDensity = 0.0f;
    };

    /// One row of LightParams.db2: the switches and sun geometry for a parameter set.
    struct LightParamsRow
    {
        uint32_t paramsId = 0;
        uint32_t flags = 0;
        float    sunPolar = 0.0f;
        float    sunAzimuth = 0.0f;
    };

    struct StoreStatus
    {
        bool     loaded = false;
        bool     failed = false;
        std::string error;
        uint32_t lights = 0;
        uint32_t data = 0;
        uint32_t params = 0;
    };

    /// Loads the three tables on first use (lazy, once). Safe to call from any thread.
    const StoreStatus& Status();

    /**
     * @brief Decodes the three tables from bytes already in memory instead of the archive set.
     *
     * The client always goes through the archives (Status()); this entry exists so the very same
     * definitions can be decoded offline, from loose files, by a test or a tool.
     */
    StoreStatus LoadFrom(const uint8_t* light, size_t lightSize,
                         const uint8_t* data, size_t dataSize,
                         const uint8_t* params, size_t paramsSize);

    /// Raw access to a loaded table, or null until the first Status()/Load attempt.
    const Table* Lights();
    const Table* Data();
    const Table* Params();

    /// Typed single-row lookups. @return false when the table is absent or holds no such row.
    bool FindLight(uint32_t lightId, LightPoint& out);
    bool FindParams(uint32_t paramsId, LightParamsRow& out);

    /// Returns every time-stamp row for one parameter set, in file order.
    bool RowsForParams(uint32_t paramsId, std::vector<const wdc5::Row*>& out);
}

#endif
