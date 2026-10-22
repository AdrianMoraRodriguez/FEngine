#version 450

layout(location = 0) in vec3 fragNormal;
layout(location = 1) in vec3 fragColor;
layout(location = 2) in vec3 fragWorldPos;

/// Final pixel colour written to the render target.
layout(location = 0) out vec4 outColor;

const int MAX_LIGHTS         = 16;   // must equal kMaxLights in SceneUniforms.h
const int MAX_SHADOW_CASTERS = 4;    // must equal kMaxShadowCasters in SceneUniforms.h
const int MAX_POINT_SHADOW_CASTERS = 2;   // must equal kMaxPointShadowCasters in SceneUniforms.h

const float LIGHT_DIRECTIONAL = 0.0;
const float LIGHT_POINT       = 1.0;
const float LIGHT_SPOT        = 2.0;

/// One light. Mirrors GpuLight in SceneUniforms.h.
struct Light {
    vec4 positionRange;   ///< xyz: world position. w: range.
    vec4 colorType;       ///< rgb: colour * intensity. w: type (LIGHT_*).
    vec4 direction;       ///< xyz: direction the light travels.
    vec4 spotCone;        ///< x: cos of the spot's outer half-angle. y: cos of the inner one. z: shadow map slot, or -1.
};

/// Scene-wide data, filled by the CPU once per frame (see SceneUniformData in
/// SceneUniforms.h; the two declarations must match). Set 0 is bound by
/// SceneRenderer before any draw call.
layout(set = 0, binding = 0, std140) uniform SceneData {
    vec4  ambientColor;                        ///< Light that reaches every surface (rgb).
    ivec4 lightCount;                          ///< x: number of lights in use.
    mat4  shadowViewProj[MAX_SHADOW_CASTERS];  ///< World space -> each shadow map's clip space.
    vec4  shadowParams[MAX_SHADOW_CASTERS];    ///< x: one texel in UV units. y: normal offset. z: 1 if y scales with distance.
    vec4  pointShadowParams;                   ///< Shadow cubes. x: texel size per unit of distance. y: normal offset per unit of distance. z: near plane.
    Light lights[MAX_LIGHTS];
} scene;

/// Depth of the scene as seen from each shadow-casting light. *Shadow*
/// samplers: texture() takes a reference depth and returns 1 where the stored
/// depth is not nearer than it (lit) and 0 where it is (shadowed).
layout(set = 0, binding = 1) uniform sampler2DShadow shadowMaps[MAX_SHADOW_CASTERS];

/// The shadow cubes of the point lights: six faces each, read as one cube
/// with a direction instead of a 2D position. Same comparison sampler idea.
layout(set = 0, binding = 2) uniform samplerCubeShadow shadowCubes[MAX_POINT_SHADOW_CASTERS];

/// Reads shadow cube `slot`. `coord.xyz` is the direction from the light and
/// `coord.w` the reference depth. textureGrad with zero gradients is used
/// because this runs under a non-uniform condition, where the implicit
/// derivatives of texture() would be undefined (and textureLod does not
/// exist for cube shadow samplers).
float lookupCube(int slot, vec4 coord) {
    switch (slot) {
        case 0:  return textureGrad(shadowCubes[0], coord, vec3(0.0), vec3(0.0));
        default: return textureGrad(shadowCubes[1], coord, vec3(0.0), vec3(0.0));
    }
}

/// Reads shadow map `slot`. Every index is a constant on purpose: indexing a
/// sampler array with a runtime value is an optional GPU feature.
float lookupShadow(int slot, vec3 coord) {
    switch (slot) {
        case 0:  return textureLod(shadowMaps[0], coord, 0.0);
        case 1:  return textureLod(shadowMaps[1], coord, 0.0);
        case 2:  return textureLod(shadowMaps[2], coord, 0.0);
        default: return textureLod(shadowMaps[3], coord, 0.0);
    }
}

/// How much of a shadow-casting light reaches a surface point: 1 fully lit,
/// 0 fully in shadow, in between at the soft edge of a shadow.
/// `lightDistance` is the distance to the light (only used by spot lights).
float shadowFactor(int slot, vec3 worldPos, vec3 normal, vec3 toLight, float lightDistance) {
    vec4 params = scene.shadowParams[slot];

    // Normal offset: look the point up slightly above the surface. Surfaces
    // that are steep as seen from the light need more. For a spot light the
    // texels grow with distance, so the offset does too.
    float cosTheta = clamp(dot(normal, toLight), 0.0, 1.0);
    float sinTheta = sqrt(1.0 - cosTheta * cosTheta);
    float offset   = params.y * mix(1.0, lightDistance, params.z);
    vec3  lookup   = worldPos + normal * offset * (0.3 + sinTheta);

    // To the light's clip space, then to texture coordinates. Clip x and y go
    // from -1 to 1, texture coordinates from 0 to 1. Depth already is 0..1.
    // A spot light's projection is perspective, so divide by w; points behind
    // the light have no place in the map.
    vec4 clip = scene.shadowViewProj[slot] * vec4(lookup, 1.0);
    if (clip.w <= 0.0) return 1.0;
    vec3 ndc = clip.xyz / clip.w;
    vec2 uv  = ndc.xy * 0.5 + 0.5;

    // Beyond the light's depth range there is nothing to compare with.
    if (ndc.z < 0.0 || ndc.z > 1.0) return 1.0;

    // Percentage-closer filtering: 3x3 lookups one texel apart, averaged. Each
    // lookup is already blended over four texels by the sampler. textureLod
    // is used because this runs under a non-uniform condition, where the
    // implicit derivatives of texture() would be undefined.
    float texel = params.x;
    float lit   = 0.0;
    for (int x = -1; x <= 1; ++x)
        for (int y = -1; y <= 1; ++y)
            lit += lookupShadow(slot, vec3(uv + vec2(x, y) * texel, ndc.z));
    return lit / 9.0;
}

/// Like shadowFactor(), for a point light and its shadow cube.
///
/// The face that holds a point is the one the direction from the light points
/// at most: the axis with the largest absolute value. The distance along that
/// axis is what the face's projection turned into depth, so the same
/// formula gives the reference depth to compare with:
///     depth = far * (d - near) / (d * (far - near))
float pointShadowFactor(int slot, vec3 worldPos, vec3 normal, vec3 toLight,
                        float lightDistance, vec3 lightPos, float range) {
    vec4 params = scene.pointShadowParams;

    float cosTheta = clamp(dot(normal, toLight), 0.0, 1.0);
    float sinTheta = sqrt(1.0 - cosTheta * cosTheta);
    vec3  lookup   = worldPos + normal * params.y * lightDistance * (0.3 + sinTheta);

    vec3  dir   = lookup - lightPos;
    vec3  a     = abs(dir);
    float major = max(a.x, max(a.y, a.z));

    float nearPlane = params.z;
    float farPlane  = max(range, nearPlane + 0.1);
    if (major <= nearPlane || major >= farPlane) return 1.0;
    float reference = farPlane * (major - nearPlane) / (major * (farPlane - nearPlane));

    // Percentage-closer filtering: 3x3 lookups one texel apart, in the plane
    // across the direction. A texel is `params.x * major` wide there.
    vec3 helper = (a.y < 0.9 * length(dir)) ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
    vec3 side1  = normalize(cross(dir, helper));
    vec3 side2  = normalize(cross(dir, side1));
    float texel = params.x * major;

    float lit = 0.0;
    for (int x = -1; x <= 1; ++x)
        for (int y = -1; y <= 1; ++y)
            lit += lookupCube(slot, vec4(dir + (float(x) * side1 + float(y) * side2) * texel, reference));
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
        float lightDistance = 0.0;   // distance to the light (point and spot)

        if (type == LIGHT_DIRECTIONAL) {
            // Parallel rays: only the direction matters, never the distance.
            toLight = normalize(-light.direction.xyz);
        } else {
            vec3  offset   = light.positionRange.xyz - fragWorldPos;
            float distance = max(length(offset), 1e-4);
            toLight  = offset / distance;
            lightDistance = distance;
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

        // A light with a shadow map checks it. Surfaces turned away from the
        // light, or outside its cone, are dark already, so the lookup is skipped.
        int shadowSlot = int(light.spotCone.z);
        if (shadowSlot >= 0 && facing > 0.0 && strength > 0.0) {
            if (type == LIGHT_POINT)
                strength *= pointShadowFactor(shadowSlot, fragWorldPos, normal, toLight,
                                              lightDistance, light.positionRange.xyz,
                                              light.positionRange.w);
            else
                strength *= shadowFactor(shadowSlot, fragWorldPos, normal, toLight, lightDistance);
        }

        diffuseLight += light.colorType.rgb * facing * strength;
    }

    vec3 lit = fragColor * (scene.ambientColor.rgb + diffuseLight);
    outColor = vec4(lit, 1.0);
}
