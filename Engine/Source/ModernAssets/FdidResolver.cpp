#include "Engine/Source/ModernAssets/FdidResolver.h"

#include "Engine/Source/ModernAssets/Db2/Db2File.h"
#include "Engine/Source/ModernAssets/ModernAssetsLog.h"

#include <atomic>
#include <cctype>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace modern::db2::fdid
{
    namespace
    {
        // TextureFilePath.db2 and ModelFilePath.db2 both decode to {id, stringOffset}.
        struct PathRow { int32_t id; int32_t path; };

        // A path table keeps its strings in the table's own string block. Declaring the column lets
        // the decoder rebase the offset on the multi-section / WDC2+ layouts; on the single-section
        // WDC1 layout that ships today the decoder already stores an absolute offset and ignores this.
        class PathTable : public DB2Table<PathRow>
        {
        protected:
            const uint32_t* StringColumns(uint32_t* count) const override
            {
                static const uint32_t columns[] = { 1 };
                *count = 1;
                return columns;
            }
        };

        PathTable g_texturePaths;
        PathTable g_modelPaths;
        std::unordered_map<std::string, uint32_t> g_modelIdByStem;

        std::once_flag g_once;
        std::atomic<bool> g_ready{ false };

        /// Resolves a row's string offset, rejecting a negative or out-of-range one.
        const char* RowPath(const PathTable& table, const PathRow& row)
        {
            if (row.path < 0) return nullptr;
            const char* path = table.Str(static_cast<uint32_t>(row.path));
            return (path && *path) ? path : nullptr;
        }

        /// Lowercases, folds forward slashes to backslashes and drops any extension, so the one
        /// spelling the path table stores and the many a caller can hold collapse to the same key.
        std::string NormalizeStem(const char* path)
        {
            std::string s;
            for (const char* p = path; *p; ++p)
            {
                const char c = *p;
                s.push_back(c == '/' ? '\\'
                                     : static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
            }
            const size_t dot = s.find_last_of('.');
            const size_t sep = s.find_last_of('\\');
            if (dot != std::string::npos && (sep == std::string::npos || dot > sep)) s.resize(dot);
            return s;
        }

        void LoadTables()
        {
            g_texturePaths.Load("TextureFilePath.db2");
            g_modelPaths.Load("ModelFilePath.db2");

            g_modelIdByStem.clear();
            g_modelIdByStem.reserve(g_modelPaths.RowCount());
            for (uint32_t i = 0; i < g_modelPaths.RowCount(); ++i)
            {
                const PathRow* row = g_modelPaths.At(i);
                if (!row) continue;
                const char* path = RowPath(g_modelPaths, *row);
                // First spelling wins: two rows collapsing to one stem differ only by container
                // extension, and either id resolves to the same asset for every consumer here.
                if (path) g_modelIdByStem.emplace(NormalizeStem(path), static_cast<uint32_t>(row->id));
            }

            const bool ready = g_texturePaths.RowCount() != 0 || g_modelPaths.RowCount() != 0;
            g_ready.store(ready, std::memory_order_release);

            MODERN_ASSETS_LOG_INFO("fdid: texture paths=%u model paths=%u (model stems=%zu)%s",
                                   g_texturePaths.RowCount(), g_modelPaths.RowCount(),
                                   g_modelIdByStem.size(),
                                   ready ? "" : " -- no path tables, FileDataID resolution disabled");
        }

        // The resolved string is stored in a deque, which never relocates or frees an element it
        // already holds, so the pointer handed to a caller stays valid for the process lifetime even
        // while other ids are being resolved (and cached) concurrently. The id -> pointer map is only
        // touched under the mutex; nothing reads it afterwards.
        std::mutex                          g_cacheMutex;
        std::unordered_map<uint32_t, const char*> g_cache;
        std::deque<std::string>             g_paths;

        bool LookupPath(uint32_t fileDataId, const char*& outPath)
        {
            const PathRow* row = g_texturePaths.Find(static_cast<int32_t>(fileDataId));
            const char* path = row ? RowPath(g_texturePaths, *row) : nullptr;

            if (!path)
            {
                row = g_modelPaths.Find(static_cast<int32_t>(fileDataId));
                path = row ? RowPath(g_modelPaths, *row) : nullptr;
            }
            if (!path) return false;

            outPath = path;
            return true;
        }

        const char* Resolve(uint32_t fileDataId)
        {
            if (fileDataId == 0) return nullptr;
            EnsureLoaded();
            if (!g_ready.load(std::memory_order_acquire)) return nullptr;

            std::lock_guard<std::mutex> lock(g_cacheMutex);
            auto it = g_cache.find(fileDataId);
            if (it != g_cache.end()) return it->second;

            // The id is looked up once and the answer kept: a texture shared by hundreds of tiles is
            // one lookup, and a confirmed miss is remembered as nullptr rather than re-searched.
            const char* found = nullptr;
            if (!LookupPath(fileDataId, found))
            {
                g_cache.emplace(fileDataId, nullptr);
                return nullptr;
            }

            g_paths.emplace_back(found);
            const char* stable = g_paths.back().c_str();
            g_cache.emplace(fileDataId, stable);
            return stable;
        }
    }

    void EnsureLoaded()
    {
        std::call_once(g_once, LoadTables);
    }

    bool Ready()
    {
        EnsureLoaded();
        return g_ready.load(std::memory_order_acquire);
    }

    const char* ResolveTexture(uint32_t fileDataId) { return Resolve(fileDataId); }
    const char* ResolveModel(uint32_t fileDataId)   { return Resolve(fileDataId); }

    uint32_t ResolveModelId(const char* modelPath)
    {
        if (!modelPath || !*modelPath) return 0;
        EnsureLoaded();
        if (!g_ready.load(std::memory_order_acquire)) return 0;

        const auto it = g_modelIdByStem.find(NormalizeStem(modelPath));
        return it == g_modelIdByStem.end() ? 0 : it->second;
    }
}
