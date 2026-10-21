#version 450

/// Per-vertex input.
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec3 inColor;

/// Interpolated outputs to the fragment shader.
layout(location = 0) out vec3 fragNormal;   ///< Normal in world space.
layout(location = 1) out vec3 fragColor;

/// Push constants: one block per draw call, no descriptor needed.
layout(push_constant) uniform PushConstants {
    mat4 mvp;     ///< Model-View-Projection matrix.
    mat4 model;   ///< Object-to-world matrix, used to bring normals to world space.
} pc;

void main() {
    gl_Position = pc.mvp * vec4(inPosition, 1.0);

    // Normals are transformed by the inverse transpose of the model matrix.
    // With a plain model matrix they would stop being perpendicular to the
    // surface as soon as the object is scaled non-uniformly.
    fragNormal = normalize(transpose(inverse(mat3(pc.model))) * inNormal);
    fragColor  = inColor;
}
