#version 450
// full-target triangle strip (4 vertices)
void main() {
    vec2 t = vec2(float(gl_VertexIndex & 1), float(gl_VertexIndex >> 1));
    gl_Position = vec4(t * 2.0 - 1.0, 0.0, 1.0);
}
