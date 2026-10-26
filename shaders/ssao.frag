#version 450

/// Screen-space ambient occlusion: how much of the hemisphere above each
/// pixel's surface is blocked by nearby geometry. Written as 1 (open) to 0
/// (closed), then blurred by blur.frag and read by mesh.frag, which dims only
/// the ambient and sky light with it: direct lights are not occluded.
///
/// Only the depth image is needed. The surface normal is rebuilt from the
/// depth of the neighbouring pixels.

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

/// Sample directions in a hemisphere around +Z, denser near the centre.
const int SAMPLES = 16;
const vec3 kernel[SAMPLES] = vec3[](
    vec3(0.0271, 0.0000, 0.1509),
    vec3(-0.0369, 0.0338, 0.1554),
    vec3(0.0062, -0.0708, 0.1652),
    vec3(0.0578, 0.0754, 0.1795),
    vec3(-0.1217, -0.0215, 0.1975),
    vec3(0.1333, -0.0848, 0.2183),
    vec3(-0.0517, 0.1925, 0.2409),
    vec3(-0.1144, -0.2203, 0.2642),
    vec3(0.2868, 0.1047, 0.2868),
    vec3(-0.3433, 0.1417, 0.3072),
    vec3(0.1894, -0.4048, 0.3235),
    vec3(0.1594, 0.5081, 0.3331),
    vec3(-0.5438, -0.3152, 0.3326),
    vec3(0.7184, -0.1579, 0.3165),
    vec3(-0.4912, 0.6986, 0.2747),
    vec3(-0.1265, -0.9761, 0.1768)
);

/// A different rotation of the kernel for each pixel of a 4 x 4 tile, so that
/// the blur (which covers more than the tile) averages the pattern away.
float tileRotation() {
    const float pattern[16] = float[](
         0.0,  8.0,  2.0, 10.0,
        12.0,  4.0, 14.0,  6.0,
         3.0, 11.0,  1.0,  9.0,
        15.0,  7.0, 13.0,  5.0);
    ivec2 p = ivec2(gl_FragCoord.xy) & 3;
    return (pattern[p.y * 4 + p.x] + 0.5) / 16.0 * 6.2831853;
}

void main() {
    float stored = storedDepth(uv);
    if (stored >= 1.0) {          // nothing was drawn here: the sky
        outColor = vec4(1.0);
        return;
    }

    vec3 position = viewPosition(uv, stored);

    // The normal, from the neighbouring pixels. For each axis the neighbour on
    // the side that is closer in depth is used, so that the edge of an object
    // does not bend the normal towards the background.
    vec2 texel = 1.0 / vec2(textureSize(depthMap, 0));
    vec3 right = viewPosition(uv + vec2( texel.x, 0.0), storedDepth(uv + vec2( texel.x, 0.0)));
    vec3 left  = viewPosition(uv + vec2(-texel.x, 0.0), storedDepth(uv + vec2(-texel.x, 0.0)));
    vec3 down  = viewPosition(uv + vec2(0.0,  texel.y), storedDepth(uv + vec2(0.0,  texel.y)));
    vec3 up    = viewPosition(uv + vec2(0.0, -texel.y), storedDepth(uv + vec2(0.0, -texel.y)));
    vec3 dx = abs(right.z - position.z) < abs(position.z - left.z) ? right - position : position - left;
    vec3 dy = abs(down.z  - position.z) < abs(position.z - up.z)   ? down  - position : position - up;
    vec3 normal = normalize(cross(dx, dy));
    // Facing the camera (the pixel's positive-Z side).
    if (normal.z < 0.0) normal = -normal;

    // A frame around the normal, turned by this pixel's own angle.
    float angle = tileRotation();
    vec3 randomDir = vec3(cos(angle), sin(angle), 0.0);
    vec3 tangent   = normalize(randomDir - normal * dot(randomDir, normal));
    vec3 bitangent = cross(normal, tangent);

    float radius = pc.params.x;
    float bias   = 0.02 + 0.03 * radius;   // keeps flat surfaces from shadowing themselves

    float occlusion = 0.0;
    for (int i = 0; i < SAMPLES; ++i) {
        vec3 offset = tangent * kernel[i].x + bitangent * kernel[i].y + normal * kernel[i].z;
        vec3 samplePos = position + offset * radius;

        // Where that point is on screen, and what the depth buffer holds there.
        vec4 clip = pc.projection * vec4(samplePos, 1.0);
        vec2 sampleUv = clip.xy / clip.w * 0.5 + 0.5;
        if (sampleUv.x < 0.0 || sampleUv.x > 1.0 || sampleUv.y < 0.0 || sampleUv.y > 1.0) continue;

        float sceneStored = storedDepth(sampleUv);
        if (sceneStored >= 1.0) continue;
        float sceneZ = viewZ(sceneStored);

        // Occluded when the surface seen there is nearer to the camera than the
        // sample. A surface far in front of this pixel (another object) must
        // not count: the weight fades out as the depth gap exceeds the radius.
        float rangeCheck = smoothstep(0.0, 1.0, radius / abs(position.z - sceneZ));
        occlusion += (sceneZ >= samplePos.z + bias ? 1.0 : 0.0) * rangeCheck;
    }

    float visibility = 1.0 - occlusion / float(SAMPLES);
    outColor = vec4(pow(clamp(visibility, 0.0, 1.0), max(pc.params.y, 0.0)));
}
