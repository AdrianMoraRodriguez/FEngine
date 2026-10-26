#version 450

layout(location = 0) in vec3 fragNormal;
layout(location = 1) in vec3 fragColor;
layout(location = 2) in vec3 fragWorldPos;
layout(location = 3) in vec2 fragUv;

/// The material (set 1, bound for every draw). Mirrors TextureLibrary.h.
/// A slot without a texture holds a 1x1 neutral one (white, or a flat
/// normal), so every lookup below is safe and multiplies by 1.

/// Colour. Stored as sRGB, so the GPU hands back linear values.
layout(set = 1, binding = 0) uniform sampler2D albedoMap;

/// Green: roughness. Blue: metalness (the glTF layout). Linear data.
layout(set = 1, binding = 1) uniform sampler2D metalRoughMap;

/// Tangent-space normal, stored as 0..1 (so 0.5 means 0). Linear data.
layout(set = 1, binding = 2) uniform sampler2D normalMap;

/// Light the surface gives off. Stored as sRGB.
layout(set = 1, binding = 3) uniform sampler2D emissiveMap;

/// The material's numbers, which multiply what the textures say.
layout(set = 1, binding = 4, std140) uniform MaterialData {
    vec4 params;     ///< x: metallic. y: roughness. z: normal map strength.
    vec4 emissive;   ///< rgb: emissive colour (linear).
    vec4 alpha;      ///< x: alpha mode (0 opaque, 1 mask, 2 blend). y: mask cutoff. z: opacity.
} material;

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
    vec4  cameraPosition;                      ///< w = 1: xyz is the camera's position. w = 0: xyz is the unit vector towards a camera infinitely far away (previews).
    vec4  environmentParams;                   ///< x: brightness of the environment light (0 = none).
} scene;

/// The environment (set 2): the sky as the objects see it. Mirrors Environment.h.
/// Mip 0 is the sky itself; each level after it is blurrier, filtered the way a
/// rougher surface would blur a reflection (level = roughness * last level).
layout(set = 2, binding = 0) uniform samplerCube environmentMap;

/// The light the sky gives to a surface of any orientation, as nine numbers
/// per colour channel (second-order spherical harmonics). Already convolved
/// with the diffuse lobe and scaled so that a uniformly bright sky of colour C
/// gives exactly C. Each entry is the coefficient of one term of
/// environmentDiffuse(), with the basis constants folded in.
layout(set = 2, binding = 1, std140) uniform EnvironmentData {
    vec4 sh[9];
} environment;

/// Ambient occlusion (set 3): 1 where the sky is fully visible, less in creases
/// and under objects. It has the size of the render target and is read at the
/// pixel's own position; a 1 x 1 white image stands in when there is none.
layout(set = 3, binding = 0) uniform sampler2D ambientOcclusionMap;

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
/// show. This is the "PBR Neutral" curve of the Khronos Group: colours below
/// the shoulder (about 0.76) pass through unchanged, and above it the
/// brightness is rolled off smoothly while the hue is kept. A filmic curve such
/// as ACES works on each channel separately and so pushes a saturated bright
/// colour towards white (a strong red light turns pink); this one does not,
/// which keeps coloured lights and coloured textures looking like themselves.
/// sky.frag has the same function: the sky and the objects must match.
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

const float PI = 3.14159265359;

/// The sharpest highlight allowed. At zero roughness the highlight would
/// shrink to a point that no pixel ever hits, and the maths divides by zero.
const float MIN_ROUGHNESS = 0.045;

/// Normal distribution (GGX / Trowbridge-Reitz): what fraction of the surface's
/// tiny facets face exactly along the half vector `h`. The rougher the surface,
/// the more spread out they are and the wider and dimmer the highlight.
float distributionGGX(float nDotH, float alpha) {
    float a2 = alpha * alpha;
    float d  = nDotH * nDotH * (a2 - 1.0) + 1.0;
    return a2 / (PI * d * d);
}

/// Visibility: how much of the facets are neither shadowed by their neighbours
/// (towards the light) nor hidden by them (towards the eye), divided by the
/// 4 * (n.l) * (n.v) of the Cook-Torrance formula. Smith's method with the
/// Schlick-GGX approximation, using the remapping meant for analytic lights.
float visibilitySmith(float nDotL, float nDotV, float roughness) {
    float k = (roughness + 1.0) * (roughness + 1.0) / 8.0;
    float gl = nDotL * (1.0 - k) + k;
    float gv = nDotV * (1.0 - k) + k;
    return 0.25 / (gl * gv);
}

/// Fresnel (Schlick): every surface turns mirror-like at grazing angles.
/// `f0` is the reflectance when looking straight at it.
vec3 fresnelSchlick(float cosTheta, vec3 f0) {
    return f0 + (1.0 - f0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

/// How much of the ambient light a surface reflects towards the eye: a fit of
/// the "split sum" environment BRDF (Karis, mobile approximation). It darkens
/// rough metals less than a plain `f0` would and brightens grazing angles.
vec3 ambientSpecularBRDF(vec3 f0, float roughness, float nDotV) {
    const vec4 c0 = vec4(-1.0, -0.0275, -0.572, 0.022);
    const vec4 c1 = vec4( 1.0,  0.0425,  1.04, -0.04);
    vec4  r  = roughness * c0 + c1;
    float a004 = min(r.x * r.x, exp2(-9.28 * nDotV)) * r.x + r.y;
    vec2  ab = vec2(-1.04, 1.04) * a004 + r.zw;
    return f0 * ab.x + ab.y;
}

/// Brightens the occlusion on light surfaces. Occlusion from the depth buffer
/// assumes the light that the blocked directions would have brought is lost,
/// but on a light surface that light bounces off the neighbours and comes back,
/// so creases are not as dark as the plain value says (Jimenez et al., "multi-
/// bounce" fit). Dark surfaces get the plain value.
float multiBounceOcclusion(float occlusion, vec3 albedo) {
    vec3 a = 2.0404 * albedo - 0.3324;
    vec3 b = -4.7951 * albedo + 0.6417;
    vec3 c = 2.7552 * albedo + 0.6903;
    // One number from the three colour channels: the luminance of the result.
    vec3 bounced = max(vec3(occlusion), ((occlusion * a + b) * occlusion + c) * occlusion);
    return dot(bounced, vec3(0.2126, 0.7152, 0.0722));
}

/// How much the occlusion dims the reflection of the sky. A mirror-like
/// surface only sees along one direction, so a crease dims it much less than
/// it dims diffuse light (Lagarde, Frostbite): rough surfaces approach the
/// plain occlusion, smooth ones are barely affected unless seen at a grazing angle.
float specularOcclusion(float nDotV, float occlusion, float roughness) {
    return clamp(pow(nDotV + occlusion, exp2(-16.0 * roughness - 1.0)) - 1.0 + occlusion, 0.0, 1.0);
}

/// Bends the surface normal `n` by a normal map sample `tangentNormal`
/// (already in -1..1). The tangent frame is not stored in the vertices: it is
/// worked out from how the position and the texture coordinates change from
/// one pixel to the next. Returns `n` unchanged where that is not possible
/// (no texture coordinates, or a degenerate triangle).
vec3 perturbNormal(vec3 n, vec3 worldPos, vec2 uv, vec3 tangentNormal) {
    vec3 dpdx = dFdx(worldPos);
    vec3 dpdy = dFdy(worldPos);
    vec2 duvdx = dFdx(uv);
    vec2 duvdy = dFdy(uv);

    float det = duvdx.x * duvdy.y - duvdy.x * duvdx.y;
    if (abs(det) < 1e-12) return n;

    vec3 t = (duvdy.y * dpdx - duvdx.y * dpdy) / det;
    t = t - n * dot(n, t);                 // make it perpendicular to the normal
    float len = length(t);
    if (len < 1e-8) return n;
    t /= len;
    vec3 b = cross(n, t);

    return normalize(mat3(t, b, n) * tangentNormal);
}

/// The light that reaches a surface facing `n` from the whole sky, already
/// divided by pi so it multiplies the diffuse colour directly.
vec3 environmentDiffuse(vec3 n) {
    vec3 c = environment.sh[0].rgb
           + environment.sh[1].rgb * n.y
           + environment.sh[2].rgb * n.z
           + environment.sh[3].rgb * n.x
           + environment.sh[4].rgb * (n.x * n.y)
           + environment.sh[5].rgb * (n.y * n.z)
           + environment.sh[6].rgb * (3.0 * n.z * n.z - 1.0)
           + environment.sh[7].rgb * (n.x * n.z)
           + environment.sh[8].rgb * (n.x * n.x - n.y * n.y);
    return max(c, vec3(0.0));   // the nine terms can ring slightly below zero next to a bright sun
}

/// What a surface of the given roughness reflects of the sky in direction `r`.
vec3 environmentSpecular(vec3 r, float roughness) {
    float lastLevel = float(textureQueryLevels(environmentMap) - 1);
    return textureLod(environmentMap, r, roughness * lastLevel).rgb;
}

void main() {
    // The surface as geometry says (used for shadows) and as the normal map
    // says (used for shading).
    vec3 geometricNormal = normalize(fragNormal);
    // A double-sided surface seen from behind: the normal must face the viewer.
    if (!gl_FrontFacing) geometricNormal = -geometricNormal;

    vec3 tangentNormal = texture(normalMap, fragUv).xyz * 2.0 - 1.0;
    tangentNormal.xy *= material.params.z;
    vec3 normal = perturbNormal(geometricNormal, fragWorldPos, fragUv, normalize(tangentNormal));

    // The material. The colour comes in as sRGB; everything below is linear light.
    vec4  albedoSample = texture(albedoMap, fragUv);

    // Transparency. Opaque ignores the alpha channel; a cut-out drops the pixels
    // that are too transparent and keeps the rest solid; a translucent surface
    // keeps its alpha and is mixed with the background by the blend state.
    //
    // The cut-out edge is anti-aliased with alpha to coverage: the pipeline
    // turns the alpha written here into the share of the pixel's samples that
    // are drawn. The alpha is first sharpened to the width of one pixel around
    // the cutoff, so the edge is a smooth one-pixel ramp and not a blur.
    float opacity  = 1.0;   // how much a translucent surface hides what is behind it
    float coverage = 1.0;   // the alpha handed to alpha to coverage
    if (material.alpha.x > 0.5) {
        float alphaValue = albedoSample.a * material.alpha.z;
        if (material.alpha.x < 1.5) {
            coverage = clamp((alphaValue - material.alpha.y) / max(fwidth(alphaValue), 1e-4) + 0.5, 0.0, 1.0);
            if (coverage <= 0.0) discard;
        } else {
            opacity  = alphaValue;
            coverage = alphaValue;
        }
    }
    vec3  albedo       = srgbToLinear(fragColor) * albedoSample.rgb;
    vec3  mr           = texture(metalRoughMap, fragUv).rgb;
    float metallic     = clamp(material.params.x * mr.b, 0.0, 1.0);
    float roughness    = clamp(material.params.y * mr.g, MIN_ROUGHNESS, 1.0);
    float alpha        = roughness * roughness;

    // A dielectric reflects about 4% of the light as a white highlight; a
    // metal reflects its own colour and has no diffuse light at all.
    vec3 f0          = mix(vec3(0.04), albedo, metallic);
    vec3 diffuseColor = albedo * (1.0 - metallic);

    // Direction towards the eye.
    vec3 viewDir = (scene.cameraPosition.w > 0.5)
                 ? normalize(scene.cameraPosition.xyz - fragWorldPos)
                 : scene.cameraPosition.xyz;
    float nDotV = max(dot(normal, viewDir), 1e-4);

    vec3 radiance     = vec3(0.0);   // light that the surface scatters (diffuse) and emits
    vec3 radianceSpec = vec3(0.0);   // mirror-like highlights and reflections: a see-through surface keeps them

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

        float nDotL = max(dot(normal, toLight), 0.0);

        // A light with a shadow map checks it. Surfaces turned away from the
        // light, or outside its cone, are dark already, so the lookup is
        // skipped. The shadow uses the geometric normal: the normal map is
        // detail finer than the shadow map can show.
        int shadowSlot = int(light.spotCone.z);
        if (shadowSlot >= 0 && nDotL > 0.0 && strength > 0.0) {
            if (type == LIGHT_POINT)
                strength *= pointShadowFactor(shadowSlot, fragWorldPos, geometricNormal, toLight,
                                              lightDistance, light.positionRange.xyz,
                                              light.positionRange.w, light.direction.w);
            else if (light.spotCone.w > 0.5)
                strength *= cascadedShadowFactor(fragWorldPos, geometricNormal, toLight, light.direction.w);
            else
                strength *= shadowFactor(shadowSlot, -1, fragWorldPos, geometricNormal, toLight,
                                         lightDistance, light.direction.w);
        }

        if (nDotL <= 0.0 || strength <= 0.0) continue;

        // Cook-Torrance: a Lambert diffuse plus a microfacet specular lobe.
        // The diffuse part gets what the specular part did not reflect.
        vec3  halfDir = normalize(toLight + viewDir);
        float nDotH   = max(dot(normal, halfDir), 0.0);
        float vDotH   = max(dot(viewDir, halfDir), 0.0);

        vec3 fresnel  = fresnelSchlick(vDotH, f0);
        vec3 specular = distributionGGX(nDotH, alpha) * visibilitySmith(nDotL, nDotV, roughness) * fresnel;
        vec3 diffuse  = (1.0 - fresnel) * diffuseColor;

        // The light colours of the editor are not divided by pi, as radiometry
        // would have them, so the pi that Lambert's law divides by is left out
        // of the diffuse term and put back into the specular one. A white
        // diffuse surface under a light of intensity 1 is just as bright as before.
        radiance     += diffuse * light.colorType.rgb * nDotL * strength;
        radianceSpec += PI * specular * light.colorType.rgb * nDotL * strength;
    }

    // Light that comes from everywhere: the flat ambient colour of the scene
    // plus the sky. The sky gives each direction its own colour and brightness:
    // the diffuse part is the whole sky seen from the surface (a face turned
    // up is lit by the blue above, one turned down by the ground), and the
    // specular part is the sky mirrored in the surface, blurrier the rougher
    // it is. Both are scaled by the same brightness setting.
    //
    // The reflection direction is bent towards the normal for rough surfaces:
    // the bright part of a wide highlight lies between the two (Frostbite's
    // "dominant direction").
    float skyStrength = scene.environmentParams.x;
    vec3  reflected   = reflect(-viewDir, normal);
    reflected = normalize(mix(reflected, normal, alpha));

    vec3 diffuseLight  = scene.ambientColor.rgb + skyStrength * environmentDiffuse(normal);
    vec3 specularLight = scene.ambientColor.rgb + skyStrength * environmentSpecular(reflected, roughness);

    // Where nearby geometry blocks the sky, less of this light arrives. Direct
    // lights are not touched: they have their shadow maps for that.
    float occlusion = texture(ambientOcclusionMap, gl_FragCoord.xy / vec2(textureSize(ambientOcclusionMap, 0))).r;
    float diffuseOcclusion  = multiBounceOcclusion(occlusion, diffuseColor);
    float specularOcc       = specularOcclusion(nDotV, occlusion, roughness);

    radiance += diffuseColor * diffuseLight * diffuseOcclusion;
    radianceSpec += ambientSpecularBRDF(f0, roughness, nDotV) * specularLight * specularOcc;

    // Light the surface gives off itself.
    radiance += texture(emissiveMap, fragUv).rgb * material.emissive.rgb;

    // The colour leaves multiplied by its own alpha: the blend state of a
    // translucent surface adds it to the background dimmed by (1 - alpha).
    // Highlights are added whole, so glass still glints. For anything solid
    // opacity is 1 and this is just the sum.
    outColor = vec4(linearToSrgb(toneMapNeutral(radiance * opacity + radianceSpec)), coverage);
}
