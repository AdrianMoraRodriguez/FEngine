#version 450

layout(location = 0) in vec3 fragNormal;
layout(location = 1) in vec3 fragColor;

/// Final pixel colour written to the render target.
layout(location = 0) out vec4 outColor;

// Temporary: one fixed directional light. Scene lights will replace these
// constants once they are uploaded to the GPU.
const vec3  kLightDirection = normalize(vec3(-0.4, -1.0, -0.6)); // direction the light travels
const vec3  kLightColor     = vec3(1.0);
const float kAmbient        = 0.25;                              // light that reaches every surface

void main() {
    vec3 normal = normalize(fragNormal);

    // Lambert: a surface is brightest when it faces the light head-on and
    // dark when it faces away. The light travels along kLightDirection, so
    // the direction towards the light is its opposite.
    float diffuse = max(dot(normal, -kLightDirection), 0.0);

    vec3 lit = fragColor * kLightColor * (kAmbient + (1.0 - kAmbient) * diffuse);
    outColor = vec4(lit, 1.0);
}
