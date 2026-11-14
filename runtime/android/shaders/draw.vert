#version 450
// GS draw: vertices in native pixels (GV in gs_vk.cpp, same layout as gs_gpu.cpp)
layout(std140, set = 0, binding = 0) uniform CB {
    vec4 vp;  // surface width, height (native pixels), scale S
    uvec4 f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10;
    vec4 f11;
    uvec4 f12;
    ivec4 f13;
    uvec4 f14;
    vec4 f15;
    ivec4 dimx[4];
};
layout(location = 0) in vec2 i_pos;
layout(location = 1) in float i_z;
layout(location = 2) in vec4 i_col;
layout(location = 3) in vec3 i_stq;
layout(location = 4) in float i_fog;
layout(location = 5) in vec4 i_fcol;
layout(location = 6) in uvec2 i_st;
layout(location = 0) noperspective out vec4 o_col;
layout(location = 1) flat out vec4 o_fcol;
layout(location = 2) noperspective out vec3 o_stq;
layout(location = 3) noperspective out float o_z;
layout(location = 4) noperspective out float o_fog;
layout(location = 5) flat out uvec2 o_st;
void main() {
    float S = vp.z;
    // GS sample point x (pixel top-left) lands on the center of the first of its S x S pixels
    vec2 d = i_pos * S + 0.5;
    gl_Position = vec4(d.x / (vp.x * S) * 2.0 - 1.0, d.y / (vp.y * S) * 2.0 - 1.0, 0.5, 1.0);
    o_col = i_col;
    o_fcol = i_fcol;
    o_stq = i_stq;
    o_z = i_z;
    o_fog = i_fog;
    o_st = i_st;
}
