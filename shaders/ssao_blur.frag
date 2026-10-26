#version 450

/// Blurs the ambient occlusion along one axis (run twice: across, then down).
/// Bilateral: a neighbour only counts as much as its depth is close to the
/// centre's, so the occlusion does not bleed over the edges of objects.

layout(location = 0) in vec2 uv;

/// Set 0 is shared by both passes. Binding 0 is the image the pass blurs (the
/// ambient occlusion pass does not read it); binding 1 is the scene's depth.
layout(set = 0, binding = 0) uniform sampler2D inputMap;
layout(set = 0, binding = 1) uniform sampler2D depthMap;

layout(push_constant) uniform Push {
    mat4 projection;   ///< The camera's projection (Vulkan clip space, as the editor camera makes it).
    vec4 params;       ///< SSAO: x radius (world units), y intensity. Blur: xy step between taps, in uv units.
} pc;

layout(location = 0) out vec4 outColor;

/// Depth of the nearest surface at `uv` as the depth buffer stores it (0 near, 1 far).
float storedDepth(vec2 uv) {
    return textureLod(depthMap, uv, 0.0).r;
}

/// Position in front of the camera (view space: the camera looks down -Z) of
/// the surface that a depth value belongs to. The projection keeps two numbers
/// for depth: stored = -A - B / z, so z = -B / (stored + A).
float viewZ(float stored) {
    return -pc.projection[3][2] / (stored + pc.projection[2][2]);
}

/// The view-space position of the surface seen at `uv`.
vec3 viewPosition(vec2 uv, float stored) {
    float z   = viewZ(stored);
    vec2  ndc = uv * 2.0 - 1.0;
    return vec3(ndc.x * -z / pc.projection[0][0],
                ndc.y * -z / pc.projection[1][1],
                z);
}

const int RADIUS = 3;   // taps on each side

void main() {
    float centerStored = storedDepth(uv);
    if (centerStored >= 1.0) {
        outColor = vec4(1.0);
        return;
    }
    float centerZ = viewZ(centerStored);

    // How far apart in depth two pixels may be and still be the same surface.
    float tolerance = 0.03 * abs(centerZ) + 0.02;

    float sum = 0.0, weightSum = 0.0;
    for (int i = -RADIUS; i <= RADIUS; ++i) {
        vec2  tap    = uv + pc.params.xy * float(i);
        float stored = storedDepth(tap);
        if (stored >= 1.0) continue;

        float spatial = exp(-0.5 * float(i * i) / 4.0);
        float depthDiff = abs(viewZ(stored) - centerZ);
        float weight = spatial * exp(-depthDiff / tolerance);

        sum       += textureLod(inputMap, tap, 0.0).r * weight;
        weightSum += weight;
    }
    outColor = vec4(weightSum > 0.0 ? sum / weightSum : 1.0);
}
