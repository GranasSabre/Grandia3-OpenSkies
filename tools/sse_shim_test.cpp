// PC test of runtime/android/include/g3_sse.h: every replacement intrinsic against real SSE, bit for
// bit, on random values plus NaN, infinities, signed zeros, denormals and int32 limits.
// Build (x86, LLVM-MinGW): clang++ -O1 -msse4.1 -std=c++20 -ffp-contract=off
//   tools/sse_shim_test.cpp -o build/sse_shim_test.exe
#include <immintrin.h>
#define G3_SSE_TEST
#include "../runtime/android/include/g3_sse.h"
#include <cstdio>
#include <cstring>
#include <random>

static std::mt19937 rng(12345);
static int g_fail = 0, g_checks = 0;

static float special(int k) {
    static const uint32_t bits[] = {0x7FC00000u, 0xFFC00000u, 0x7F800000u, 0xFF800000u, 0x00000000u, 0x80000000u,
                                    0x00000001u, 0x807FFFFFu, 0x4F000000u, 0xCF000000u, 0x4EFFFFFFu, 0x7F7FFFFFu,
                                    0x3F800000u, 0xBF800000u, 0x7FA00000u /* signaling NaN */};
    float f;
    memcpy(&f, &bits[k % 15], 4);
    return f;
}
static void fill(void* p) {
    uint32_t* u = (uint32_t*)p;
    for (int i = 0; i < 4; i++) {
        int m = rng() % 4;
        if (m == 0) { float f = special(rng()); memcpy(&u[i], &f, 4); }
        else if (m == 1) { float f = (float)((int)(rng() % 2000000) - 1000000) / (float)(1 + rng() % 1000); memcpy(&u[i], &f, 4); }
        else u[i] = rng();
    }
}
template <class A, class B>
static void same(const char* what, const A& a, const B& b) {
    static_assert(sizeof(A) == sizeof(B));
    g_checks++;
    if (memcmp(&a, &b, sizeof(A)) != 0) {
        if (g_fail++ < 20) {
            const uint32_t *x = (const uint32_t*)&a, *y = (const uint32_t*)&b;
            printf("MISMATCH %s: sse %08x %08x %08x %08x  shim %08x %08x %08x %08x\n", what, x[0], x[1], x[2], x[3], y[0],
                   y[1], y[2], y[3]);
        }
    }
}

int main() {
    for (int it = 0; it < 200000; it++) {
        alignas(16) float fa[4], fb[4], fm[4];
        fill(fa); fill(fb); fill(fm);
        __m128 a = _mm_load_ps(fa), b = _mm_load_ps(fb), m = _mm_load_ps(fm);
        g3_m128 sa = g3_mm_load_ps(fa), sb = g3_mm_load_ps(fb), sm = g3_mm_load_ps(fm);
        __m128i ia = _mm_castps_si128(a), ib = _mm_castps_si128(b), im = _mm_castps_si128(m);
        g3_m128i sia = g3_mm_castps_si128(sa), sib = g3_mm_castps_si128(sb), sim = g3_mm_castps_si128(sm);
        // arithmetic results with NaN inputs: x86 returns the first NaN operand (quietened);
        // compare all of them bit for bit, NaN payloads included
        same("add", _mm_add_ps(a, b), g3_mm_add_ps(sa, sb));
        same("sub", _mm_sub_ps(a, b), g3_mm_sub_ps(sa, sb));
        same("mul", _mm_mul_ps(a, b), g3_mm_mul_ps(sa, sb));
        same("max", _mm_max_ps(a, b), g3_mm_max_ps(sa, sb));
        same("min", _mm_min_ps(a, b), g3_mm_min_ps(sa, sb));
        same("and_ps", _mm_and_ps(a, b), g3_mm_and_ps(sa, sb));
        same("blendv_ps", _mm_blendv_ps(a, b, m), g3_mm_blendv_ps(sa, sb, sm));
        same("movemask_ps", _mm_movemask_ps(a), g3_mm_movemask_ps(sa));
        same("cvtepi32_ps", _mm_cvtepi32_ps(ia), g3_mm_cvtepi32_ps(sia));
        same("cvttps_epi32", _mm_cvttps_epi32(a), g3_mm_cvttps_epi32(sa));
        same("set1_ps", _mm_set1_ps(fa[1]), g3_mm_set1_ps(fa[1]));
        alignas(16) float o1[4], o2[4];
        _mm_storeu_ps(o1, a); g3_mm_storeu_ps(o2, sa); same("storeu", o1, o2);
        same("loadu", _mm_loadu_ps(fb), g3_mm_loadu_ps(fb));
        same("shuffle_1b", _mm_shuffle_ps(a, b, 0x1B), G3_SHUFFLE_PS(sa, sb, 0x1B));
        same("shuffle_e4", _mm_shuffle_ps(a, b, 0xE4), G3_SHUFFLE_PS(sa, sb, 0xE4));
        same("shuffle_93", _mm_shuffle_ps(a, b, 0x93), G3_SHUFFLE_PS(sa, sb, 0x93));
        same("blend_5", _mm_blend_ps(a, b, 5), G3_BLEND_PS(sa, sb, 5));
        same("blend_e", _mm_blend_ps(a, b, 14), G3_BLEND_PS(sa, sb, 14));
        // integer
        same("and_si", _mm_and_si128(ia, ib), g3_mm_and_si128(sia, sib));
        same("or_si", _mm_or_si128(ia, ib), g3_mm_or_si128(sia, sib));
        same("xor_si", _mm_xor_si128(ia, ib), g3_mm_xor_si128(sia, sib));
        same("add_epi16", _mm_add_epi16(ia, ib), g3_mm_add_epi16(sia, sib));
        same("mullo_epi16", _mm_mullo_epi16(ia, ib), g3_mm_mullo_epi16(sia, sib));
        same("cmpeq_epi32", _mm_cmpeq_epi32(ia, ib), g3_mm_cmpeq_epi32(sia, sib));
        same("cmpgt_epi32", _mm_cmpgt_epi32(ia, ib), g3_mm_cmpgt_epi32(sia, sib));
        same("cmpeq_self", _mm_cmpeq_epi32(ia, ia), g3_mm_cmpeq_epi32(sia, sia));
        int n = (int)(rng() % 20);
        same("srli_epi16", _mm_srli_epi16(ia, n), g3_mm_srli_epi16(sia, n));
        same("srai_epi32", _mm_srai_epi32(ia, n * 2), g3_mm_srai_epi32(sia, n * 2));
        same("srli_si128_4", _mm_srli_si128(ia, 4), g3_mm_srli_si128(sia, 4));
        same("srli_si128_8", _mm_srli_si128(ia, 8), g3_mm_srli_si128(sia, 8));
        same("unpacklo_epi8", _mm_unpacklo_epi8(ia, ib), g3_mm_unpacklo_epi8(sia, sib));
        same("packus_epi16", _mm_packus_epi16(ia, ib), g3_mm_packus_epi16(sia, sib));
        same("blendv_epi8", _mm_blendv_epi8(ia, ib, im), g3_mm_blendv_epi8(sia, sib, sim));
        same("movemask_epi8", _mm_movemask_epi8(ia), g3_mm_movemask_epi8(sia));
        same("cvtsi128_si32", _mm_cvtsi128_si32(ia), g3_mm_cvtsi128_si32(sia));
        int v = (int)rng();
        same("set1_epi32", _mm_set1_epi32(v), g3_mm_set1_epi32(v));
        same("set1_epi16", _mm_set1_epi16((short)v), g3_mm_set1_epi16((short)v));
        same("set_epi32", _mm_set_epi32(v, v + 1, v * 3, ~v), g3_mm_set_epi32(v, v + 1, v * 3, ~v));
        same("setzero", _mm_setzero_si128(), g3_mm_setzero_si128());
    }
    printf("%d checks, %d mismatches\n", g_checks, g_fail);
    return g_fail != 0;
}
