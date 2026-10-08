#version 450

/// Per-vertex input.
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inColor;

/// Interpolated output to the fragment shader.
layout(location = 0) out vec3 fragColor;

/// Push constant block — one matrix per draw call, zero descriptor overhead.
layout(push_constant) uniform PushConstants {
    mat4 mvp;   ///< Model-View-Projection matrix.
} pc;

void main() {
    gl_Position = pc.mvp * vec4(inPosition, 1.0);
    fragColor   = inColor;
}
