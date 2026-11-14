// ARM64 versions of the hottest VU helpers in runtime/src/vu/vu_internal.h (vsat after almost every
// VU operation, mac_of for the MAC flags), bit for bit the same results as the SSE-style code there.
// Checked on the phone by tools/neon_shim_test.cpp.
#pragma once
#include <arm_neon.h>
#include <stdint.h>

// PS2 floats have no Inf/NaN: exponent 255 becomes +-max. As integers: |x| bits above 0x7F7FFFFF
// are exactly the exponent-255 patterns, so min(|x|, 0x7F7FFFFF) with the sign put back.
static inline __attribute__((always_inline)) uint32x4_t g3_vsat_u32(uint32x4_t xi) {
    const uint32x4_t sign = vdupq_n_u32(0x80000000u);
    uint32x4_t mag = vminq_u32(vbicq_u32(xi, sign), vdupq_n_u32(0x7F7FFFFFu));
    return vbslq_u32(sign, xi, mag);
}

// MAC flags: zero bits 3..0 and sign bits 7..4 (x is the highest bit of each group) of the lanes in
// dest (x = 8, y = 4, z = 2, w = 1)
static inline __attribute__((always_inline)) uint32_t g3_mac_of_u32(uint32x4_t ri, uint32_t dest) {
    const uint32x4_t zw = {8, 4, 2, 1}, sw = {128, 64, 32, 16};
    uint32x4_t z = vceqzq_u32(vbicq_u32(ri, vdupq_n_u32(0x80000000u)));
    uint32x4_t s = vreinterpretq_u32_s32(vshrq_n_s32(vreinterpretq_s32_u32(ri), 31));
    return vaddvq_u32(vorrq_u32(vandq_u32(z, zw), vandq_u32(s, sw))) & (dest * 0x11u);
}
