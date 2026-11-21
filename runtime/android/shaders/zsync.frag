#version 450
// start of a pass with a z surface: the depth attachment takes the z surface's values (draw.frag ZD);
// a full-target triangle strip (full.vert) with depth test always, colour writes off
layout(input_attachment_index = 1, set = 0, binding = 11) uniform usubpassInput zb_in;
void main() { gl_FragDepth = float(subpassLoad(zb_in).x) * (1.0 / 4294967296.0); }
