#version 450

layout(location = 0) in vec3 fragNormal;
layout(location = 1) in vec3 fragColor;
layout(location = 2) in vec3 fragWorldPos;

/// Final pixel colour written to the render target.
layout(location = 0) out vec4 outColor;

const int MAX_LIGHTS = 16;   // must equal kMaxLights in SceneUniforms.h

const float LIGHT_DIRECTIONAL = 0.0;
const float LIGHT_POINT       = 1.0;
const float LIGHT_SPOT        = 2.0;

/// One light. Mirrors GpuLight in SceneUniforms.h.
struct Light {
    vec4 positionRange;   ///< xyz: world position. w: range.
    vec4 colorType;       ///< rgb: colour * intensity. w: type (LIGHT_*).
    vec4 direction;       ///< xyz: direction the light travels.
    vec4 spotCone;        ///< x: cos of the spot's outer half-angle. y: cos of the inner one.
};

/// Scene-wide data, filled by the CPU once per frame (see SceneUniformData in
/// SceneUniforms.h; the two declarations must match). Set 0 is bound by
/// SceneRenderer before any draw call.
layout(set = 0, binding = 0, std140) uniform SceneData {
    vec4  ambientColor;           ///< Light that reaches every surface (rgb).
    ivec4 lightCount;             ///< x: number of lights in use. y: index of the light that casts shadows, or -1.
    mat4  lightViewProj;          ///< World space -> the shadow map's clip space.
    vec4  shadowParams;           ///< x: one shadow-map texel in UV units. y: normal offset in world units.
    Light lights[MAX_LIGHTS];
} scene;

/// Depth of the scene as seen from the shadow-casting light. A *shadow*
/// sampler: texture() takes a reference depth and returns 1 where the stored
/// depth is not nearer than it (lit) and 0 where it is (shadowed).
layout(set = 0, binding = 1) uniform sampler2DShadow shadowMap;

/// How much of the shadow-casting light reaches a surface point: 1 fully lit,
/// 0 fully in shadow, in between at the soft edge of a shadow.
float shadowFactor(vec3 worldPos, vec3 normal, vec3 toLight) {
    // Normal offset: look the point up slightly above the surface. Surfaces
    // that are steep as seen from the light need more.
    float cosTheta = clamp(dot(normal, toLight), 0.0, 1.0);
    float sinTheta = sqrt(1.0 - cosTheta * cosTheta);
    vec3  lookup   = worldPos + normal * scene.shadowParams.y * (0.3 + sinTheta);

    // To the light's clip space, then to texture coordinates. Clip x and y go
    // from -1 to 1, texture coordinates from 0 to 1. Depth already is 0..1.
    vec4 clip = scene.lightViewProj * vec4(lookup, 1.0);
    vec3 ndc  = clip.xyz / clip.w;
    vec2 uv   = ndc.xy * 0.5 + 0.5;

    // Beyond the light's depth range there is nothing to compare with.
    if (ndc.z < 0.0 || ndc.z > 1.0) return 1.0;

    // Percentage-closer filtering: 3x3 lookups one texel apart, averaged. Each
    // lookup is already blended over four texels by the sampler. textureLod
    // is used because this runs under a non-uniform condition, where the
    // implicit derivatives of texture() would be undefined.
    float texel = scene.shadowParams.x;
    float lit   = 0.0;
    for (int x = -1; x <= 1; ++x)
        for (int y = -1; y <= 1; ++y)
            lit += textureLod(shadowMap, vec3(uv + vec2(x, y) * texel, ndc.z), 0.0);
    return lit / 9.0;
}

/// How much of a light reaches a point at the given distance: 1 close to the
/// light, falling with the square of the distance, and exactly 0 at `range`
/// so that a light never affects anything beyond the reach it declares.
float distanceFalloff(float distance, float range) {
    float window = clamp(1.0 - pow(distance / range, 4.0), 0.0, 1.0);
    return window * window / (distance * distance + 1.0);
}

void main() {
    vec3 normal = normalize(fragNormal);

    vec3 diffuseLight = vec3(0.0);

    int count = min(scene.lightCount.x, MAX_LIGHTS);
    for (int i = 0; i < count; ++i) {
        Light light = scene.lights[i];
        float type  = light.colorType.w;

        vec3  toLight;            // unit vector from the surface towards the light
        float strength = 1.0;     // 0..1: distance falloff and spot cone

        if (type == LIGHT_DIRECTIONAL) {
            // Parallel rays: only the direction matters, never the distance.
            toLight = normalize(-light.direction.xyz);
        } else {
            vec3  offset   = light.positionRange.xyz - fragWorldPos;
            float distance = max(length(offset), 1e-4);
            toLight  = offset / distance;
            strength = distanceFalloff(distance, max(light.positionRange.w, 1e-3));

            if (type == LIGHT_SPOT) {
                // Angle between the cone's axis and the direction towards the
                // surface, compared as cosines (a larger cosine is a smaller
                // angle). Full strength inside the inner cone, nothing outside
                // the outer one, and a smooth fade in between. smoothstep needs
                // its two edges to differ, so the inner cosine is kept a hair
                // above the outer one for a cone with no soft border.
                float cosAngle = dot(-toLight, normalize(light.direction.xyz));
                float cosOuter = light.spotCone.x;
                float cosInner = max(light.spotCone.y, cosOuter + 1e-4);
                strength *= smoothstep(cosOuter, cosInner, cosAngle);
            }
        }

        // Lambert: brightest when the surface faces the light head-on.
        float facing = max(dot(normal, toLight), 0.0);

        // Only one light casts shadows. A surface turned away from the light
        // is dark already, so the lookup is skipped there.
        if (i == scene.lightCount.y && facing > 0.0)
            strength *= shadowFactor(fragWorldPos, normal, toLight);

        diffuseLight += light.colorType.rgb * facing * strength;
    }

    vec3 lit = fragColor * (scene.ambientColor.rgb + diffuseLight);
    outColor = vec4(lit, 1.0);
}
