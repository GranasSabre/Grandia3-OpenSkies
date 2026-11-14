// Phone test of the NEON paths in runtime/android/include/g3_sse.h: each against the portable
// definition (which tools/sse_shim_test.cpp checks against real SSE on the PC), bit for bit.
// Build (NDK clang): clang++ --target=aarch64-linux-android28 -O1 -std=c++20 -ffp-contract=off -static-libstdc++
//   tools/neon_shim_test.cpp -o build/android/neon_shim_test; then adb push to /data/local/tmp and run it
#include "../runtime/android/include/g3_sse.h"
#include "../runtime/android/include/g3_vu_neon.h"
#include <cstdio>
#include <cstring>
#include <random>

static std::mt19937 rng(777);
static float special(int k) {
    static const uint32_t bits[] = {0x7FC00000u, 0xFFC00000u, 0x7F800000u, 0xFF800000u, 0x00000000u, 0x80000000u,
                                    0x00000001u, 0x807FFFFFu, 0x4F000000u, 0xCF000000u, 0x4EFFFFFFu, 0x7F7FFFFFu,
                                    0x3F800000u, 0xBF800000u, 0x7FA00000u, 0xCF000001u, 0x4F7FFFFFu};
    float f;
    memcpy(&f, &bits[k % 17], 4);
    return f;
}
static void fill(void* p) {
    uint32_t* u = (uint32_t*)p;
    for (int i = 0; i < 4; i++) {
        int m = rng() % 4;
        if (m == 0) { float f = special(rng()); memcpy(&u[i], &f, 4); }
        else if (m == 1) { float f = (float)((int)(rng() % 2000000) - 1000000) / (float)(1 + rng() % 1000) * (float)(1 << (rng() % 31)); memcpy(&u[i], &f, 4); }
        else u[i] = rng();
    }
}
// portable references (copies of the generic code paths)
static int ref_movemask_ps(__m128 a) {
    g3_v4su s = (g3_v4su)a >> 31;
    return (int)(s[0] | (s[1] << 1) | (s[2] << 2) | (s[3] << 3));
}
static int ref_movemask_epi8(__m128i a) {
    g3_v16qu s = (g3_v16qu)a >> 7;
    int r = 0;
    for (int i = 0; i < 16; i++) r |= s[i] << i;
    return r;
}
static __m128i ref_cvtt(__m128 a) {
    g3_v4si r;
    for (int i = 0; i < 4; i++) {
        float f = a[i];
        r[i] = (f >= -2147483648.0f && f < 2147483648.0f) ? (int32_t)f : (int32_t)0x80000000u;
    }
    return (__m128i)r;
}

// vu_internal.h vsat / mac_of, SSE-style (as on the PC)
static __m128 ref_vsat(__m128 x) {
    const __m128i e = _mm_set1_epi32(0x7F800000);
    __m128i xi = _mm_castps_si128(x);
    __m128i sp = _mm_cmpeq_epi32(_mm_and_si128(xi, e), e);
    if (_mm_movemask_epi8(sp) == 0) return x;
    __m128i rep = _mm_or_si128(_mm_and_si128(xi, _mm_set1_epi32((int)0x80000000)), _mm_set1_epi32(0x7F7FFFFF));
    return _mm_castsi128_ps(_mm_blendv_epi8(xi, rep, sp));
}
static const uint8_t g_rev4[16] = {0, 8, 4, 12, 2, 10, 6, 14, 1, 9, 5, 13, 3, 11, 7, 15};
static uint32_t ref_mac_of(__m128 r, uint32_t dest) {
    uint32_t lanes = g_rev4[dest];
    __m128i ri = _mm_castps_si128(r);
    __m128i z = _mm_cmpeq_epi32(_mm_and_si128(ri, _mm_set1_epi32(0x7FFFFFFF)), _mm_setzero_si128());
    uint32_t zm = (uint32_t)_mm_movemask_ps(_mm_castsi128_ps(z)) & lanes;
    uint32_t sm = (uint32_t)_mm_movemask_ps(r) & lanes;
    return g_rev4[zm] | ((uint32_t)g_rev4[sm] << 4);
}

int main() {
    int fail = 0;
    long checks = 0;
    for (int it = 0; it < 3000000; it++) {
        __m128 a;
        __m128i b;
        fill(&a);
        fill(&b);
        checks += 3;
        if (_mm_movemask_ps(a) != ref_movemask_ps(a)) { if (fail++ < 10) printf("movemask_ps mismatch\n"); }
        if (_mm_movemask_epi8(b) != ref_movemask_epi8(b)) { if (fail++ < 10) printf("movemask_epi8 mismatch\n"); }
        {
            __m128 p = (__m128)g3_vsat_u32((uint32x4_t)a), q = ref_vsat(a);
            if (memcmp(&p, &q, 16)) { if (fail++ < 10) puts("vsat mismatch"); }
            uint32_t d = rng() & 15;
            if (g3_mac_of_u32((uint32x4_t)a, d) != ref_mac_of(a, d)) { if (fail++ < 10) puts("mac_of mismatch"); }
            checks += 2;
        }
        __m128i x = _mm_cvttps_epi32(a), y = ref_cvtt(a);
        if (memcmp(&x, &y, 16)) {
            if (fail++ < 10) {
                const uint32_t* u = (const uint32_t*)&a;
                printf("cvttps mismatch %08x %08x %08x %08x\n", u[0], u[1], u[2], u[3]);
            }
        }
    }
    printf("%ld checks, %d mismatches\n", checks, fail);
    return fail != 0;
}
