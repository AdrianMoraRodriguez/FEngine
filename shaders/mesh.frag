#version 450

/// Interpolated colour from the vertex shader.
layout(location = 0) in vec3 fragColor;

/// Final pixel colour written to the render target.
layout(location = 0) out vec4 outColor;

void main() {
    outColor = vec4(fragColor, 1.0);
}
