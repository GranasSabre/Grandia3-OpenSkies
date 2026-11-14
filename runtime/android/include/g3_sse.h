// Android build: the SSE intrinsics the runtime and the translated VU code use, written with clang
// vector extensions (NEON on ARM64). Each one gives exactly the x86 result, including NaN, signed
// zero and out-of-range cases; tools/sse_shim_test.cpp checks that against real SSE on the PC.
// With G3_SSE_TEST defined the names get a g3 prefix so the test can use both side by side.
#pragma once
#include <stdint.h>
#include <string.h>
#if defined(__aarch64__)
#include <arm_neon.h>  // movemask / conversions in a few NEON instructions (hot in the VU code)
#endif

typedef float g3_v4sf __attribute__((vector_size(16), aligned(16)));
typedef int32_t g3_v4si __attribute__((vector_size(16), aligned(16)));
typedef uint32_t g3_v4su __attribute__((vector_size(16), aligned(16)));
typedef int16_t g3_v8hi __attribute__((vector_size(16), aligned(16)));
typedef uint16_t g3_v8hu __attribute__((vector_size(16), aligned(16)));
typedef int8_t g3_v16qi __attribute__((vector_size(16), aligned(16)));
typedef uint8_t g3_v16qu __attribute__((vector_size(16), aligned(16)));
typedef long long g3_v2di __attribute__((vector_size(16), aligned(16)));

#ifdef G3_SSE_TEST
#define G3N(n) g3##n
typedef g3_v4sf g3_m128;
typedef g3_v2di g3_m128i;
#define G3M128 g3_m128
#define G3M128I g3_m128i
#else
#define G3N(n) n
typedef g3_v4sf __m128;
typedef g3_v2di __m128i;
#define G3M128 __m128
#define G3M128I __m128i
#endif

#define G3I static inline __attribute__((always_inline, unused))

// select per lane: m all ones -> b, all zeros -> a
G3I g3_v4si g3_sel(g3_v4si a, g3_v4si b, g3_v4si m) { return (b & m) | (a & ~m); }

// ---- float ----
G3I G3M128 G3N(_mm_add_ps)(G3M128 a, G3M128 b) { return a + b; }
G3I G3M128 G3N(_mm_sub_ps)(G3M128 a, G3M128 b) { return a - b; }
G3I G3M128 G3N(_mm_mul_ps)(G3M128 a, G3M128 b) { return a * b; }
G3I G3M128 G3N(_mm_set1_ps)(float f) { G3M128 r = {f, f, f, f}; return r; }
G3I G3M128 G3N(_mm_loadu_ps)(const float* p) { G3M128 r; memcpy(&r, p, 16); return r; }
G3I G3M128 G3N(_mm_load_ps)(const float* p) { return *(const G3M128*)p; }
G3I void G3N(_mm_storeu_ps)(float* p, G3M128 a) { memcpy(p, &a, 16); }
// x86 maxps/minps: a when the comparison holds, else b (so NaN in either operand and equal zeros give b)
G3I G3M128 G3N(_mm_max_ps)(G3M128 a, G3M128 b) { return (G3M128)g3_sel((g3_v4si)b, (g3_v4si)a, (g3_v4si)(a > b)); }
G3I G3M128 G3N(_mm_min_ps)(G3M128 a, G3M128 b) { return (G3M128)g3_sel((g3_v4si)b, (g3_v4si)a, (g3_v4si)(a < b)); }
G3I G3M128 G3N(_mm_and_ps)(G3M128 a, G3M128 b) { return (G3M128)((g3_v4si)a & (g3_v4si)b); }
G3I G3M128 G3N(_mm_blendv_ps)(G3M128 a, G3M128 b, G3M128 m) {
    return (G3M128)g3_sel((g3_v4si)a, (g3_v4si)b, (g3_v4si)m >> 31);
}
G3I int G3N(_mm_movemask_ps)(G3M128 a) {
#if defined(__aarch64__)
    const int32x4_t sh = {0, 1, 2, 3};
    return (int)vaddvq_u32(vshlq_u32(vshrq_n_u32((uint32x4_t)a, 31), sh));
#else
    g3_v4su s = (g3_v4su)a >> 31;
    return (int)(s[0] | (s[1] << 1) | (s[2] << 2) | (s[3] << 3));
#endif
}
G3I G3M128 G3N(_mm_cvtepi32_ps)(G3M128I a) { return __builtin_convertvector((g3_v4si)a, g3_v4sf); }
// cvttps2dq: truncation; NaN and values outside the int32 range give 0x80000000
G3I G3M128I G3N(_mm_cvttps_epi32)(G3M128 a) {
#if defined(__aarch64__)
    // NEON saturates and turns NaN into 0: lanes outside [-2^31, 2^31) (and NaN) get 0x80000000
    float32x4_t f = (float32x4_t)a;
    uint32x4_t in = vandq_u32(vcgeq_f32(f, vdupq_n_f32(-2147483648.0f)), vcltq_f32(f, vdupq_n_f32(2147483648.0f)));
    int32x4_t c = vcvtq_s32_f32(f);
    return (G3M128I)vbslq_s32(in, c, vdupq_n_s32((int32_t)0x80000000u));
#endif
    g3_v4si r;
    for (int i = 0; i < 4; i++) {
        float f = a[i];
        r[i] = (f >= -2147483648.0f && f < 2147483648.0f) ? (int32_t)f : (int32_t)0x80000000u;
    }
    return (G3M128I)r;
}
G3I G3M128 G3N(_mm_castsi128_ps)(G3M128I a) { return (G3M128)a; }
G3I G3M128I G3N(_mm_castps_si128)(G3M128 a) { return (G3M128I)a; }

#define G3_SHUFFLE_PS(a, b, imm) \
    ((G3M128)__builtin_shufflevector((g3_v4sf)(a), (g3_v4sf)(b), (imm) & 3, ((imm) >> 2) & 3, (((imm) >> 4) & 3) + 4, (((imm) >> 6) & 3) + 4))
#define G3_BLEND_PS(a, b, imm) \
    ((G3M128)__builtin_shufflevector((g3_v4sf)(a), (g3_v4sf)(b), ((imm) & 1) ? 4 : 0, ((imm) & 2) ? 5 : 1, ((imm) & 4) ? 6 : 2, ((imm) & 8) ? 7 : 3))

// ---- integer ----
G3I G3M128I G3N(_mm_setzero_si128)(void) { G3M128I r = {0, 0}; return r; }
G3I G3M128I G3N(_mm_set1_epi32)(int v) { g3_v4si r = {v, v, v, v}; return (G3M128I)r; }
G3I G3M128I G3N(_mm_set1_epi16)(short v) { g3_v8hi r = {v, v, v, v, v, v, v, v}; return (G3M128I)r; }
G3I G3M128I G3N(_mm_set_epi32)(int e3, int e2, int e1, int e0) { g3_v4si r = {e0, e1, e2, e3}; return (G3M128I)r; }
G3I G3M128I G3N(_mm_load_si128)(const G3M128I* p) { return *p; }
G3I void G3N(_mm_store_si128)(G3M128I* p, G3M128I a) { *p = a; }
G3I int G3N(_mm_cvtsi128_si32)(G3M128I a) { return ((g3_v4si)a)[0]; }
G3I G3M128I G3N(_mm_and_si128)(G3M128I a, G3M128I b) { return a & b; }
G3I G3M128I G3N(_mm_or_si128)(G3M128I a, G3M128I b) { return a | b; }
G3I G3M128I G3N(_mm_xor_si128)(G3M128I a, G3M128I b) { return a ^ b; }
G3I G3M128I G3N(_mm_add_epi16)(G3M128I a, G3M128I b) { return (G3M128I)((g3_v8hu)a + (g3_v8hu)b); }
G3I G3M128I G3N(_mm_mullo_epi16)(G3M128I a, G3M128I b) { return (G3M128I)((g3_v8hu)a * (g3_v8hu)b); }
G3I G3M128I G3N(_mm_cmpeq_epi32)(G3M128I a, G3M128I b) { return (G3M128I)((g3_v4si)a == (g3_v4si)b); }
G3I G3M128I G3N(_mm_cmpgt_epi32)(G3M128I a, G3M128I b) { return (G3M128I)((g3_v4si)a > (g3_v4si)b); }
G3I G3M128I G3N(_mm_srli_epi16)(G3M128I a, int n) {
    if (n > 15) return G3N(_mm_setzero_si128)();
    return (G3M128I)((g3_v8hu)a >> (uint16_t)n);
}
G3I G3M128I G3N(_mm_srai_epi32)(G3M128I a, int n) { return (G3M128I)((g3_v4si)a >> (n > 31 ? 31 : n)); }
G3I G3M128I G3N(_mm_srli_si128)(G3M128I a, int n) {
    uint8_t b[32] = {0};
    memcpy(b, &a, 16);
    G3M128I r;
    memcpy(&r, b + (n > 16 ? 16 : n), 16);
    return r;
}
G3I G3M128I G3N(_mm_unpacklo_epi8)(G3M128I a, G3M128I b) {
    return (G3M128I)__builtin_shufflevector((g3_v16qi)a, (g3_v16qi)b, 0, 16, 1, 17, 2, 18, 3, 19, 4, 20, 5, 21, 6, 22, 7, 23);
}
G3I G3M128I G3N(_mm_packus_epi16)(G3M128I a, G3M128I b) {
    g3_v8hi x = (g3_v8hi)a, y = (g3_v8hi)b;
    g3_v16qu r;
    for (int i = 0; i < 8; i++) {
        int u = x[i], v = y[i];
        r[i] = (uint8_t)(u < 0 ? 0 : u > 255 ? 255 : u);
        r[i + 8] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
    }
    return (G3M128I)r;
}
G3I G3M128I G3N(_mm_blendv_epi8)(G3M128I a, G3M128I b, G3M128I m) {
    g3_v16qi s = (g3_v16qi)m < 0;  // per byte: all ones where the mask's top bit is set
    return (G3M128I)(((g3_v16qi)b & s) | ((g3_v16qi)a & ~s));
}
G3I int G3N(_mm_movemask_epi8)(G3M128I a) {
#if defined(__aarch64__)
    const int8x16_t sh = {0, 1, 2, 3, 4, 5, 6, 7, 0, 1, 2, 3, 4, 5, 6, 7};
    uint8x16_t b = vshlq_u8(vshrq_n_u8((uint8x16_t)a, 7), sh);
    return (int)(vaddv_u8(vget_low_u8(b)) | ((unsigned)vaddv_u8(vget_high_u8(b)) << 8));
#endif
    g3_v16qu s = (g3_v16qu)a >> 7;
    int r = 0;
    for (int i = 0; i < 16; i++) r |= s[i] << i;
    return r;
}

#ifndef G3_SSE_TEST
#define _mm_shuffle_ps G3_SHUFFLE_PS
#define _mm_blend_ps G3_BLEND_PS
#define _MM_SHUFFLE(z, y, x, w) (((z) << 6) | ((y) << 4) | ((x) << 2) | (w))

G3I void _mm_pause(void) {
#if defined(__aarch64__)
    __asm__ __volatile__("yield");
#endif
}
// time stamp counter: the ARM generic timer (used for relative busy/idle statistics only)
G3I unsigned long long __rdtsc(void) {
#if defined(__aarch64__)
    uint64_t v;
    __asm__ __volatile__("mrs %0, cntvct_el0" : "=r"(v));
    return v;
#else
    return __builtin_readcyclecounter();
#endif
}
#endif
