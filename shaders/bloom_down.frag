#version 450

/// One step down of the bloom: the image is shrunk to half its size with the
/// 13-tap filter of "Next Generation Post Processing in Call of Duty:
/// Advanced Warfare" (Jimenez, SIGGRAPH 2014). Thirteen bilinear samples
/// around each output pixel, weighted so that the result is a smooth blur
/// with no shimmering when the picture moves.
///
/// The first step (flags bit 0) also picks out the bright light: everything
/// below the threshold is removed first, and the samples are combined with the
/// "Karis average" (each group weighted by 1 / (1 + brightness)), so that one
/// extremely bright pixel cannot dominate the blur and flicker.

layout(location = 0) in vec2 uv;

layout(set = 0, binding = 0) uniform sampler2D source;

layout(push_constant) uniform Push {
    vec2  texel;            ///< 1 / size of the source image.
    float threshold;        ///< Brightness from which light glows.
    float knee;             ///< Width of the soft transition below the threshold.
    float bloomIntensity;   ///< (final pass only)
    float exposure;         ///< (final pass only)
    int   toneMapper;       ///< (final pass only)
    int   flags;            ///< Bit 0: this is the first step.
} pc;

layout(location = 0) out vec4 outColor;

/// Largest brightness the glow looks at. Light brighter than this (the sun in
/// an HDR sky can be thousands of times white) would still make a huge glow.
const float kMaxBrightness = 64.0;

/// Keeps what is above the threshold, with a soft shoulder of width knee
/// below it, so that the glow fades in instead of switching on.
vec3 prefilter(vec3 colour) {
    colour = min(colour, vec3(kMaxBrightness));
    float brightness = max(colour.r, max(colour.g, colour.b));
    float soft = clamp(brightness - pc.threshold + pc.knee, 0.0, 2.0 * pc.knee);
    soft = soft * soft / (4.0 * pc.knee + 1e-4);
    float contribution = max(soft, brightness - pc.threshold) / max(brightness, 1e-4);
    return colour * contribution;
}

vec3 fetch(vec2 offset, bool first) {
    vec3 colour = textureLod(source, uv + offset * pc.texel, 0.0).rgb;
    return first ? prefilter(colour) : colour;
}

/// Weight of a group of samples in the Karis average.
float karis(vec3 colour) {
    float luma = dot(colour, vec3(0.2126, 0.7152, 0.0722));
    return 1.0 / (1.0 + luma);
}

void main() {
    bool first = (pc.flags & 1) != 0;

    // The 13 samples: a 3x3 grid two pixels apart (a..i) and the four
    // pixels between its corners and its centre (j..m).
    vec3 a = fetch(vec2(-2.0, -2.0), first);
    vec3 b = fetch(vec2( 0.0, -2.0), first);
    vec3 c = fetch(vec2( 2.0, -2.0), first);
    vec3 d = fetch(vec2(-2.0,  0.0), first);
    vec3 e = fetch(vec2( 0.0,  0.0), first);
    vec3 f = fetch(vec2( 2.0,  0.0), first);
    vec3 g = fetch(vec2(-2.0,  2.0), first);
    vec3 h = fetch(vec2( 0.0,  2.0), first);
    vec3 i = fetch(vec2( 2.0,  2.0), first);
    vec3 j = fetch(vec2(-1.0, -1.0), first);
    vec3 k = fetch(vec2( 1.0, -1.0), first);
    vec3 l = fetch(vec2(-1.0,  1.0), first);
    vec3 m = fetch(vec2( 1.0,  1.0), first);

    vec3 result;
    if (first) {
        // Five overlapping groups of four samples, each averaged and weighted.
        vec3 g0 = (a + b + d + e) * 0.25;
        vec3 g1 = (b + c + e + f) * 0.25;
        vec3 g2 = (d + e + g + h) * 0.25;
        vec3 g3 = (e + f + h + i) * 0.25;
        vec3 g4 = (j + k + l + m) * 0.25;

        float w0 = 0.125 * karis(g0);
        float w1 = 0.125 * karis(g1);
        float w2 = 0.125 * karis(g2);
        float w3 = 0.125 * karis(g3);
        float w4 = 0.5   * karis(g4);

        result = (g0 * w0 + g1 * w1 + g2 * w2 + g3 * w3 + g4 * w4) / (w0 + w1 + w2 + w3 + w4);
    } else {
        result = e * 0.125
               + (a + c + g + i) * 0.03125
               + (b + d + f + h) * 0.0625
               + (j + k + l + m) * 0.125;
    }
    outColor = vec4(result, 1.0);
}
