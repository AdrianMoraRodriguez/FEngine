#version 450

/// One step up of the bloom: the smaller image is enlarged with a 3x3 tent
/// filter (a smooth blur) and ADDED to the bigger image it is drawn into, so
/// that the glow gathers the blurs of every size.

layout(location = 0) in vec2 uv;

layout(set = 0, binding = 0) uniform sampler2D source;

layout(push_constant) uniform Push {
    vec2  texel;            ///< 1 / size of the source (the smaller image).
    float threshold;        ///< (down passes only)
    float knee;             ///< (down passes only)
    float bloomIntensity;   ///< (final pass only)
    float exposure;         ///< (final pass only)
    int   toneMapper;       ///< (final pass only)
    int   flags;            ///< (unused here)
} pc;

layout(location = 0) out vec4 outColor;

void main() {
    vec3 a = textureLod(source, uv + vec2(-1.0,  1.0) * pc.texel, 0.0).rgb;
    vec3 b = textureLod(source, uv + vec2( 0.0,  1.0) * pc.texel, 0.0).rgb;
    vec3 c = textureLod(source, uv + vec2( 1.0,  1.0) * pc.texel, 0.0).rgb;
    vec3 d = textureLod(source, uv + vec2(-1.0,  0.0) * pc.texel, 0.0).rgb;
    vec3 e = textureLod(source, uv,                               0.0).rgb;
    vec3 f = textureLod(source, uv + vec2( 1.0,  0.0) * pc.texel, 0.0).rgb;
    vec3 g = textureLod(source, uv + vec2(-1.0, -1.0) * pc.texel, 0.0).rgb;
    vec3 h = textureLod(source, uv + vec2( 0.0, -1.0) * pc.texel, 0.0).rgb;
    vec3 i = textureLod(source, uv + vec2( 1.0, -1.0) * pc.texel, 0.0).rgb;

    // 1 2 1
    // 2 4 2   / 16
    // 1 2 1
    vec3 result = e * 4.0 + (b + d + f + h) * 2.0 + (a + c + g + i);
    outColor = vec4(result * (1.0 / 16.0), 0.0);
}
