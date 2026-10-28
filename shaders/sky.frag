#version 450

/// The sky behind the scene: the environment map seen in the direction of
/// each pixel, as linear HDR light like the objects it lights. Tone mapping
/// happens afterwards for the whole picture (PostProcess).

layout(location = 0) in vec2 ndc;

/// The environment (set 0 of this pipeline). Mip 0 is the sky itself.
layout(set = 0, binding = 0) uniform samplerCube environmentMap;

layout(push_constant) uniform Push {
    mat4 inverseViewProj;   ///< Clip space -> world space.
    vec4 params;            ///< x: brightness of the sky.
} pc;

layout(location = 0) out vec4 outColor;

/// The world-space point that the pixel sees at the given depth (0 near, 1 far).
vec3 unproject(float depth) {
    vec4 p = pc.inverseViewProj * vec4(ndc, depth, 1.0);
    return p.xyz / p.w;
}

void main() {
    // The ray through this pixel: from where it enters the view to where it leaves.
    vec3 direction = normalize(unproject(1.0) - unproject(0.0));

    vec3 color = textureLod(environmentMap, direction, 0.0).rgb * pc.params.x;
    outColor = vec4(min(color, vec3(60000.0)), 1.0);   // the cap only avoids overflowing the 16-bit float
}
