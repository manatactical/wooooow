// Throwaway analysis tool: dumps the sequence table (id, duration, flags) of M2 files given on the command
// line, so mount animation support can be compared across models with data instead of guesswork.
//
// Reads plain files from disk: the retail mounts in patch-Z.mpq are loose, so no archive mounting is needed.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace
{
    struct Sequence
    {
        uint16_t id;
        uint16_t variation;
        uint32_t duration;
        float moveSpeed;
        uint32_t flags;
    };

    bool ReadWhole(const char* path, std::vector<uint8_t>& d)
    {
        FILE* f = std::fopen(path, "rb");
        if (!f) return false;

        std::fseek(f, 0, SEEK_END);
        const long n = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);

        if (n <= 0) { std::fclose(f); return false; }

        d.resize(static_cast<size_t>(n));
        const size_t got = std::fread(d.data(), 1, d.size(), f);
        std::fclose(f);
        return got == d.size();
    }

    bool ReadSequences(const std::vector<uint8_t>& d, std::vector<Sequence>& out, uint32_t& version)
    {
        if (d.size() < 0x40) return false;

        const uint32_t magic = *reinterpret_cast<const uint32_t*>(d.data());
        size_t base = 0;

        if (magic == 0x3031444D || magic == 0x3032444D) { // MD10 / MD20
            version = *reinterpret_cast<const uint32_t*>(d.data() + 4);
        } else if (magic == 0x3132444D) { // MD21
            // MD21 is a header: fourcc, u32 length, then the embedded MD20 body starts immediately at 8.
            // (This is not a chunk list -- walking chunks here read the body bytes as chunk headers and
            // produced a garbage version.)
            if (d.size() < 16) return false;

            const uint32_t chunkSize = *reinterpret_cast<const uint32_t*>(d.data() + 4);

            if (chunkSize < 0x40 || 8 + static_cast<size_t>(chunkSize) > d.size()) return false;

            base = 8;

            if (*reinterpret_cast<const uint32_t*>(d.data() + base) != 0x3032444D) return false; // 'MD20'

            version = *reinterpret_cast<const uint32_t*>(d.data() + base + 4);
        } else {
            return false;
        }

        if (base + 0x30 > d.size()) return false;

        const uint32_t seqCount = *reinterpret_cast<const uint32_t*>(d.data() + base + 0x1C);
        const uint32_t seqOffset = *reinterpret_cast<const uint32_t*>(d.data() + base + 0x20);

        if (!seqCount || seqCount > 8192) return false;

        // Array offsets are absolute file offsets, not relative to the MD20 body: adding the body base
        // again walked into the middle of unrelated data and produced plausible-looking garbage ids.
        const size_t stride = 0x40;
        if (static_cast<size_t>(seqOffset) + static_cast<size_t>(seqCount) * stride > d.size()) return false;

        for (uint32_t i = 0; i < seqCount; i++)
        {
            const uint8_t* p = d.data() + seqOffset + static_cast<size_t>(i) * stride;
            Sequence s{};
            s.id = *reinterpret_cast<const uint16_t*>(p + 0x00);
            s.variation = *reinterpret_cast<const uint16_t*>(p + 0x02);
            s.duration = *reinterpret_cast<const uint32_t*>(p + 0x04);
            s.moveSpeed = *reinterpret_cast<const float*>(p + 0x08);
            s.flags = *reinterpret_cast<const uint32_t*>(p + 0x0C);
            out.push_back(s);
        }

        return true;
    }
}

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::printf("usage: m2seq <file.m2> [...]\n");
        return 2;
    }

    for (int a = 1; a < argc; a++)
    {
        std::vector<uint8_t> bytes;

        if (!ReadWhole(argv[a], bytes))
        {
            std::printf("%s: NOT FOUND\n", argv[a]);
            continue;
        }

        std::vector<Sequence> seqs;
        uint32_t version = 0;

        if (!ReadSequences(bytes, seqs, version))
        {
            std::printf("%s: unreadable (%zu bytes, magic %08x)\n", argv[a], bytes.size(),
                        *reinterpret_cast<const uint32_t*>(bytes.data()));
            continue;
        }

        std::printf("%s version=%u sequences=%zu\n", argv[a], version, seqs.size());

        for (const Sequence& s : seqs)
            std::printf("   id=%-5u dur=%-6u speed=%-7.2f flags=0x%x\n", s.id, s.duration, s.moveSpeed, s.flags);
    }

    return 0;
}
