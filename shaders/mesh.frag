#version 450

layout(location = 0) in vec3 fragNormal;
layout(location = 1) in vec3 fragColor;
layout(location = 2) in vec3 fragWorldPos;
layout(location = 3) in vec2 fragUv;

/// Albedo (colour) texture of the material. Set 1 is bound for every draw; an
/// object without a texture gets a 1x1 white one, so this multiplies by 1.
/// The image is stored as sRGB, so the GPU hands back linear values.
layout(set = 1, binding = 0) uniform sampler2D albedoMap;

/// Final pixel colour written to the render target.
layout(location = 0) out vec4 outColor;

const int MAX_LIGHTS         = 16;   // must equal kMaxLights in SceneUniforms.h
const int MAX_SHADOW_CASTERS = 4;    // must equal kMaxShadowCasters in SceneUniforms.h
const int MAX_POINT_SHADOW_CASTERS = 2;   // must equal kMaxPointShadowCasters in SceneUniforms.h
const int SHADOW_CASCADES = 4;       // must equal kShadowCascades in SceneUniforms.h

/// Fraction of a cascade's far distance, at its end, over which it blends into
/// the next cascade. Must equal kCascadeBlend in SceneRenderer.cpp, which
/// makes each cascade cover that band as well.
const float CASCADE_BLEND = 0.1;

const float LIGHT_DIRECTIONAL = 0.0;
const float LIGHT_POINT       = 1.0;
const float LIGHT_SPOT        = 2.0;

/// One light. Mirrors GpuLight in SceneUniforms.h.
struct Light {
    vec4 positionRange;   ///< xyz: world position. w: range.
    vec4 colorType;       ///< rgb: colour * intensity. w: type (LIGHT_*).
    vec4 direction;       ///< xyz: direction the light travels. w: size of the light (radius in world units; for a directional light, the tangent of its angular radius).
    vec4 spotCone;        ///< x: cos of the spot's outer half-angle. y: cos of the inner one. z: shadow map slot, or -1. w: 1 if cascaded.
};

/// Scene-wide data, filled by the CPU once per frame (see SceneUniformData in
/// SceneUniforms.h; the two declarations must match). Set 0 is bound by
/// SceneRenderer before any draw call.
layout(set = 0, binding = 0, std140) uniform SceneData {
    vec4  ambientColor;                        ///< Light that reaches every surface (rgb).
    ivec4 lightCount;                          ///< x: number of lights in use.
    mat4  shadowViewProj[MAX_SHADOW_CASTERS];  ///< World space -> each shadow map's clip space.
    vec4  shadowParams[MAX_SHADOW_CASTERS];    ///< x: one texel in UV units. y: normal offset. z: 1 if y and w scale with distance. w: world width the map covers.
    vec4  pointShadowParams;                   ///< Shadow cubes. x: texel size per unit of distance. y: normal offset per unit of distance. z: near plane.
    Light lights[MAX_LIGHTS];
    mat4  cascadeViewProj[SHADOW_CASCADES];    ///< World space -> each cascade's clip space.
    vec4  cascadeParams[SHADOW_CASCADES];      ///< Same meaning as shadowParams (z is 0).
    vec4  cascadeSplits;                       ///< Distance from the camera at which each cascade ends.
    vec4  cameraForward;                       ///< xyz: view direction. w: dot(xyz, p) + w is p's depth in front of the camera.
} scene;

/// Depth of the scene as seen from each shadow-casting light. *Shadow*
/// samplers: texture() takes a reference depth and returns 1 where the stored
/// depth is not nearer than it (lit) and 0 where it is (shadowed).
layout(set = 0, binding = 1) uniform sampler2DShadow shadowMaps[MAX_SHADOW_CASTERS];

/// The shadow cubes of the point lights: six faces each, read as one cube
/// with a direction instead of a 2D position. Same comparison sampler idea.
layout(set = 0, binding = 2) uniform samplerCubeShadow shadowCubes[MAX_POINT_SHADOW_CASTERS];

/// The cascades of the sun's shadow: one array image, one layer per cascade.
layout(set = 0, binding = 3) uniform sampler2DArrayShadow cascadeMaps;

/// The same three images again, through plain samplers: texture() returns the
/// stored depth itself. The shadow filter needs it to find what blocks a light
/// and how far from the surface that is.
layout(set = 0, binding = 4) uniform sampler2D      shadowDepthMaps[MAX_SHADOW_CASTERS];
layout(set = 0, binding = 5) uniform samplerCube    shadowDepthCubes[MAX_POINT_SHADOW_CASTERS];
layout(set = 0, binding = 6) uniform sampler2DArray cascadeDepthMaps;

/// Near plane of a spot light's shadow projection. Must equal kSpotNear in SceneRenderer.cpp.
const float SPOT_NEAR = 0.1;

/// The widest blur a shadow edge can have, in world units. Keeps the filter
/// from spreading over most of the map when a blocker is very far from the
/// surface it shades.
const float PCSS_MAX_RADIUS = 1.5;

/// How many points look for blockers, and how many points filter the edge.
const int BLOCKER_SAMPLES = 16;
const int SHADOW_SAMPLES  = 32;

// All the lookups below use constant indices into sampler arrays on purpose:
// indexing a sampler array with a runtime value is an optional GPU feature.
// They also use textureLod / textureGrad with zero gradients rather than
// texture(): the shadow code runs under non-uniform conditions, where the
// implicit derivatives texture() needs are undefined.

/// Reads shadow cube `slot` through the comparison sampler. `coord.xyz` is the
/// direction from the light and `coord.w` the reference depth. (textureLod
/// does not exist for cube shadow samplers, hence textureGrad.)
float lookupCube(int slot, vec4 coord) {
    switch (slot) {
        case 0:  return textureGrad(shadowCubes[0], coord, vec3(0.0), vec3(0.0));
        default: return textureGrad(shadowCubes[1], coord, vec3(0.0), vec3(0.0));
    }
}

/// Reads one layer of the cascades. `coord` is (u, v, layer, reference depth).
float lookupCascade(vec4 coord) {
    return textureGrad(cascadeMaps, coord, vec2(0.0), vec2(0.0));
}

/// Reads shadow map `slot` through the comparison sampler.
float lookupShadow(int slot, vec3 coord) {
    switch (slot) {
        case 0:  return textureLod(shadowMaps[0], coord, 0.0);
        case 1:  return textureLod(shadowMaps[1], coord, 0.0);
        case 2:  return textureLod(shadowMaps[2], coord, 0.0);
        default: return textureLod(shadowMaps[3], coord, 0.0);
    }
}

/// The depth stored in shadow map `slot` at `uv`.
float rawMapDepth(int slot, vec2 uv) {
    switch (slot) {
        case 0:  return textureLod(shadowDepthMaps[0], uv, 0.0).r;
        case 1:  return textureLod(shadowDepthMaps[1], uv, 0.0).r;
        case 2:  return textureLod(shadowDepthMaps[2], uv, 0.0).r;
        default: return textureLod(shadowDepthMaps[3], uv, 0.0).r;
    }
}

/// The depth stored in cascade `layer` at `uv`.
float rawCascadeDepth(int layer, vec2 uv) {
    return textureLod(cascadeDepthMaps, vec3(uv, float(layer)), 0.0).r;
}

/// The depth stored in shadow cube `slot` in direction `dir` from the light.
float rawCubeDepth(int slot, vec3 dir) {
    switch (slot) {
        case 0:  return textureLod(shadowDepthCubes[0], dir, 0.0).r;
        default: return textureLod(shadowDepthCubes[1], dir, 0.0).r;
    }
}

/// A random number from 0 to 1 that is different at every screen pixel and
/// shows no pattern (a hash of the pixel's coordinates).
float pixelNoise() {
    vec3 p = fract(vec3(gl_FragCoord.xyx) * 0.1031);
    p += dot(p, p.yzx + 33.33);
    return fract((p.x + p.y) * p.z);
}

/// Point number `i` of `count` spread evenly inside a disc of radius 1, turned
/// by `angle`. The points follow a spiral that advances by the golden angle
/// each step, which spreads them far more evenly than a random choice. The
/// shadow filter reads the shadow map at these points, so the edge of a
/// shadow is an average over a disc.
///
/// `angle` is taken at random per pixel. With the same points everywhere, a
/// wide filter would show faint copies of the shadow's edge; turning the
/// points differently at each pixel turns those copies into fine grain, which
/// the eye reads as a smooth gradient.
vec2 sampleOnDisc(int i, int count, float angle) {
    float radius = sqrt((float(i) + 0.5) / float(count));
    float theta  = float(i) * 2.39996323 + angle;   // 2.39996 rad = the golden angle
    return radius * vec2(cos(theta), sin(theta));
}

/// Two unit vectors that span the plane perpendicular to `n`.
void planeAxes(vec3 n, out vec3 axis1, out vec3 axis2) {
    vec3 helper = (abs(n.y) < 0.9) ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
    axis1 = normalize(cross(n, helper));
    axis2 = cross(n, axis1);
}

/// Turns a depth stored in a shadow map back into a distance along the view
/// axis of the light, in world units. `persp` is true for a perspective
/// projection (spot light, cube face), false for an orthographic one.
float linearDepth(float z, float nearPlane, float farPlane, bool persp) {
    return persp ? (nearPlane * farPlane) / (farPlane - z * (farPlane - nearPlane))
                 : nearPlane + z * (farPlane - nearPlane);
}

/// How much of a shadow-casting light reaches a surface point: 1 fully lit,
/// 0 fully in shadow, in between at the soft edge of a shadow.
///
/// The edge is drawn with percentage-closer soft shadows (PCSS), in three steps:
///  1. Find the blockers: look around the point for anything that stands
///     between it and the light, and average how far from the light it is.
///  2. Work out how wide the edge is there. A light with some size is seen
///     from the surface as a disc, and a blocker hides part of that disc:
///     the farther the blocker is from the surface, compared with its
///     distance to the light, the wider the transition from light to dark.
///     For a point or spot light (`size` = its radius):
///         width = size * (receiver - blocker) / blocker
///     and for the sun (`size` = tangent of its angular radius):
///         width = size * (receiver - blocker)
///     with both distances measured from the light.
///  3. Average the shadow map over a disc of that width.
/// A surface touching its blocker gets a sharp edge; one far from it, a soft one.
///
/// Every sample is placed on the plane of the surface itself rather than in the
/// map's own plane, so that a surface seen at a grazing angle is never
/// compared with its own depth and cast a shadow on itself.
///
/// `lightDistance` is the distance to the light (used by spot lights).
/// `cascade` is -1 to read the flat map `slot`, or the index of a cascade of
/// the sun's shadow, which then takes the place of `slot`.
float shadowFactor(int slot, int cascade, vec3 worldPos, vec3 normal, vec3 toLight,
                   float lightDistance, float size) {
    bool  cascaded = cascade >= 0;
    vec4  params   = cascaded ? scene.cascadeParams[cascade] : scene.shadowParams[slot];
    bool  persp    = params.z > 0.5;
    float nearPlane = persp ? SPOT_NEAR : 0.0;
    float farPlane  = params.w;

    // For a spot light, texels grow with the distance to the light; for a
    // directional light they are constant.
    float scale = persp ? lightDistance : 1.0;
    float texel = params.x * scale;      // one texel, in world units

    // Normal offset: look the point up slightly above the surface. Surfaces
    // that are steep as seen from the light need more.
    float cosTheta = clamp(dot(normal, toLight), 0.0, 1.0);
    float sinTheta = sqrt(1.0 - cosTheta * cosTheta);
    vec3  base     = worldPos + normal * params.y * scale * (0.3 + sinTheta);

    // To the light's clip space. Clip x and y go from -1 to 1, texture
    // coordinates from 0 to 1; depth already is 0..1. A perspective projection
    // needs the division by w; points behind the light have no place in the map.
    mat4 viewProj = cascaded ? scene.cascadeViewProj[cascade] : scene.shadowViewProj[slot];
    vec4 clip = viewProj * vec4(base, 1.0);
    if (clip.w <= 0.0) return 1.0;
    vec3 ndc = clip.xyz / clip.w;

    // Beyond the light's depth range there is nothing to compare with.
    if (ndc.z < 0.0 || ndc.z > 1.0) return 1.0;

    float receiver = linearDepth(ndc.z, nearPlane, farPlane, persp);

    vec3  axis1, axis2;
    planeAxes(normal, axis1, axis2);
    float angle = 6.2831853 * pixelNoise();

    // 1. Blockers. The search covers the widest edge the light can produce
    //    (capped), around the point and in the plane of the surface.
    float width = texel;
    if (size > 0.0) {
        float searchRadius = persp ? min(3.0 * size, PCSS_MAX_RADIUS)
                                   : min(size * farPlane, PCSS_MAX_RADIUS);
        float bias       = 1.5 * texel;
        float blockerSum = 0.0;
        int   blockers   = 0;

        for (int i = 0; i < BLOCKER_SAMPLES; ++i) {
            vec2 offset = sampleOnDisc(i, BLOCKER_SAMPLES, angle) * searchRadius;
            vec4 c = viewProj * vec4(base + offset.x * axis1 + offset.y * axis2, 1.0);
            if (c.w <= 0.0) continue;
            vec3 n = c.xyz / c.w;
            if (n.z < 0.0 || n.z > 1.0) continue;

            float stored  = cascaded ? rawCascadeDepth(cascade, n.xy * 0.5 + 0.5)
                                     : rawMapDepth(slot, n.xy * 0.5 + 0.5);
            float blocker = linearDepth(stored, nearPlane, farPlane, persp);
            if (blocker < linearDepth(n.z, nearPlane, farPlane, persp) - bias) {
                blockerSum += blocker;
                ++blockers;
            }
        }

        // Nothing between the surface and the light: fully lit.
        if (blockers == 0) return 1.0;

        // 2. The width of the edge, never smaller than one texel (the least
        //    the map can resolve) and never wider than the cap.
        float blocker  = blockerSum / float(blockers);
        float penumbra = persp ? size * (receiver - blocker) / max(blocker, 1e-3)
                               : size * (receiver - blocker);
        width = clamp(penumbra, texel, PCSS_MAX_RADIUS);
    }

    // 3. Filter. Each lookup is already blended over four texels by the sampler.
    float lit = 0.0;
    for (int i = 0; i < SHADOW_SAMPLES; ++i) {
        vec2 offset = sampleOnDisc(i, SHADOW_SAMPLES, angle) * width;
        vec4 c = viewProj * vec4(base + offset.x * axis1 + offset.y * axis2, 1.0);
        if (c.w <= 0.0) { lit += 1.0; continue; }
        vec3 n = c.xyz / c.w;
        if (n.z < 0.0 || n.z > 1.0) { lit += 1.0; continue; }

        vec2 uv = n.xy * 0.5 + 0.5;
        lit += cascaded ? lookupCascade(vec4(uv, float(cascade), n.z))
                        : lookupShadow(slot, vec3(uv, n.z));
    }
    return lit / float(SHADOW_SAMPLES);
}

/// Like shadowFactor(), for the sun's cascaded shadow. The cascade is chosen
/// by how far the point is in front of the camera. Near the end of a cascade
/// the next one is blended in, so no seam shows where they meet, and past the
/// last cascade the shadow fades out to full light.
float cascadedShadowFactor(vec3 worldPos, vec3 normal, vec3 toLight, float size) {
    float depth  = dot(scene.cameraForward.xyz, worldPos) + scene.cameraForward.w;
    vec4  splits = scene.cascadeSplits;
    if (depth >= splits.w) return 1.0;

    int cascade = 0;
    if (depth > splits.x) cascade = 1;
    if (depth > splits.y) cascade = 2;
    if (depth > splits.z) cascade = 3;

    float end   = splits[cascade];
    float band  = CASCADE_BLEND * end;
    float blend = clamp((depth - (end - band)) / band, 0.0, 1.0);

    float lit = shadowFactor(0, cascade, worldPos, normal, toLight, 0.0, size);
    if (blend > 0.0) {
        float next = (cascade < SHADOW_CASCADES - 1)
                   ? shadowFactor(0, cascade + 1, worldPos, normal, toLight, 0.0, size)
                   : 1.0;
        lit = mix(lit, next, blend);
    }
    return lit;
}

/// Like shadowFactor(), for a point light and its shadow cube.
///
/// The face that holds a point is the one the direction from the light points
/// at most: the axis with the largest absolute value. The distance along that
/// axis is what the face's projection turned into depth, so the same
/// formula gives the reference depth to compare with:
///     depth = far * (d - near) / (d * (far - near))
/// and linearDepth() is its inverse.
float pointShadowFactor(int slot, vec3 worldPos, vec3 normal, vec3 toLight,
                        float lightDistance, vec3 lightPos, float range,
                        float size) {
    vec4 params = scene.pointShadowParams;

    float cosTheta = clamp(dot(normal, toLight), 0.0, 1.0);
    float sinTheta = sqrt(1.0 - cosTheta * cosTheta);
    vec3  base     = worldPos + normal * params.y * lightDistance * (0.3 + sinTheta);

    float nearPlane = params.z;
    float farPlane  = max(range, nearPlane + 0.1);

    vec3  dir0   = base - lightPos;
    vec3  a0     = abs(dir0);
    float major0 = max(a0.x, max(a0.y, a0.z));
    if (major0 <= nearPlane || major0 >= farPlane) return 1.0;

    float texel = params.x * major0;     // one texel on the face, in world units here

    vec3  axis1, axis2;
    planeAxes(normal, axis1, axis2);
    float angle = 6.2831853 * pixelNoise();

    // 1. Blockers, as in shadowFactor(), reading the cube's raw depths.
    float width = texel;
    if (size > 0.0) {
        float searchRadius = min(3.0 * size, PCSS_MAX_RADIUS);
        float bias       = 1.5 * texel;
        float blockerSum = 0.0;
        int   blockers   = 0;

        for (int i = 0; i < BLOCKER_SAMPLES; ++i) {
            vec2 offset = sampleOnDisc(i, BLOCKER_SAMPLES, angle) * searchRadius;
            vec3 dir    = dir0 + offset.x * axis1 + offset.y * axis2;
            vec3 ad     = abs(dir);
            float major = max(ad.x, max(ad.y, ad.z));
            if (major <= nearPlane || major >= farPlane) continue;

            float blocker = linearDepth(rawCubeDepth(slot, dir), nearPlane, farPlane, true);
            if (blocker < major - bias) {
                blockerSum += blocker;
                ++blockers;
            }
        }

        if (blockers == 0) return 1.0;

        // 2. The width of the edge.
        float blocker  = blockerSum / float(blockers);
        float penumbra = size * (major0 - blocker) / max(blocker, 1e-3);
        width = clamp(penumbra, texel, PCSS_MAX_RADIUS);
    }

    // 3. Filter, with the samples on the plane of the surface and a reference
    //    depth of their own, since each sits at its own distance from the light.
    float lit = 0.0;
    for (int i = 0; i < SHADOW_SAMPLES; ++i) {
        vec2 offset = sampleOnDisc(i, SHADOW_SAMPLES, angle) * width;
        vec3 dir    = dir0 + offset.x * axis1 + offset.y * axis2;
        vec3 ad     = abs(dir);
        float major = max(ad.x, max(ad.y, ad.z));
        if (major <= nearPlane || major >= farPlane) { lit += 1.0; continue; }

        float reference = farPlane * (major - nearPlane) / (major * (farPlane - nearPlane));
        lit += lookupCube(slot, vec4(dir, reference));
    }
    return lit / float(SHADOW_SAMPLES);
}

/// How much of a light reaches a point at the given distance: 1 close to the
/// light, falling with the square of the distance, and exactly 0 at `range`
/// so that a light never affects anything beyond the reach it declares.
float distanceFalloff(float distance, float range) {
    float window = clamp(1.0 - pow(distance / range, 4.0), 0.0, 1.0);
    return window * window / (distance * distance + 1.0);
}

/// Compresses any brightness, however large, into the 0..1 range a screen can
/// show, with a smooth "shoulder" instead of cutting everything above 1 to
/// white. This is the ACES filmic curve in the approximation by Krzysztof
/// Narkowicz: dark values are lifted a little and bright ones are rolled off,
/// so a surface lit by several strong lights keeps its shading.
vec3 toneMapACES(vec3 x) {
    return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0);
}

/// Converts a colour as shown on a screen (sRGB, what an artist picks) into
/// the linear amounts of light that lighting maths needs. Uses the simple
/// power-2.2 approximation of the exact sRGB curve.
vec3 srgbToLinear(vec3 c) {
    return pow(c, vec3(2.2));
}

/// Converts linear light into the encoding a screen expects. Without this
/// the display applies its own curve to values that never had one, and
/// every mid-tone comes out too dark. Inverse of srgbToLinear().
vec3 linearToSrgb(vec3 c) {
    return pow(c, vec3(1.0 / 2.2));
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
                                              light.positionRange.w, light.direction.w);
            else if (light.spotCone.w > 0.5)
                strength *= cascadedShadowFactor(fragWorldPos, normal, toLight, light.direction.w);
            else
                strength *= shadowFactor(shadowSlot, -1, fragWorldPos, normal, toLight,
                                         lightDistance, light.direction.w);
        }

        diffuseLight += light.colorType.rgb * facing * strength;
    }

    // The whole calculation runs in linear light: the surface colour comes in
    // as sRGB and is converted first, and only the final, tone-mapped result
    // goes back to sRGB for the screen.
    vec3 albedo = srgbToLinear(fragColor) * texture(albedoMap, fragUv).rgb;
    vec3 lit    = albedo * (scene.ambientColor.rgb + diffuseLight);
    outColor    = vec4(linearToSrgb(toneMapACES(lit)), 1.0);
}
