#version 450

/// Depth-only fragment shader for materials with transparency.
///
/// Cut-out: pixels that the material does not draw (alpha below its cutoff)
/// leave no depth, so they cast no shadow either.
///
/// Translucent: a pixel casts a shadow with a chance equal to the surface's
/// opacity, decided by a noise pattern. Half-opaque glass leaves half the
/// pixels of its shadow in place; the shadow map's filtering blends them into a
/// lighter shadow, without needing a second kind of shadow map.
///
/// Reads set 0, which is the material's own descriptor set (the same one
/// mesh.frag reads as set 1).

layout(location = 0) in vec2 fragUv;

/// Albedo, binding 0 of the material set. Only its alpha matters here.
layout(set = 0, binding = 0) uniform sampler2D albedoMap;

/// The material's numbers; only `alpha` is used. Must match MaterialData in mesh.frag.
layout(set = 0, binding = 4, std140) uniform MaterialData {
    vec4 params;
    vec4 emissive;
    vec4 alpha;      ///< y: cutoff. z: opacity.
} material;

/// Interleaved gradient noise (Jimenez): a cheap, evenly spread value in 0..1
/// for each pixel, with no visible repeating pattern at this scale.
float noise(vec2 pixel) {
    return fract(52.9829189 * fract(dot(pixel, vec2(0.06711056, 0.00583715))));
}

void main() {
    float alpha = texture(albedoMap, fragUv).a * material.alpha.z;

    if (material.alpha.x > 1.5) {
        // Translucent (alpha mode 2). Opaque pixels (alpha 1) always stay, as
        // the noise never reaches 1; fully clear ones (alpha 0) always go.
        if (alpha <= noise(gl_FragCoord.xy)) discard;
    } else if (alpha < material.alpha.y) {
        discard;   // cut-out
    }
}
