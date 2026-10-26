#version 450

/// Fullscreen triangle for the sky: three vertices that cover the whole
/// screen, generated from the vertex index (no vertex buffer). They sit on
/// the far plane (depth 1), so the sky only shows where nothing was drawn.

layout(location = 0) out vec2 ndc;   ///< Position on the screen, -1..1 (Vulkan: +Y is down).

void main() {
    // Index 0, 1, 2 -> (0,0), (2,0), (0,2) -> corners (-1,-1), (3,-1), (-1,3).
    vec2 corner = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    ndc = corner * 2.0 - 1.0;
    gl_Position = vec4(ndc, 1.0, 1.0);
}
