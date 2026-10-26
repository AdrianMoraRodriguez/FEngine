#version 450

/// Depth-only fragment shader for cut-out materials: pixels that the material
/// does not draw (alpha below its cutoff) leave no depth, so they cast no
/// shadow either. Reads set 0, which is the material's own descriptor set
/// (the same one mesh.frag reads as set 1).

layout(location = 0) in vec2 fragUv;

/// Albedo, binding 0 of the material set. Only its alpha matters here.
layout(set = 0, binding = 0) uniform sampler2D albedoMap;

/// The material's numbers; only `alpha` is used. Must match MaterialData in mesh.frag.
layout(set = 0, binding = 4, std140) uniform MaterialData {
    vec4 params;
    vec4 emissive;
    vec4 alpha;      ///< y: cutoff. z: opacity.
} material;

void main() {
    if (texture(albedoMap, fragUv).a * material.alpha.z < material.alpha.y) discard;
}
