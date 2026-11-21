#version 450
// GS pixel pipeline (port of DRAW_HLSL ps_main in gs_gpu_shaders.h). The frame and z surfaces are
// color attachments that are also input attachments of the subpass (rasterization order attachment
// access): subpassLoad reads the pixel as earlier primitives left it. Both outputs are always
// written (the old value where nothing changes); discard keeps both.
// Built twice: HAS_Z 0 (frame surface only) and 1 (frame + z surface).
#ifndef HAS_Z
#define HAS_Z 0
#endif
// the frame surface is bound through its RGBA8 view (the same bits as the R32 value: R in the low
// byte), so hardware blending works on it (HW build); packUnorm4x8/unpackUnorm4x8 are exact.
// Passes with a z surface also have a depth attachment that mirrors it: every pipeline writes
// gl_FragDepth = ZD(the z value the pixel ends with) (the HW build tests against it).
#define ZD(z) (float(z) * (1.0 / 4294967296.0))
// Per batch the renderer builds a pipeline with these fixed (gs_vk.cpp draw_spec): the branches on
// them fold away, which shortens the shader a lot (with rasterization order access the latency of
// one fragment holds up the next one on the same pixel).
// The general pipeline (draws while a batch's specialized one compiles in the background) uses
// draw_general.frag, the 1.0.7-beta1 shader with the SC_DYN switch. Two Adreno 840
// driver bugs (2026-11-15) keep them apart: SC_DYN combined with the other constants in one shader
// gave striped cutscenes at x2 in the specialized pipelines; a constant-free build of this file as
// the general pipeline drew battle models as noise.
layout(constant_id = 0) const uint SC_TEX = 1u;     // any primitive of the batch textured
layout(constant_id = 1) const uint SC_TFMT = 0u;
layout(constant_id = 2) const uint SC_TFX = 0u;
layout(constant_id = 3) const uint SC_TCC = 0u;
layout(constant_id = 4) const uint SC_WMS = 0u;
layout(constant_id = 5) const uint SC_WMT = 0u;
layout(constant_id = 6) const uint SC_TS1 = 1u;     // texture scale 1 (else vp.z)
layout(constant_id = 7) const uint SC_MAGLIN = 0u;
layout(constant_id = 8) const uint SC_MINLIN = 0u;
layout(constant_id = 9) const uint SC_NEEDLOD = 0u;
layout(constant_id = 10) const uint SC_MIP = 0u;    // mip levels bound
layout(constant_id = 11) const uint SC_MMIN = 0u;
layout(constant_id = 12) const uint SC_LCM = 0u;
layout(constant_id = 13) const uint SC_FONT = 0u;
layout(constant_id = 14) const uint SC_CPSM16 = 0u;
layout(constant_id = 15) const uint SC_AEM = 0u;
layout(constant_id = 16) const uint SC_FPK = 0u;
layout(constant_id = 17) const uint SC_FBMSK0 = 1u; // no frame buffer mask
layout(constant_id = 18) const uint SC_DTHE = 0u;
layout(constant_id = 19) const uint SC_ZTE = 0u;
layout(constant_id = 20) const uint SC_ZTST = 0u;
layout(constant_id = 21) const uint SC_ZMSK = 0u;
layout(constant_id = 22) const uint SC_ZFMT = 0u;
#define SPEC(sc, dyn) (sc)
layout(std140, set = 0, binding = 0) uniform CB {
    vec4 vp;   // surface width, height (native pixels), scale S
    uvec4 f0;  // fpsm kind (0 C32, 1 C24, 2 C16), fbmsk
    uvec4 f1;  // dthe
    uvec4 f2;  // -, -, -, zte
    uvec4 f3;  // ztst, zmsk, zfmt (0 Z32, 1 Z24, 2 Z16)
    uvec4 f4;
    uvec4 f5;
    uvec4 f6;  // -, tfx, tcc, tfmt
    uvec4 f7;  // tw, th, wms, wmt
    uvec4 f8;  // minu, maxu, minv, maxv
    uvec4 f9;  // cpsm16, csa, ta0, ta1
    uvec4 f10; // aem, mag_linear, min_linear, need_lod
    vec4 f11;  // lodk, qthr, texture scale (1 or S)
    uvec4 f12;
    ivec4 f13; // texture origin in the surface (native texels)
    uvec4 f14; // mip levels bound (0 = none), mmin, lcm, L
    vec4 f15;  // HD texture: on, scale k; sharp text: on, full alpha
    ivec4 dimx[4];
};
#define FPSM SPEC(SC_FPK, f0.x)
#define FBMSK (SC_FBMSK0 != 0u ? 0u : f0.y)
#define DTHE SPEC(SC_DTHE, f1.x)
#define ZTE SPEC(SC_ZTE, f2.w)
#define ZTST SPEC(SC_ZTST, f3.x)
#define ZMSK SPEC(SC_ZMSK, f3.y)
#define ZFMT SPEC(SC_ZFMT, f3.z)
#define TFX SPEC(SC_TFX, f6.y)
#define TCC SPEC(SC_TCC, f6.z)
// the texture format is always a runtime switch (SC_TFMT unused): with texel() folded to one format
// the Adreno 840 driver broke the bilinear reads of surface copies at internal x2 (striped cutscene
// 053) and, with only format 0 left at runtime, battle models (noise); found 2026-11-15 with
// GRANDIA_VK_SPEC_GEN and per-constant tests
#define TFMT f6.w
#define TW f7.x
#define TH f7.y
#define WMS SPEC(SC_WMS, f7.z)
#define WMT SPEC(SC_WMT, f7.w)
#define MINU f8.x
#define MAXU f8.y
#define MINV f8.z
#define MAXV f8.w
#define CPSM16 SPEC(SC_CPSM16, f9.x)
#define CSA f9.y
#define TA0 f9.z
#define TA1 f9.w
#define AEM SPEC(SC_AEM, f10.x)
#define MAGLIN SPEC(SC_MAGLIN, f10.y)
#define MINLIN SPEC(SC_MINLIN, f10.z)
#define NEEDLOD SPEC(SC_NEEDLOD, f10.w)
#define LODK f11.x
#define QTHR f11.y
#define TSCALE (SC_TS1 != 0u ? 1.0 : f11.z)
#define MIPL f14.x
#define MMIN SPEC(SC_MMIN, f14.y)
#define LCM SPEC(SC_LCM, f14.z)
#define LODL f14.w
#define HDTEX 0.0  // (no HD textures on Android)
#define HDK f15.y
#define FONTTEX (SC_FONT != 0u ? 1.0 : 0.0)
#define FONTAMAX f15.w
#define MIPON (SC_MIP != 0u)

layout(set = 0, binding = 1) uniform usampler2D tex;
layout(std430, set = 0, binding = 2) readonly buffer Clut { uint clut[512]; };
layout(set = 0, binding = 3) uniform usampler2D texm1;  // mip levels 1-6
layout(set = 0, binding = 4) uniform usampler2D texm2;
layout(set = 0, binding = 5) uniform usampler2D texm3;
layout(set = 0, binding = 6) uniform usampler2D texm4;
layout(set = 0, binding = 7) uniform usampler2D texm5;
layout(set = 0, binding = 8) uniform usampler2D texm6;
layout(set = 0, binding = 9) uniform sampler2D hdtex;   // HD replacement (sampler chosen per batch)
layout(input_attachment_index = 0, set = 0, binding = 10) uniform subpassInput fb_in;
#if HAS_Z && !defined(HW)
layout(input_attachment_index = 1, set = 0, binding = 11) uniform usubpassInput zb_in;
#endif

layout(location = 0) noperspective in vec4 i_col;
layout(location = 1) flat in vec4 i_fcol;
layout(location = 2) noperspective in vec3 i_stq;
layout(location = 3) noperspective in float i_z;
layout(location = 4) noperspective in float i_fog;
layout(location = 5) flat in uvec2 i_st;
layout(location = 0) out vec4 fb_out;
#if HAS_Z
layout(location = 1) out uint zb_out;
#endif

uvec2 PST;
#define ATE (PST.x & 1u)
#define ATST ((PST.x >> 1) & 7u)
#define AFAIL ((PST.x >> 4) & 3u)
#define DATE ((PST.x >> 6) & 1u)
#define DATM ((PST.x >> 7) & 1u)
#define AREF ((PST.x >> 8) & 0xFFu)
#define ABE ((PST.x >> 16) & 1u)
#define BA ((PST.x >> 17) & 3u)
#define BB ((PST.x >> 19) & 3u)
#define BC ((PST.x >> 21) & 3u)
#define BD ((PST.x >> 23) & 3u)
#define PABE ((PST.x >> 25) & 1u)
#define FBA (((PST.x >> 26) & 1u) << 7)
#define COLCLAMP ((PST.x >> 27) & 1u)
#define FGE ((PST.x >> 28) & 1u)
#define IIP ((PST.x >> 29) & 1u)
#define FST ((PST.x >> 30) & 1u)
#define TME (SC_TEX != 0u ? (PST.x >> 31) : 0u)
#define BFIX (PST.y & 0xFFu)
#define FOGCOL (PST.y >> 8)

uvec4 unpack32(uint c) { return uvec4(c & 0xFFu, (c >> 8) & 0xFFu, (c >> 16) & 0xFFu, c >> 24); }
uvec4 expand16(uint c) {
    uint a = (c & 0x8000u) != 0u ? TA1 : ((AEM != 0u && (c & 0x7FFFu) == 0u) ? 0u : TA0);
    return uvec4((c & 0x1Fu) << 3, ((c >> 5) & 0x1Fu) << 3, ((c >> 10) & 0x1Fu) << 3, a);
}
uvec4 clut_lookup(uint idx, bool is4) {
    if (CPSM16 != 0u) return expand16(clut[(idx + CSA * 16u) & 511u] & 0xFFFFu);
    if (is4) return unpack32(clut[(idx + (CSA & 15u) * 16u) & 255u]);
    return unpack32(clut[idx & 255u]);
}
uvec4 texel(uint raw) {
    switch (TFMT) {
    case 0u: return unpack32(raw);
    case 1u: { uint c = raw & 0xFFFFFFu; return uvec4(unpack32(c).xyz, (AEM != 0u && c == 0u) ? 0u : TA0); }
    case 2u: return expand16(raw & 0xFFFFu);
    case 3u: return clut_lookup(raw & 0xFFu, false);
    case 4u: return clut_lookup(raw & 0xFu, true);
    case 5u: return clut_lookup(raw >> 24, false);
    case 6u: return clut_lookup((raw >> 24) & 0xFu, true);
    case 7u: return clut_lookup(raw >> 28, true);
    default: return uvec4(unpack32(raw & 0xFFFFFFu).xyz, TA0);
    }
}

// one axis of the texture address in texture texels at scale k (TSCALE for level 0, 1 for mips)
int wrap_axis(int c, uint mode, int size, int mn, int mx, int k) {
    int n = int(floor(float(c) / float(k)));
    n += ((n + 1) * k <= c) ? 1 : 0;
    n -= (n * k > c) ? 1 : 0;
    int sub = c - n * k;
    int m;
    switch (mode) {
    case 0u: m = n & (size - 1); break;
    case 1u: m = clamp(n, 0, size - 1); break;
    case 2u: m = clamp(n, mn, mx); break;
    default: m = (n & mn) | mx; break;
    }
    if (mode == 1u || mode == 2u) sub = (m > n) ? 0 : (m < n ? k - 1 : sub);
    return m * k + sub;
}
int wrap_u(int l, int u) {
    if (l == 0) return wrap_axis(u, WMS, int(TW), int(MINU), int(MAXU), int(TSCALE));
    return wrap_axis(u, WMS, max(int(TW) >> l, 1), int(MINU) >> l, int(MAXU) >> l, 1);
}
int wrap_v(int l, int v) {
    if (l == 0) return wrap_axis(v, WMT, int(TH), int(MINV), int(MAXV), int(TSCALE));
    return wrap_axis(v, WMT, max(int(TH) >> l, 1), int(MINV) >> l, int(MAXV) >> l, 1);
}
uint load_lv(int l, int x, int y) {
    if (l == 0) {
        int k = int(TSCALE);
        return texelFetch(tex, ivec2(x + f13.x * k, y + f13.y * k), 0).x;
    }
    ivec2 p = ivec2(x, y);
    switch (l) {
    case 1: return texelFetch(texm1, p, 0).x;
    case 2: return texelFetch(texm2, p, 0).x;
    case 3: return texelFetch(texm3, p, 0).x;
    case 4: return texelFetch(texm4, p, 0).x;
    case 5: return texelFetch(texm5, p, 0).x;
    default: return texelFetch(texm6, p, 0).x;
    }
}
uint fetch_lv(int l, int u, int v) { return load_lv(l, wrap_u(l, u), wrap_v(l, v)); }
uvec4 lerp8(uvec4 a, uvec4 b, uint w) { return (a * (256u - w) + b * w) >> 8; }
uvec4 sample_lv(int l, float u, float v, bool lin) {
    float sc = (l == 0) ? TSCALE : 1.0 / float(1 << l);
    u *= sc;
    v *= sc;
    if (!lin) return texel(fetch_lv(l, int(floor(u)), int(floor(v))));
    float fu = u - 0.5, fv = v - 0.5;
    float u0f = floor(fu), v0f = floor(fv);
    uint wu = uint((fu - u0f) * 256.0), wv = uint((fv - v0f) * 256.0);
    int u0 = int(u0f), v0 = int(v0f);
    int x0 = wrap_u(l, u0), x1 = wrap_u(l, u0 + 1), y0 = wrap_v(l, v0), y1 = wrap_v(l, v0 + 1);
    uvec4 t00 = texel(load_lv(l, x0, y0)), t10 = texel(load_lv(l, x1, y0));
    uvec4 t01 = texel(load_lv(l, x0, y1)), t11 = texel(load_lv(l, x1, y1));
    return lerp8(lerp8(t00, t10, wu), lerp8(t01, t11, wu), wv);
}
uvec4 sample_tex(float u, float v, float lod) {
    bool lin = (lod <= 0.0) ? (MAGLIN != 0u) : (MINLIN != 0u);
    return sample_lv(0, u, v, lin);
}
uvec4 sample_mip(float u, float v, float q) {
    float lod = (LCM != 0u) ? LODK : log2(1.0 / max(abs(q), 1e-30)) * float(1 << LODL) + LODK;
    if (lod <= 0.0) return sample_lv(0, u, v, MAGLIN != 0u);
    bool lin = (MMIN == 4u || MMIN == 5u);
    if (MMIN == 2u || MMIN == 4u) return sample_lv(min(int(floor(lod + 0.5)), int(MIPL)), u, v, lin);
    int l0 = min(int(floor(lod)), int(MIPL)), l1 = min(l0 + 1, int(MIPL));
    uint w = uint(fract(lod) * 256.0);
    return lerp8(sample_lv(l0, u, v, lin), sample_lv(l1, u, v, lin), w);
}
ivec4 sample_hd(float u, float v) {
    float k = HDK;
    if (WMS == 3u || WMT == 3u) {
        int x = wrap_axis(int(floor(u * k)), WMS, int(TW), int(MINU), int(MAXU), int(k));
        int y = wrap_axis(int(floor(v * k)), WMT, int(TH), int(MINV), int(MAXV), int(k));
        return ivec4(round(texelFetch(hdtex, ivec2(x, y), 0) * 255.0));
    }
    if (WMS == 2u) u = clamp(u, float(MINU) + 0.5 / k, float(MAXU) + 1.0 - 0.5 / k);
    if (WMT == 2u) v = clamp(v, float(MINV) + 0.5 / k, float(MAXV) + 1.0 - 0.5 / k);
    return ivec4(round(texture(hdtex, vec2(u / float(TW), v / float(TH))) * 255.0));
}
vec4 cr_weights(float t) {
    float t2 = t * t, t3 = t2 * t;
    return vec4(-0.5 * t3 + t2 - 0.5 * t, 1.5 * t3 - 2.5 * t2 + 1.0, -1.5 * t3 + 2.0 * t2 + 0.5 * t, 0.5 * t3 - 0.5 * t2);
}
ivec4 sample_font(float u, float v, float lod) {
    float texels = max(fwidth(u), fwidth(v)) * vp.z;
    float fu = u - 0.5, fv = v - 0.5;
    vec2 b = floor(vec2(fu, fv));
    vec4 wx = cr_weights(fu - b.x), wy = cr_weights(fv - b.y);
    vec4 acc = vec4(0.0);
    int xs[4], ys[4];
    for (int j = 0; j < 4; j++) {
        xs[j] = wrap_u(0, int(b.x) - 1 + j);
        ys[j] = wrap_v(0, int(b.y) - 1 + j);
    }
    for (int y = 0; y < 4; y++) {
        vec4 row = vec4(0.0);
        for (int x = 0; x < 4; x++) row += vec4(texel(load_lv(0, xs[x], ys[y]))) * wx[x];
        acc += row * wy[y];
    }
    float cov = clamp(acc.w / FONTAMAX, 0.0, 1.0);
    float w = clamp(fwidth(cov) * 0.75, 0.02, 0.5);
    float a = smoothstep(0.5 - w, 0.5 + w, cov) * FONTAMAX;
    ivec4 sharp = ivec4(clamp(acc.xyz, 0.0, 255.0), a);
    if (texels < 0.75 || texels > 1.5) return ivec4(sample_tex(u, v, lod));
    return sharp;
}

bool atest_pass(uint ua) {
    switch (ATST) {
    case 0u: return false;
    case 1u: return true;
    case 2u: return ua < AREF;
    case 3u: return ua <= AREF;
    case 4u: return ua == AREF;
    case 5u: return ua >= AREF;
    case 6u: return ua > AREF;
    default: return ua != AREF;
    }
}

void main() {
    PST = i_st;
    ivec2 P = ivec2(gl_FragCoord.xy);
    int S = int(vp.z);
#ifndef HW
    uint raw = packUnorm4x8(subpassLoad(fb_in));
    fb_out = unpackUnorm4x8(raw);
#if HAS_Z
    uint zraw = subpassLoad(zb_in).x;
    zb_out = zraw;
    gl_FragDepth = ZD(zraw);
#endif
#endif
    ivec4 c = (IIP != 0u) ? ivec4(i_col) : ivec4(i_fcol);
    int r = c.x, g = c.y, b = c.z, a = c.w;
    if (TME != 0u) {
        float u, v;
        if (FST != 0u) { u = i_stq.x; v = i_stq.y; }
        else {
            float q = (i_stq.z == 0.0) ? 1e-10 : i_stq.z;
            float rq = 1.0 / q;
            u = i_stq.x * rq * float(TW);
            v = i_stq.y * rq * float(TH);
        }
        ivec4 t;
        if (HDTEX != 0.0) t = sample_hd(u, v);
        else if (FONTTEX != 0.0) t = sample_font(u, v, (NEEDLOD != 0u) ? ((abs(i_stq.z) >= QTHR) ? -1.0 : 1.0) : LODK);
        else if (MIPON) t = ivec4(sample_mip(u, v, (FST != 0u) ? 1.0 : i_stq.z));
        else {
            float lod = (NEEDLOD != 0u) ? ((abs(i_stq.z) >= QTHR) ? -1.0 : 1.0) : LODK;
            t = ivec4(sample_tex(u, v, lod));
        }
        switch (TFX) {
        case 0u:
            r = (t.x * r) >> 7; g = (t.y * g) >> 7; b = (t.z * b) >> 7;
            if (TCC != 0u) a = (t.w * a) >> 7;
            break;
        case 1u:
            r = t.x; g = t.y; b = t.z;
            if (TCC != 0u) a = t.w;
            break;
        case 2u:
            r = ((t.x * r) >> 7) + a; g = ((t.y * g) >> 7) + a; b = ((t.z * b) >> 7) + a;
            if (TCC != 0u) a = t.w + a;
            break;
        default:
            r = ((t.x * r) >> 7) + a; g = ((t.y * g) >> 7) + a; b = ((t.z * b) >> 7) + a;
            if (TCC != 0u) a = t.w;
            break;
        }
        r = min(r, 255); g = min(g, 255); b = min(b, 255); a = min(a, 255);
    }
    if (FGE != 0u) {
        int fg = int(i_fog);
        int fr = int(FOGCOL & 0xFFu), fgc = int((FOGCOL >> 8) & 0xFFu), fbc = int((FOGCOL >> 16) & 0xFFu);
        r = (r * fg + fr * (255 - fg)) >> 8;
        g = (g * fg + fgc * (255 - fg)) >> 8;
        b = (b * fg + fbc * (255 - fg)) >> 8;
    }
#ifdef HW
    // hardware path (gs_vk.cpp hw_config): blending, depth test and write masks are pipeline state.
    // f12.x: 0 whole alpha test (fail = discard), 1 the passing part, 2 the failing part (own draw
    // with its own masks); f12.y: 1 = blend factor As/128 in the output alpha (the written alpha
    // then comes from the blend constant 128/255, see there)
    if (ATE != 0u) {
        bool ap = atest_pass(uint(a));
        if (f12.x == 2u) { if (ap) discard; }
        else if (!ap) discard;
    }
#if HAS_Z
    {
        uint zm = (ZFMT == 0u) ? 0xFFFFFFFFu : ((ZFMT == 1u) ? 0xFFFFFFu : 0xFFFFu);
        float zf = i_z;
        uint z = (zf >= 4294967295.0) ? 0xFFFFFFFFu : ((zf <= 0.0) ? 0u : uint(zf));
        z = min(z, zm);
        zb_out = z;
        gl_FragDepth = ZD(z);
    }
#endif
    r = clamp(r, 0, 255); g = clamp(g, 0, 255); b = clamp(b, 0, 255);
    int oa = (int(a) & 0xFF) | int(FBA);
    fb_out = vec4(float(r), float(g), float(b), (f12.y == 1u) ? float(a) * 255.0 / 128.0 : float(oa)) / 255.0;
#else
    bool write_rgb = true, write_a = true, write_z = (ZMSK == 0u);
    if (ATE != 0u) {
        bool apass = atest_pass(uint(a));
        if (!apass) {
            if (AFAIL == 0u) discard;
            else if (AFAIL == 1u) write_z = false;
            else if (AFAIL == 2u) { write_rgb = false; write_a = false; }
            else { write_a = false; write_z = false; }
        }
    }
    uint dst = raw;
    if (FPSM == 2u)
        dst = ((raw & 0x1Fu) << 3) | (((raw >> 5) & 0x1Fu) << 11) | (((raw >> 10) & 0x1Fu) << 19) | ((raw & 0x8000u) << 16);
    if (DATE != 0u && FPSM != 1u) {
        if (((dst >> 31) & 1u) != DATM) return;
    }
#if HAS_Z
    if (ZTE != 0u) {
        uint zm = (ZFMT == 0u) ? 0xFFFFFFFFu : ((ZFMT == 1u) ? 0xFFFFFFu : 0xFFFFu);
        float zf = i_z;
        uint z = (zf >= 4294967295.0) ? 0xFFFFFFFFu : ((zf <= 0.0) ? 0u : uint(zf));
        z = min(z, zm);
        if (ZTST != 1u) {
            uint dz = zraw & zm;
            bool zpass = (ZTST == 0u) ? false : ((ZTST == 2u) ? (z >= dz) : (z > dz));
            if (!zpass) return;
        }
        if (write_z) {
            zb_out = (ZFMT == 1u) ? ((zraw & 0xFF000000u) | z) : ((ZFMT == 2u) ? ((zraw & 0xFFFF0000u) | z) : z);
            gl_FragDepth = ZD(zb_out);
        }
    }
#endif
    if (!write_rgb && !write_a) return;
    if (ABE != 0u && !(PABE != 0u && a < 0x80)) {
        ivec3 cd = ivec3(dst & 0xFFu, (dst >> 8) & 0xFFu, (dst >> 16) & 0xFFu);
        ivec3 cs = ivec3(r, g, b);
        int ad = (FPSM == 1u) ? 0x80 : int(dst >> 24);
        int cc = (BC == 0u) ? a : ((BC == 1u) ? ad : int(BFIX));
        ivec3 A = (BA == 0u) ? cs : ((BA == 1u) ? cd : ivec3(0));
        ivec3 B = (BB == 0u) ? cs : ((BB == 1u) ? cd : ivec3(0));
        ivec3 D = (BD == 0u) ? cs : ((BD == 1u) ? cd : ivec3(0));
        ivec3 o = (((A - B) * cc) >> 7) + D;
        r = o.x; g = o.y; b = o.z;
    }
    if (DTHE != 0u && FPSM == 2u) {
        ivec2 N = P / S;
        int dd = dimx[N.y & 3][N.x & 3];
        r += dd; g += dd; b += dd;
    }
    if (COLCLAMP != 0u) {
        r = clamp(r, 0, 255); g = clamp(g, 0, 255); b = clamp(b, 0, 255);
    } else {
        r &= 0xFF; g &= 0xFF; b &= 0xFF;
    }
    uint outa = (uint(a) & 0xFFu) | FBA;
    uint col = uint(r) | (uint(g) << 8) | (uint(b) << 16) | ((outa & 0xFFu) << 24);
    uint mask = FBMSK;
    if (!write_rgb) mask |= 0x00FFFFFFu;
    if (!write_a) mask |= 0xFF000000u;
    if (mask == 0xFFFFFFFFu) return;
    if (FPSM == 2u) {
        uint c16 = ((col >> 3) & 0x1Fu) | (((col >> 11) & 0x1Fu) << 5) | (((col >> 19) & 0x1Fu) << 10) | ((col >> 16) & 0x8000u);
        uint m16 = ((mask >> 3) & 0x1Fu) | (((mask >> 11) & 0x1Fu) << 5) | (((mask >> 19) & 0x1Fu) << 10) | ((mask >> 16) & 0x8000u);
        fb_out = unpackUnorm4x8((raw & (m16 | 0xFFFF0000u)) | (c16 & ~m16 & 0xFFFFu));
    } else {
        if (FPSM == 1u) mask |= 0xFF000000u;
        fb_out = unpackUnorm4x8((raw & mask) | (col & ~mask));
    }
#endif
}
