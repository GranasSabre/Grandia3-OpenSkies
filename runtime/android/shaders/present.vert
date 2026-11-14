#version 450
// Full-screen presentation: a quad for the destination rectangle (normalized device coordinates)
// with the source rectangle in texture coordinates.
layout(push_constant) uniform P {
    vec4 dst;  // x0, y0, x1, y1 in NDC
    vec4 src;  // u0, v0, u1, v1
} p;
layout(location = 0) out vec2 uv;
void main() {
    vec2 c = vec2(float(gl_VertexIndex & 1), float(gl_VertexIndex >> 1));  // triangle strip corners
    gl_Position = vec4(mix(p.dst.xy, p.dst.zw, c), 0.0, 1.0);
    uv = mix(p.src.xy, p.src.zw, c);
}
