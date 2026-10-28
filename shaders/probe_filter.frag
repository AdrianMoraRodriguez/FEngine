#version 450

/// Blurs a photographed environment the way a surface of a given roughness
/// would blur its reflection (see ReflectionProbes). One pass draws one face of
/// one mip level of a probe's cube map, reading the sharp photograph.
///
/// For each output texel the direction it stands for is the mirror direction
/// of a surface facing that way; the result is the average of the photograph
/// around that direction, weighted by the GGX lobe of the roughness. The
/// samples are picked with a low-discrepancy sequence and read from a blurrier
/// mip level of the photograph the farther apart they are, so that few
/// samples are enough without speckles.

layout(set = 0, binding = 0) uniform samplerCube sourceCubes[8];   // the first one is the photograph; the set shares the layout of the probes' set

layout(push_constant) uniform Push {
    int   face;          ///< Cube face being drawn, in the order of Vulkan's cube map faces (+X, -X, +Y, -Y, +Z, -Z).
    float roughness;     ///< 0 = mirror, 1 = completely matte.
    int   sampleCount;   ///< How many samples to average. 1 for the mirror level.
    float sourceSize;    ///< Width of one face of the photograph, in texels.
} pc;

layout(location = 0) in  vec2 uv;
layout(location = 0) out vec4 outColor;

const float PI = 3.14159265359;

/// The direction a texel of a cube face looks at. `a` is the texel's position
/// on the face in -1..1, x to the right and y down, as the image is stored.
/// Mirrors the face selection table of the Vulkan specification.
vec3 faceDirection(int face, vec2 a) {
    switch (face) {
        case 0:  return vec3( 1.0, -a.y, -a.x);
        case 1:  return vec3(-1.0, -a.y,  a.x);
        case 2:  return vec3( a.x,  1.0,  a.y);
        case 3:  return vec3( a.x, -1.0, -a.y);
        case 4:  return vec3( a.x, -a.y,  1.0);
        default: return vec3(-a.x, -a.y, -1.0);
    }
}

vec2 hammersley(uint i, uint n) {
    return vec2(float(i) / float(n), float(bitfieldReverse(i)) * 2.3283064365386963e-10);
}

/// A half vector chosen with the GGX distribution of width `alpha`, around `n`.
vec3 importanceSampleGGX(vec2 xi, float alpha, vec3 n) {
    float phi      = 2.0 * PI * xi.x;
    float cosTheta = sqrt((1.0 - xi.y) / (1.0 + (alpha * alpha - 1.0) * xi.y));
    float sinTheta = sqrt(max(1.0 - cosTheta * cosTheta, 0.0));
    vec3 h = vec3(sinTheta * cos(phi), sinTheta * sin(phi), cosTheta);

    vec3 up = abs(n.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
    vec3 tx = normalize(cross(up, n));
    vec3 ty = cross(n, tx);
    return tx * h.x + ty * h.y + n * h.z;
}

float distributionGGX(float nDotH, float alpha) {
    float a2 = alpha * alpha;
    float d  = nDotH * nDotH * (a2 - 1.0) + 1.0;
    return a2 / (PI * d * d);
}

void main() {
    vec3 n = normalize(faceDirection(pc.face, uv * 2.0 - 1.0));

    // The mirror level is the photograph itself.
    if (pc.sampleCount <= 1) {
        outColor = vec4(min(textureLod(sourceCubes[0], n, 0.0).rgb, vec3(10000.0)), 1.0);
        return;
    }

    float alpha = pc.roughness * pc.roughness;
    float solidAnglePerTexel = 4.0 * PI / (6.0 * pc.sourceSize * pc.sourceSize);

    vec3  sum    = vec3(0.0);
    float weight = 0.0;
    for (int i = 0; i < pc.sampleCount; ++i) {
        vec3  h    = importanceSampleGGX(hammersley(uint(i), uint(pc.sampleCount)), alpha, n);
        vec3  l    = normalize(2.0 * dot(n, h) * h - n);   // with the view along the normal
        float nDotL = dot(n, l);
        if (nDotL <= 0.0) continue;

        // The sample stands for a patch of the sphere: read the mip level whose texels are as big.
        float nDotH = max(dot(n, h), 0.0);
        float pdf   = distributionGGX(nDotH, alpha) * 0.25;
        float sampleSolidAngle = 1.0 / (float(pc.sampleCount) * pdf + 1e-4);
        float lod = max(0.5 * log2(sampleSolidAngle / solidAnglePerTexel) + 1.0, 0.0);

        sum    += min(textureLod(sourceCubes[0], l, lod).rgb, vec3(10000.0)) * nDotL;
        weight += nDotL;
    }
    outColor = vec4(sum / max(weight, 1e-4), 1.0);
}
