#version 450

/// The last step of the picture: adds the bloom glow to the HDR scene,
/// applies the exposure, squeezes the light into the 0..1 range with the
/// chosen tone mapping curve and encodes it for the screen.
///
/// Everything before this is linear light that may be far brighter than 1;
/// everything after it is what the screen shows.

layout(location = 0) in vec2 uv;

layout(set = 0, binding = 0) uniform sampler2D sceneColor;   ///< The scene as HDR linear light.
layout(set = 1, binding = 0) uniform sampler2D bloomColor;   ///< The glow (half size). Only read when flags bit 0 is set.

layout(push_constant) uniform Push {
    vec2  texel;            ///< (unused here)
    float threshold;        ///< (unused here)
    float knee;             ///< (unused here)
    float bloomIntensity;   ///< Strength of the glow, already divided by the number of levels.
    float exposure;         ///< Brightness multiplier (2 to the power of the EV).
    int   toneMapper;       ///< 0 Neutral, 1 ACES, 2 Reinhard, 3 Linear (a ToneMapper).
    int   flags;            ///< Bit 0: add the glow.
} pc;

layout(location = 0) out vec4 outColor;

/// The Khronos "PBR Neutral" curve. Colours below the shoulder (about 0.76)
/// pass through unchanged, and above it the brightness is rolled off smoothly
/// while the hue is kept. A filmic curve works on each channel separately and
/// so pushes a saturated bright colour towards white; this one does not, which
/// keeps coloured lights and coloured textures looking like themselves.
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

/// A fit of the ACES filmic curve by Krzysztof Narkowicz: strong contrast,
/// deep darks, and bright colours that drift towards white.
vec3 toneMapAces(vec3 x) {
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

/// Reinhard: x / (1 + x). Soft and simple; colours come out a little washed out.
vec3 toneMapReinhard(vec3 x) {
    return x / (1.0 + x);
}

/// Converts linear light into the encoding of a screen. A power-2.2
/// approximation of the exact sRGB curve, the same the textures use when read.
vec3 linearToSrgb(vec3 c) {
    return pow(c, vec3(1.0 / 2.2));
}

void main() {
    vec3 color = texture(sceneColor, uv).rgb;
    if ((pc.flags & 1) != 0)
        color += texture(bloomColor, uv).rgb * pc.bloomIntensity;

    color *= pc.exposure;
    color = max(color, vec3(0.0));

    if      (pc.toneMapper == 0) color = toneMapNeutral(color);
    else if (pc.toneMapper == 1) color = toneMapAces(color);
    else if (pc.toneMapper == 2) color = toneMapReinhard(color);

    outColor = vec4(linearToSrgb(clamp(color, 0.0, 1.0)), 1.0);
}
