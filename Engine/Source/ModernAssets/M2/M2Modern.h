#ifndef ENGINE_SOURCE_MODERNASSETS_M2_M2MODERN_H
#define ENGINE_SOURCE_MODERNASSETS_M2_M2MODERN_H

#include <cstdint>

// Modern M2 (MD21) support: reads a model authored for a newer build and repacks it in place into the
// one layout the stock client's parser accepts (MD20, inner version 264, client record widths).
//
// Why repacking rather than a second parser: this client is source, not a hooked binary, and its
// parser (`CM2Shared::FinishLoading` -> `M2Init`) already resolves a v264 body's body-relative offsets
// into pointers, bounds-checks every array, and handles the per-sequence track slots. Handing it a
// well-formed v264 body is therefore the whole job, and every consumer downstream -- animation,
// particles, ribbons, hit testing, shadows -- then needs no changes at all.
//
// Ported from WarcraftXL wxl-modern-m2 (GPL-3.0-or-later, Copyright (C) 2026 WarcraftXL), whose native
// reader normalises the same records inside the original binary.
namespace modern::m2
{
    /// Session counters, so a model that looks wrong can be traced to a reason rather than guessed at.
    struct Stats
    {
        uint32_t modelsConverted = 0;
        uint32_t modelsFailed = 0;
        uint32_t texturesResolved = 0;
        uint32_t texturesUnresolved = 0;
        uint32_t particlesNormalized = 0;
        uint32_t camerasNormalized = 0;
        uint32_t sequencesRemapped = 0;
        uint32_t externalSequences = 0;
        /// Models whose bones carried no shadow-animate flag and were corrected.
        uint32_t shadowGateForced = 0;
    };

    Stats GetStats();

    /// True when the buffer starts with the MD21 chunk tag.
    bool IsModernContainer(const uint8_t* bytes, uint32_t size);

    /**
     * @brief Converts a resident MD21 container into a v264 MD20 body, in place.
     *
     * On success the body starts at `file[0]`, `bodySize` is its length (which may be a little larger
     * than it was, because resolved texture paths are appended past the old end), and the stock parser
     * accepts it unchanged. Layout conversions only ever shrink a record, so every array keeps its
     * offset and no nested body-relative offset has to be revisited.
     *
     * @param file      the resident file bytes. Must have at least `capacity` writable bytes, with
     *                  slack past `bodySize` for appended texture paths.
     * @param capacity  writable bytes available at `file`.
     * @param bodySize  in: the file's byte size. out: the converted body's byte size.
     * @param why       receives a static reason on failure.
     * @return true when a complete, client-readable model was produced.
     */
    bool Convert(uint8_t* file, uint32_t capacity, uint32_t& bodySize, const char** why);
}

#endif
