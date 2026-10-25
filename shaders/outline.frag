#version 450

/// The selection mask: 1 where the selected object covers the pixel, else 0.
layout(set = 0, binding = 0) uniform sampler2D mask;

layout(push_constant) uniform Params {
    vec4  color;     ///< rgb: outline colour. a: strength.
    vec2  texel;     ///< 1 / size of the mask in pixels.
    float radius;    ///< Thickness of the outline, in pixels.
    float padding;
} pc;

layout(location = 0) out vec4 outColor;

/// How many directions are looked at around each pixel, and at how many
/// distances along each of them.
const int DIRECTIONS = 16;
const int RINGS      = 2;

void main() {
    vec2  uv     = gl_FragCoord.xy * pc.texel;
    float center = texture(mask, uv).r;

    // Inside the object: it is drawn as usual, with no outline over it.
    if (center >= 0.999) discard;

    // The outline is whatever is within `radius` pixels of the object: the
    // largest mask value around this pixel. Reading the mask at several
    // distances keeps a thick outline from breaking into dots.
    float nearby = 0.0;
    for (int ring = 1; ring <= RINGS; ++ring) {
        float distance = pc.radius * float(ring) / float(RINGS);
        for (int i = 0; i < DIRECTIONS; ++i) {
            float angle = 6.2831853 * float(i) / float(DIRECTIONS);
            vec2  step  = vec2(cos(angle), sin(angle)) * distance * pc.texel;
            nearby = max(nearby, texture(mask, uv + step).r);
        }
    }

    // On the object's own border the mask is partly covered, so the outline
    // fades in as the object fades out.
    float alpha = nearby * (1.0 - center) * pc.color.a;
    if (alpha <= 0.0) discard;

    outColor = vec4(pc.color.rgb, alpha);
}
