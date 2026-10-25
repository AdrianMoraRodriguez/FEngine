#version 450

/// A triangle that covers the whole screen, made from the vertex index alone
/// (no vertex buffer). Its three corners are (-1,-1), (3,-1) and (-1,3) in
/// clip space; the part outside the screen is clipped away.
void main() {
    vec2 corner = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    gl_Position = vec4(corner * 2.0 - 1.0, 0.0, 1.0);
}
