#version 450

/// Fullscreen triangle for the passes that work on whole images (ambient
/// occlusion and its blur): three vertices that cover the screen, made from
/// the vertex index, with no vertex buffer.

layout(location = 0) out vec2 uv;   ///< Position in the image, 0..1, (0, 0) at the top left like the framebuffer.

void main() {
    // Index 0, 1, 2 -> (0,0), (2,0), (0,2): the triangle reaches past the screen.
    vec2 corner = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    uv = corner;
    gl_Position = vec4(corner * 2.0 - 1.0, 0.0, 1.0);
}
