#ifndef ENGINE_SOURCE_TEMPEST_CMATH_H
#define ENGINE_SOURCE_TEMPEST_CMATH_H

#include <cmath>
#include <cstddef>
#include <cstdint>

#if defined(WOWDECOMP_ARCH_64)
#include <xmmintrin.h>

// Defined in Engine/Source/Os/RenderProfiler.cpp. Declared here rather than including that header, so
// this widely-included header stays dependency-free.
void RenderProfilerCountFintOutOfRange();
#endif

class CMath {
    public:
    static float cos_(float x);
    static double exp_(float x);
    static double exp2_(double x);
    static bool fequal(float a, float b);
    static int32_t fint_(float f);
    static int32_t fint_n(float f);
    static int32_t ftrunc(float f);
    static int32_t ftol_round_0_255_(float f);
    static double log2_(double x);
    static bool fnotequal(float a, float b);
    static float normalizeAnglePi(float angle);
    static float normalizeAngleNegPiToPi_(float angle);
    static float normalizeangle0to2pi_(float angle);
    static float sin_(float x);
    static bool solvequad_(float a, float b, float c, float& r0, float& r1);
    static void split_(float x, float* frac, int32_t* whole);
    static void sincos_(float x, float* s, float* c);
    static uint32_t fuint_pi(float f);
    static uint32_t sqrt_(uint32_t n);
};

class CSoftFloat {
    public:
    static CSoftFloat Ceil(const CSoftFloat& value);
    static uint32_t CountLeadingZeros(uint32_t value);
    static CSoftFloat Floor(const CSoftFloat& value);
    static CSoftFloat FromInt(int32_t value);
    static CSoftFloat FromRatio(int32_t numerator, int32_t denominator);
    static CSoftFloat Reciprocal(const CSoftFloat& value);
    static CSoftFloat Round(const CSoftFloat& value);

    /* 0000 */ uint32_t m_bits;
};

CSoftFloat operator*(const CSoftFloat& l, const CSoftFloat& r);
CSoftFloat operator+(const CSoftFloat& l, const CSoftFloat& r);

float fclamp(float value, float min, float max);
bool fequal(float a, float b);
int32_t fequalz(float a, float b, float z);
int32_t Clamp(int32_t value, int32_t min, int32_t max);
int32_t fnotequalz(float a, float b, float z);
uint32_t CeilToInt64(float value);
int32_t FloatToInt64(float value);
int32_t RoundToInt64(float value);
void ClampInPlace(float& value, float min, float max);
bool fnotequal(float a, float b);
int32_t IsNaN(double value);

// VERIFY: 03d90c2d auto
// OFFSET: 0x005FFF80
inline float CMath::cos_(float x) {
    float frac;
    int32_t whole;
    CMath::split_(x * 0.31830987f, &frac, &whole);

    float result = 1.0f - frac * ((6.0f - 4.0f * frac) * frac);

    return (whole & 1) ? -result : result;
}

// INLINED
//
// DELIBERATE DEVIATION from the original source form, for x64 speed.
//
// `lrintf` only becomes a single instruction when the compiler is free to assume the default rounding
// mode, which needs /fp:fast. This project sets /fp:fast for Win32 only (CMakeLists.txt:30, to match the
// VS2005 original), so on x64 every fint_ call site is an actual CRT call. That was measured: 302 call
// sites across 55 files, and replacing the two inside CWorldOcclusion::UpdateBufferChunk alone removed
// 0.41ms/frame (~5.5%) because those loops ran thousands of times per frame.
//
// The instruction below is what the Win32 build already emits, and what the original 32-bit binary used.
//
// Out-of-range handling is preserved deliberately, not simplified. `lrintf` returns 0 for values outside
// int32 range (and for inf/NaN); `cvtss2si` returns the x86 integer indefinite 0x80000000. A probe over
// 604002 values found no in-range difference (including all .5 round-to-even boundaries), but the
// out-of-range case differs, and a wrong value here becomes a wrong index or world coordinate. So the
// range test keeps lrintf's behaviour, and RenderProfilerCountFintOutOfRange makes the case visible
// instead of silently changing it.
inline int32_t CMath::fint_(float f) {
#if defined(WOWDECOMP_ARCH_64)
    // The bounds are the largest floats below/above the int32 range that round into it.
    if (f > -2147483648.0f && f < 2147483648.0f) {
        return _mm_cvtss_si32(_mm_set_ss(f));
    }

    RenderProfilerCountFintOutOfRange();

    return static_cast<int32_t>(lrintf(f));
#else
    return static_cast<int32_t>(lrintf(f));
#endif
}

// INLINED
inline int32_t CMath::ftol_round_0_255_(float f) {
    return static_cast<int32_t>(f * 255.0f + 0.5f);
}

float OrganicSmooth(float from, float to, float t);

#endif
