#version 450

/// The sky behind the scene: the environment map seen in the direction of
/// each pixel. Same tone mapping and encoding as mesh.frag, so the sky and the
/// objects it lights match.

layout(location = 0) in vec2 ndc;

/// The environment (set 0 of this pipeline). Mip 0 is the sky itself.
layout(set = 0, binding = 0) uniform samplerCube environmentMap;

layout(push_constant) uniform Push {
    mat4 inverseViewProj;   ///< Clip space -> world space.
    vec4 params;            ///< x: brightness of the sky.
} pc;

layout(location = 0) out vec4 outColor;

/// Same as in mesh.frag.
vec3 toneMapNeutral(vec3 color) {
    const float startCompression = 0.8 - 0.04;
    const float desaturation     = 0.15;

    float x = min(color.r, min(color.g, color.b));
    float offset = x < 0.08 ? x - 6.25 * x * x : 0.04;
    color -= offset;

    float peak = max(color.r, max(color.g, color.b));
    if (peak < startCompression) return color;

    float d = 1.0 - startCompression;
    float newPeak = 1.0 - d * d / (peak + d - startCompression);
    color *= newPeak / peak;

    float g = 1.0 - 1.0 / (desaturation * (peak - newPeak) + 1.0);
    return mix(color, newPeak * vec3(1.0), g);
}

/// Same as in mesh.frag.
vec3 linearToSrgb(vec3 c) {
    return pow(c, vec3(1.0 / 2.2));
}

/// The world-space point that the pixel sees at the given depth (0 near, 1 far).
vec3 unproject(float depth) {
    vec4 p = pc.inverseViewProj * vec4(ndc, depth, 1.0);
    return p.xyz / p.w;
}

void main() {
    // The ray through this pixel: from where it enters the view to where it leaves.
    vec3 direction = normalize(unproject(1.0) - unproject(0.0));

    vec3 color = textureLod(environmentMap, direction, 0.0).rgb * pc.params.x;
    outColor = vec4(linearToSrgb(toneMapNeutral(color)), 1.0);
}
