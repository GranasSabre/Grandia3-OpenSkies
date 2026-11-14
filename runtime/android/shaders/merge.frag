#version 450
// Display (PRESENT_HLSL ps_merge): PCRTC merge of the two read circuits from surfaces
layout(push_constant) uniform P {
    ivec4 c1;  // dbx, dby (native), psm kind (0 C32, 1 C24, 2 C16), enabled
    ivec4 c2;
    ivec4 m;   // alp, mmod, slbg, field mode
    ivec4 bg;  // background color, scale S
} p;
layout(set = 0, binding = 0) uniform usampler2D s1;
layout(set = 0, binding = 1) uniform usampler2D s2;
layout(location = 0) out vec4 color;
uvec3 rgb_of(uint raw, int kind) {
    if (kind == 2) return uvec3((raw & 0x1Fu) << 3, ((raw >> 5) & 0x1Fu) << 3, ((raw >> 10) & 0x1Fu) << 3);
    return uvec3(raw & 0xFFu, (raw >> 8) & 0xFFu, (raw >> 16) & 0xFFu);
}
uint alpha_of(uint raw, int kind) {
    if (kind == 2) return (raw & 0x8000u) != 0u ? 0x80u : 0u;
    return raw >> 24;
}
void main() {
    ivec2 P = ivec2(gl_FragCoord.xy);
    int S = p.bg.w;
    if (p.m.w != 0) P.y = (P.y / (2 * S)) * S + (P.y % S);  // field mode: each field line shown twice
    uvec3 col2 = uvec3(uint(p.bg.x) & 0xFFu, (uint(p.bg.x) >> 8) & 0xFFu, (uint(p.bg.x) >> 16) & 0xFFu);
    uint raw1 = 0u, raw2 = 0u;
    if (p.c2.w != 0) { raw2 = texelFetch(s2, ivec2(p.c2.x * S + P.x, p.c2.y * S + P.y), 0).x; col2 = rgb_of(raw2, p.c2.z); }
    uvec3 o = col2;
    if (p.c1.w != 0) {
        raw1 = texelFetch(s1, ivec2(p.c1.x * S + P.x, p.c1.y * S + P.y), 0).x;
        uvec3 col1 = rgb_of(raw1, p.c1.z);
        if (p.c2.w != 0 && p.m.z == 0) {
            uint a = (p.m.y != 0) ? uint(p.m.x) : (p.c1.z == 1 ? 0x80u : alpha_of(raw1, p.c1.z));
            a = min(a, 128u);
            o = min((col1 * a + col2 * (128u - a)) >> 7, uvec3(255u));
        } else if (p.c2.w == 0) {
            o = col1;
        }
    }
    color = vec4(vec3(o) / 255.0, 1.0);
}
