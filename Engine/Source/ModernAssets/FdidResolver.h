#ifndef ENGINE_SOURCE_MODERNASSETS_FDIDRESOLVER_H
#define ENGINE_SOURCE_MODERNASSETS_FDIDRESOLVER_H

#include <cstdint>
#include <string>

// FileDataID -> client path resolution, backed by the game's own TextureFilePath / ModelFilePath /
// TextureFileData tables, read through the client's archive set.
//
// Ported from WarcraftXL wxl-db2 api/FdidResolver.{hpp,cpp} (GPL-3.0-or-later, Copyright (C) 2026
// WarcraftXL). Modern content stopped naming most assets by path: a texture or a model is a number
// (a FileDataID) and a table maps it to the file. Textures and models share one FileDataID space, so
// either kind resolves through the same call.
//
// Every entry point is lazy (the first query loads the tables) and safe to call more than once.
// Resolve returns a pointer that stays valid for the process lifetime, which is what lets it be
// stored straight into a model's texture record.
namespace modern::db2::fdid
{
    void EnsureLoaded();

    /// FileDataID -> path, cached. nullptr when the id is unknown or the tables are absent.
    const char* ResolveTexture(uint32_t fileDataId);
    const char* ResolveModel(uint32_t fileDataId);

    /// Model path -> FileDataID (0 when unknown). Case, slash direction and extension are folded.
    uint32_t ResolveModelId(const char* modelPath);

    /// True once the path tables have loaded and produced at least one row.
    bool Ready();
}

#endif
