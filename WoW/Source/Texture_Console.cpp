#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <windows.h>

#include "WoW/Source/Texture_Console.h"

#include "Engine/Source/Base/Status.h"
#include "Engine/Source/BlizzardCore/SFile2-Core.h"
#include "Engine/Source/BLPFile/blp.h"
#include "Engine/Source/BLPFile/Types.h"
#include "Engine/Source/ConsoleBase/ConsoleCommand.h"
#include "Engine/Source/ConsoleBase/IConsole.h"
#include "Engine/Source/Event/PerfLog.h"
#include "Engine/Source/Gx/Blit.h"
#include "Engine/Source/Gx/Gx.h"
#include "Engine/Source/Images/MippedImg.h"
#include "Engine/Source/Images/tga.h"
#include "Engine/Source/Os/OsFile.h"
#include "Engine/Source/Services/Texture.h"
#include "Engine/Source/Storm/H/storm.h"
#include "Engine/Source/Tempest/CImVector.h"

// Texture diagnostics for the "some textures come out blue / yellow / green" failures.
//
// Two explanations look identical on screen and must be told apart:
//
//   1. the client decoded the file wrong -- bad BLP header, wrong mip stride, DXT decode;
//   2. the bytes were right and the upload, format selection or shader is wrong.
//
// `texdump` writes the pixels the GPU upload would read as a TGA, beside the untouched file bytes. A
// garbled TGA means (1); a clean TGA on a texture that still draws wrong means (2). `texstat` and
// `texlist` cover format selection and lifetimes; `texaudit` checks each BLP header against the bytes
// that actually arrived, at load time, which is the only way to catch a bad mip table without a repro.
//
// Everything is read-only: nothing here uploads, marks a texture for update, or touches render state.
// Output goes to Logs\texdump\. Latching a mip calls the texture's own GxTex user func, which does not
// change what it hands back, so this cannot disturb a loading texture.

namespace
{
    const char* const kLogPath = "Logs\\texaudit.log";
    const char* const kDumpDir = "Logs\\texdump";

    void AuditLog(const char* format, ...) {
        char message[1024];
        va_list args;
        va_start(args, format);
        std::vsnprintf(message, sizeof(message), format, args);
        va_end(args);

        // Loads run on the async workers, so the console and the file are both written under one lock.
        // A function-local static is initialised once, thread-safely, before any second caller arrives.
        struct Sink {
            CRITICAL_SECTION m_section;
            Sink() { InitializeCriticalSection(&m_section); }
        };

        static Sink s_sink;

        EnterCriticalSection(&s_sink.m_section);
        ConsolePrintf("texture-audit: %s", message);
        OutputDebugStringA(message);
        OutputDebugStringA("\n");

        FILE* file = DiagnosticLogsEnabled() ? std::fopen(kLogPath, "a") : nullptr;

        if (file) {
            std::fprintf(file, "%s\n", message);
            std::fclose(file);
        }

        LeaveCriticalSection(&s_sink.m_section);
    }

    const char* FormatName(EGxTexFormat format) {
        // STATIC
        static const char* const names[GxTexFormats_Last] = {
            "Unknown", "Abgr8888", "Argb8888", "Argb4444", "Argb1555", "Rgb565",
            "Dxt1", "Dxt3", "Dxt5", "Uv88", "Gr1616F", "R32F", "D24X8",
        };

        return static_cast<uint32_t>(format) < GxTexFormats_Last ? names[format] : "?";
    }

    const char* PixelFormatName(uint32_t format) {
        // STATIC
        static const char* const names[NUM_PIXEL_FORMATS] = {
            "Dxt1", "Dxt3", "Argb8888", "Argb1555", "Argb4444", "Rgb565",
            "A8", "Dxt5", "Unspecified", "Argb2565",
        };

        return format < NUM_PIXEL_FORMATS ? names[format] : "?";
    }

    const char* TargetName(EGxTexTarget target) {
        switch (target) {
            case GxTex_2d: return "2d";
            case GxTex_CubeMap: return "cube";
            case GxTex_Rectangle: return "rect";
            case GxTex_NonPow2: return "np2";
            default: return "?";
        }
    }

    /// Case-insensitive substring match on the texture name; an empty needle matches everything.
    bool NameMatches(const char* name, const char* needle) {
        if (!needle || !*needle) {
            return true;
        }

        const size_t needleLen = std::strlen(needle);

        for (const char* at = name; *at; at++) {
            if (!SStrCmpI(at, needle, needleLen)) {
                return true;
            }
        }

        return false;
    }

    /// A flat, lowercase, path-safe dump name for a client path. Only ASCII alphanumerics and '_'
    /// survive, so an archive name can never steer the write outside the dump directory.
    void DumpFileName(const char* name, char* buffer, uint32_t size, const char* suffix) {
        uint32_t at = 0;

        for (const char* p = name; *p && at + 1 < size && at < 96; p++) {
            const char c = *p;
            const bool keep = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                              (c >= '0' && c <= '9') || c == '_';

            if (keep) {
                buffer[at++] = (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
            } else if (at && buffer[at - 1] != '_') {
                buffer[at++] = '_';
            }
        }

        if (!at) {
            SStrCopy(buffer, "unnamed", size);
            at = 7;
        }

        buffer[at] = '\0';

        if (at < size) {
            SStrPrintf(buffer + at, size - at, "%s", suffix);
        }
    }

    /// Reads a whole file out of the mounted archives. Caller frees with SMemFree.
    bool ReadArchiveFile(const char* name, void*& data, uint32_t& size) {
        data = nullptr;
        size = 0;

        SFileImpl* file = TextureCreateOpenFile(name, 0);

        if (!file) {
            return false;
        }

        size = SFileGetFileSize(file, nullptr);

        if (!size) {
            SFileCloseFile(file);
            return false;
        }

        data = SMemAlloc(size, __FILE__, __LINE__, 0);

        if (!data) {
            SFileCloseFile(file);
            return false;
        }

        uint32_t read = 0;
        const bool ok = SFileReadFile(file, data, size, &read, nullptr, 0) && read == size;
        SFileCloseFile(file);

        if (!ok) {
            SMemFree(data, __FILE__, __LINE__, 0);
            data = nullptr;
            size = 0;
        }

        return ok;
    }

    bool WriteBlob(const char* path, const void* data, uint32_t size) {
        HOSFILE file = OsCreateFile(path, OS_GENERIC_WRITE, 0, OS_CREATE_ALWAYS,
                                    OS_FILE_ATTRIBUTE_NORMAL, OS_FILE_TYPE_ANY);

        if (file == OS_INVALID_FILE) {
            return false;
        }

        uint32_t written = 0;
        const bool ok = OsWriteFile(file, data, size, &written) && written == size;
        OsCloseFile(file);

        if (!ok) {
            OsDeleteFile(path);
        }

        return ok;
    }

    /// Decodes one latched mip to a top-down 32-bit image and writes it as a TGA.
    bool WriteMipTga(const char* path, const TDebugMip& mip) {
        // A file header can claim any dimensions; a level that cannot be a real TGA is refused rather
        // than truncated into a misleading picture.
        if (!mip.m_texels || !mip.m_width || !mip.m_height ||
            mip.m_width > 16384 || mip.m_height > 16384) {
            return false;
        }

        const size_t rowBytes = static_cast<size_t>(mip.m_width) * 4;
        CImVector* image = static_cast<CImVector*>(SMemAlloc(rowBytes * mip.m_height,
                                                             __FILE__, __LINE__, 0));
        if (!image) {
            return false;
        }

        const C2iVector size = { static_cast<int32_t>(mip.m_width), static_cast<int32_t>(mip.m_height) };
        const bool decoded = Blit(size, BlitAlpha_0, mip.m_texels, mip.m_strideInBytes,
                                  GxGetBlitFormat(mip.m_dataFormat), image, static_cast<uint32_t>(rowBytes),
                                  BlitFormat_Argb8888) != 0;

        bool wrote = false;

        if (decoded) {
            // SetImage copies the bits, so the decode buffer can go once it has returned.
            CTgaFile tga;
            wrote = tga.SetImage(image, static_cast<uint16_t>(mip.m_width), static_cast<uint16_t>(mip.m_height),
                                 32, 8, 1, 0) && tga.Write(path) != 0;
        }

        SMemFree(image, __FILE__, __LINE__, 0);
        return wrote;
    }
}

// ---- audit -----------------------------------------------------------------------------------------

namespace
{
    /// The byte size level `level` needs, by the same rules the image allocator uses, so a header that
    /// under-describes a level is caught before the loader reads past it.
    uint32_t HeaderLevelBytes(const BLPHeader& header, uint32_t level) {
        if (header.m_colorEncoding == 2) {
            return CalcLevelSize(level, header.m_width, header.m_height,
                                 static_cast<PIXEL_FORMAT>(header.m_preferredFormat));
        }

        // CalcLevelSize carries the same whole-block rounding for DXT; use it for the paletted case too
        // rather than repeating the shift, which double-shifts on a zero dimension.
        const uint32_t width = header.m_width >> level;
        const uint32_t height = header.m_height >> level;
        return (width ? width : 1) * (height ? height : 1);
    }

    void AuditBlp(CTexture* texture, const void* data, uint32_t bytes, void* /*arg*/) {
        if (!texture || !texture->m_filename[0]) {
            return;
        }

        if (!data || bytes < sizeof(BLPHeader)) {
            AuditLog("'%s': %u byte(s) is smaller than a BLP header", texture->m_filename, bytes);
            return;
        }

        BLPHeader header;
        std::memcpy(&header, data, sizeof(header));

        if (header.m_magic != 0x32504C42 || header.m_formatVersion != 1) {
            AuditLog("'%s': magic %08x version %u is not BLP2 v1",
                     texture->m_filename, header.m_magic, header.m_formatVersion);
            return;
        }

        if (header.m_preferredFormat >= NUM_PIXEL_FORMATS) {
            AuditLog("'%s': preferredFormat %u is outside the 0..%u table",
                     texture->m_filename, header.m_preferredFormat, NUM_PIXEL_FORMATS - 1);
            return;
        }

        if (header.m_colorEncoding == 2) {
            const PIXEL_FORMAT preferred = static_cast<PIXEL_FORMAT>(header.m_preferredFormat);

            if (preferred != PIXEL_DXT1 && preferred != PIXEL_DXT3 && preferred != PIXEL_DXT5) {
                AuditLog("'%s': DXT-encoded but names preferredFormat %s",
                         texture->m_filename, PixelFormatName(header.m_preferredFormat));
                return;
            }
        }

        const uint32_t levels = (header.m_hasMips & 0xF) ? CalcLevelCount(header.m_width, header.m_height) : 1;

        // BLPHeader carries 16 mip slots; a header that claims more levels than that is itself the bug,
        // and reading past the array would be this tool's bug.
        if (levels > 16) {
            AuditLog("'%s': header claims %u mip levels, the format holds 16", texture->m_filename, levels);
        }

        for (uint32_t level = 0; level < levels && level < 16; level++) {
            const uint32_t offset = header.m_mipOffsets[level];
            const uint32_t stored = header.m_mipSizes[level];
            const uint32_t needed = HeaderLevelBytes(header, level);

            if (!stored) {
                AuditLog("'%s': mip %u has size 0 (offset %u)", texture->m_filename, level, offset);
            } else if (offset > bytes || stored > bytes - offset) {
                AuditLog("'%s': mip %u runs past the end: offset %u + size %u > %u",
                         texture->m_filename, level, offset, stored, bytes);
            } else if (stored < needed) {
                // The happy-looking case that is still wrong: the loader copies `needed` bytes and the
                // tail is whatever follows the level. That is the streaked, colour-shifted look.
                AuditLog("'%s': mip %u holds %u byte(s), but %ux%u %s needs %u",
                         texture->m_filename, level, stored, header.m_width, header.m_height,
                         PixelFormatName(header.m_preferredFormat), needed);
            }
        }
    }

    /// Decodes one mip of a BLP straight from its file bytes, exactly as the loader does, and writes a
    /// TGA. Going through the archive bytes rather than the live texture is deliberate: the async path
    /// keeps DXT mips pointing into the file buffer it frees after upload, so latching a resident
    /// texture is a dangling read. The decode itself is the same code either way.
    bool DecodeBlpToTga(const void* raw, uint32_t rawSize, uint32_t level, const char* path,
                        PIXEL_FORMAT& format, uint32_t& width, uint32_t& height) {
        CBLPFile image;

        if (!image.Source(const_cast<void*>(raw))) {
            return false;
        }

        if (level >= image.m_numLevels) {
            return false;
        }

        const PIXEL_FORMAT preferred = static_cast<PIXEL_FORMAT>(image.m_header.m_preferredFormat);

        // Paletted files still have to be expanded for a dump; DXT ones are copied whole and decoded
        // below. Anything else falls back to 32-bit, which Blit understands.
        format = image.m_header.m_colorEncoding == 1 ? PIXEL_ARGB8888 : preferred;

        if (format == PIXEL_UNSPECIFIED || format == PIXEL_A8 || format == PIXEL_ARGB2565) {
            format = PIXEL_ARGB8888;
        }

        MipBits* images = nullptr;

        if (!image.LockChain(format, images, level) || !images) {
            return false;
        }

        width = image.Width(level);
        height = image.Height(level);

        TDebugMip mip;
        mip.m_texels = images[0].m_mip[0];
        mip.m_width = width;
        mip.m_height = height;
        mip.m_strideInBytes = GxCalcTexelStrideInBytes(TexturePixelFormatToGxTexFormat(format), width);
        mip.m_dataFormat = TexturePixelFormatToGxTexFormat(format);

        const bool wrote = WriteMipTga(path, mip);
        SMemFree(images, __FILE__, __LINE__, 0);

        return wrote;
    }

    // Written from the async load workers, read by `texstat` on the frame thread. A plain increment is
    // deliberately kept: it is a debug counter, and a lost update on a display-only figure is not worth
    // an interlocked in the loader path.
    // shortcut: approximate under concurrent loads, fine for "did the audit run".
    volatile uint32_t s_auditCount;

    void AuditEnabled(CTexture* texture, const void* data, uint32_t bytes, void* arg) {
        s_auditCount++;
        AuditBlp(texture, data, bytes, arg);
    }
}

// ---- commands --------------------------------------------------------------------------------------

namespace
{
    int32_t CCommand_TexStat(const char* /*command*/, const char* /*arguments*/) {
        const uint32_t count = TextureDebugCount();

        uint32_t perFormat[GxTexFormats_Last] = {};
        uint32_t mismatched = 0;
        uint32_t noMipBits = 0;
        uint32_t pending = 0;
        uint32_t failed = 0;
        uint32_t onAtlas = 0;
        uint32_t opaque = 0;
        uint32_t cube = 0;

        for (uint32_t i = 0; i < count; i++) {
            CTexture* texture = TextureDebugAt(i);

            if (!texture) {
                continue;
            }

            if (static_cast<uint32_t>(texture->m_gxTexFormat) < GxTexFormats_Last) {
                perFormat[texture->m_gxTexFormat]++;
            }

            // The data format drives stride and block mode; a texture whose latched data format and GPU
            // format disagree is the classic source of a channel-swapped or striped upload. DXT counts:
            // the reported colour corruption is DXT-fed, so excluding it would hide the case that matters.
            if (texture->m_gxTexFormat != GxTex_Unknown && texture->m_dataFormat != GxTex_Unknown &&
                texture->m_gxTexFormat != texture->m_dataFormat) {
                mismatched++;
            }

            if (!texture->m_mipBits) {
                noMipBits++;
            }

            if (texture->m_asyncObject) {
                pending++;
            }

            if (!texture->m_status.IsEmpty()) {
                failed++;
            }

            if (texture->m_atlas) {
                onAtlas++;
            }

            if (texture->m_flags & 1) {
                opaque++;
            }

            if (texture->m_gxTexTarget == GxTex_CubeMap) {
                cube++;
            }
        }

        ConsolePrintf("textures: %u alive, %u opaque, %u cube, %u atlas, %u pending async, %u with status",
                      count, opaque, cube, onAtlas, pending, failed);
        ConsolePrintf("  m_mipBits null on %u (DXT latch falls back to the shared s_mipBits scratch)",
                      noMipBits);
        ConsolePrintf("  gx/data format mismatches: %u", mismatched);
        ConsolePrintf("  BLP headers audited this session: %u", s_auditCount);

        for (uint32_t format = 0; format < GxTexFormats_Last; format++) {
            if (perFormat[format]) {
                ConsolePrintf("  %-10s %u", FormatName(static_cast<EGxTexFormat>(format)), perFormat[format]);
            }
        }

        return 1;
    }

    int32_t CCommand_TexList(const char* /*command*/, const char* arguments) {
        const uint32_t count = TextureDebugCount();
        uint32_t shown = 0;

        for (uint32_t i = 0; i < count; i++) {
            CTexture* texture = TextureDebugAt(i);

            if (!texture || !NameMatches(texture->m_filename, arguments)) {
                continue;
            }

            char status[256];
            status[0] = '\0';
            texture->m_status.GetErrorStr(status, sizeof(status), STATUS_WARNING, "; ");

            const char* state = texture->m_asyncObject ? (texture->m_gxTex ? "loading" : "queued")
                                 : (texture->m_gxTex ? "ready" : "no-gx");

            ConsolePrintf("%-40s %-11s %-4s %ux%u gx=%s data=%s a=%u mip=%u flags=%u%s%s",
                          texture->m_filename, state, TargetName(texture->m_gxTexTarget),
                          texture->m_gxWidth, texture->m_gxHeight,
                          FormatName(texture->m_gxTexFormat), FormatName(texture->m_dataFormat),
                          texture->m_alphaBits, texture->m_bestMip, texture->m_flags,
                          texture->m_atlas ? " atlas" : "",
                          status[0] ? status : "");

            shown++;
        }

        ConsolePrintf("texlist: %u of %u texture(s)%s", shown, count,
                      arguments && *arguments ? " matched" : "");

        return 1;
    }

    int32_t CCommand_TexDump(const char* /*command*/, const char* arguments) {
        if (!arguments || !*arguments) {
            ConsolePrintf("usage: texdump <name substring> [mip level]");
            return 1;
        }

        // A trailing all-numeric token is the mip level; anything else is part of the name, so a filter
        // like "interface icons" still works.
        char needle[260];
        SStrCopy(needle, arguments, sizeof(needle));

        uint32_t level = 0;
        char* space = std::strrchr(needle, ' ');

        if (space) {
            char* end = nullptr;
            const unsigned long parsed = std::strtoul(space + 1, &end, 10);

            if (end && *end == '\0' && space[1] >= '0' && space[1] <= '9') {
                *space = '\0';
                level = static_cast<uint32_t>(parsed);
            }
        }

        OsCreateDirectory("Logs", 0);

        if (!OsDirectoryExists(kDumpDir) && !OsCreateDirectory(kDumpDir, 0)) {
            ConsolePrintf("texdump: cannot create %s", kDumpDir);
            return 1;
        }

        const uint32_t count = TextureDebugCount();
        uint32_t dumped = 0;

        for (uint32_t i = 0; i < count; i++) {
            CTexture* texture = TextureDebugAt(i);

            if (!texture || !NameMatches(texture->m_filename, needle)) {
                continue;
            }

            char base[260];
            char path[320];
            DumpFileName(texture->m_filename, base, sizeof(base), "");
            SStrPrintf(path, sizeof(path), "%s\\%s-mip%u.tga", kDumpDir, base, level);

            // The untouched bytes, so the same texture can be checked against BLPConverter or retail
            // tooling without guessing whether the client's parse or its decode is at fault.
            void* raw = nullptr;
            uint32_t rawSize = 0;
            char rawPath[320];
            SStrPrintf(rawPath, sizeof(rawPath), "%s\\%s-raw.blp", kDumpDir, base);

            if (!ReadArchiveFile(texture->m_filename, raw, rawSize)) {
                ConsolePrintf("texdump: '%s': not readable from the archives (a generated or substituted texture)",
                              texture->m_filename);
                continue;
            }
            const bool wroteBlp = WriteBlob(rawPath, raw, rawSize);

            PIXEL_FORMAT format = PIXEL_ARGB8888;
            uint32_t width = 0;
            uint32_t height = 0;
            const bool wroteTga = DecodeBlpToTga(raw, rawSize, level, path, format, width, height);

            ConsolePrintf("texdump: '%s' mip %u %ux%u decoded from %u raw byte(s): tga %s, raw %s",
                          texture->m_filename, level, width, height, rawSize,
                          wroteTga ? "written" : "FAILED", wroteBlp ? "written" : "FAILED");

            SMemFree(raw, __FILE__, __LINE__, 0);

            if (wroteTga) dumped++;
        }

        ConsolePrintf("texdump: %u texture(s) written to %s", dumped, kDumpDir);
        return 1;
    }

    int32_t CCommand_AssetGet(const char* /*command*/, const char* arguments) {
        if (!arguments || !*arguments) {
            ConsolePrintf("usage: assetget <client path> [outfile]");
            return 1;
        }

        char path[260];
        SStrCopy(path, arguments, sizeof(path));

        // A path may contain spaces, so the destination is only split off when the trailing token names a
        // file the caller wants written.
        const char* out = nullptr;
        char* space = std::strrchr(path, ' ');

        if (space && std::strchr(space + 1, '.') != nullptr) {
            *space = '\0';
            out = space + 1;
        }

        OsCreateDirectory("Logs", 0);

        char fallback[320];

        if (!out) {
            char base[260];
            DumpFileName(path, base, sizeof(base), "");
            SStrPrintf(fallback, sizeof(fallback), "Logs\\%s.bin", base);
            out = fallback;
        }

        void* data = nullptr;
        uint32_t size = 0;

        if (!ReadArchiveFile(path, data, size)) {
            ConsolePrintf("assetget: '%s' is not in the mounted archives", path);
            return 1;
        }

        const bool wrote = WriteBlob(out, data, size);
        ConsolePrintf("assetget: '%s' -> %s (%u bytes)%s", path, out, size, wrote ? "" : " WRITE FAILED");
        SMemFree(data, __FILE__, __LINE__, 0);

        return 1;
    }

    int32_t CCommand_TexAudit(const char* /*command*/, const char* arguments) {
        const bool enable = !arguments || !*arguments || std::strtoul(arguments, nullptr, 10) != 0;

        s_auditCount = 0;
        TextureSetAuditCallback(enable ? AuditEnabled : nullptr, nullptr);

        ConsolePrintf("texaudit: header checks %s; findings go to %s", enable ? "on" : "off",
                     DiagnosticLogsEnabled() ? kLogPath : "the debugger only (WOWLOG_QUIET is set)");

        // Write the state down immediately, flushed. Console text is lost when the client is closed, and the
        // point of this command is to survive exactly that: it says whether the hook was on at all, which is
        // the first thing to check when the log turns out to be empty.
        if (DiagnosticLogsEnabled()) {
            OsCreateDirectory("Logs", 0);
        }

        FILE* file = DiagnosticLogsEnabled() ? std::fopen(kLogPath, "a") : nullptr;

        if (file) {
            std::fprintf(file, "texaudit: header checks %s (first load will be reported below)\n",
                         enable ? "on" : "off");
            std::fclose(file);
        } else if (DiagnosticLogsEnabled()) {
            ConsolePrintf("texaudit: cannot write %s", kLogPath);
        }

        return 1;
    }
}

void TextureRegisterCommands() {
    ConsoleCommandRegister("texstat", CCommand_TexStat, CATEGORY_DEBUG,
                           "Summarise live textures: formats, pending loads, dead mip pointers.");
    ConsoleCommandRegister("texlist", CCommand_TexList, CATEGORY_DEBUG,
                           "List live textures, optionally filtered: texlist [name substring].");
    ConsoleCommandRegister("texdump", CCommand_TexDump, CATEGORY_DEBUG,
                           "Write a texture's upload pixels and raw file to Logs\\texdump: texdump <substring> [mip].");
    ConsoleCommandRegister("texaudit", CCommand_TexAudit, CATEGORY_DEBUG,
                           "Check each BLP header against its file as it loads: texaudit [0|1].");
    ConsoleCommandRegister("assetget", CCommand_AssetGet, CATEGORY_DEBUG,
                           "Copy one client file out of the archives: assetget <path> [outfile].");
}
