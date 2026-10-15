// Throwaway probe: does _mm_cvtss_si32 match lrintf for the values fint_ actually sees?
// Temporary file, not part of the build.
#include <cstdio>
#include <cmath>
#include <cstdint>
#include <xmmintrin.h>

static int32_t ViaLrintf(float f) {
    return static_cast<int32_t>(lrintf(f));
}

// The proposed replacement: same instruction, plus a range guard so out-of-range values saturate
// instead of returning the x86 integer-indefinite 0x80000000.
static int32_t ViaSse(float f) {
    if (f != f) {
        return 0;
    }

    if (f > 2147483520.0f) {
        return 0x7FFFFFFF;
    }

    if (f < -2147483648.0f) {
        return static_cast<int32_t>(0x80000000);
    }

    return _mm_cvtss_si32(_mm_set_ss(f));
}

int main() {
    int mismatches = 0;
    long tested = 0;

    // The real workload: coordinates times 64 minus 0.5. Sweep a wide plausible range.
    for (int i = -300000; i <= 300000; i++) {
        float f = static_cast<float>(i) * 0.001f * 64.0f - 0.5f;
        int32_t a = ViaLrintf(f);
        int32_t b = ViaSse(f);

        if (a != b) {
            if (mismatches < 10) {
                printf("MISMATCH f=%.6f lrintf=%d sse=%d\n", f, a, b);
            }

            mismatches++;
        }

        tested++;
    }

    // Rounding boundaries: exact .5 cases, which is where round-to-even matters.
    for (int i = -2000; i <= 2000; i++) {
        float f = i + 0.5f;
        int32_t a = ViaLrintf(f);
        int32_t b = ViaSse(f);

        if (a != b) {
            if (mismatches < 10) {
                printf("MISMATCH(.5) f=%.1f lrintf=%d sse=%d\n", f, a, b);
            }

            mismatches++;
        }

        tested++;
    }

    // Negative zero, infinities, NaN.
    float specials[] = { -0.0f, 0.0f, 1e30f, -1e30f, __builtin_huge_valf(), -__builtin_huge_valf(), __builtin_nanf("") };
    for (float f : specials) {
        printf("special f=%g lrintf=%d sse=%d\n", f, ViaLrintf(f), ViaSse(f));
    }

    printf("tested=%ld mismatches=%d\n", tested, mismatches);
    return 0;
}