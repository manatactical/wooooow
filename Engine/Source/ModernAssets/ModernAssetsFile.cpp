#include "Engine/Source/ModernAssets/ModernAssetsFile.h"

#include "Engine/Source/ModernAssets/ModernAssetsLog.h"
#include "Engine/Source/BlizzardCore/SFile2-Core.h"
#include "Engine/Source/Storm/H/storm.h"

namespace modern::assets
{
    bool ReadWholeFile(const char* fileName, std::vector<uint8_t>& bytes)
    {
        bytes.clear();
        if (!fileName || !*fileName) return false;

        char path[300];
        SStrPrintf(path, sizeof(path), "%s", fileName);
        path[sizeof(path) - 1] = '\0';

        SFileImpl* file = nullptr;
        if (!SFileOpenFile(path, &file) || !file) return false;

        uint32_t high = 0;
        const uint32_t size = SFileGetFileSize(file, &high);
        if (!size || high)
        {
            // A file past 4 GiB cannot be a single table or model this reader handles; treating it as
            // absent keeps the caller's "not found" path honest instead of silently truncating it.
            SFileCloseFile(file);
            return false;
        }

        bytes.resize(size);
        // A read may legitimately come back short; loop until the buffer is full or the read makes no
        // progress, rather than discarding a partial (valid) file.
        uint32_t total = 0;
        bool ok = true;
        while (total < size)
        {
            uint32_t got = 0;
            if (!SFileReadFile(file, bytes.data() + total, size - total, &got, nullptr, 0) || !got)
            {
                ok = false;
                break;
            }
            total += got;
        }
        SFileCloseFile(file);

        if (!ok)
        {
            MODERN_ASSETS_LOG_WARN("file: %s short read (%u of %u byte(s))", path, total, size);
            bytes.clear();
            return false;
        }
        return true;
    }
}
