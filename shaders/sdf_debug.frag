#version 450

/// A picture of the global distance field: what the software ray tracing
/// "sees". Each pixel sphere-traces the field from the camera: it advances
/// by the distance to the nearest surface, which can never skip over one.
/// Surfaces are shaded from the slope of the field, so objects whose field
/// is wrong (too coarse, too thin, missing) show up at once.
///
/// The field has three levels, each a cube around the camera: a small one
/// with fine cells, then two bigger ones with coarser cells. A point is read
/// from the finest level that contains it.

layout(location = 0) in vec2 ndc;

/// The levels, stacked along z (set 0 of this pipeline). World-space distances.
layout(set = 0, binding = 0) uniform sampler3D globalField;

layout(push_constant) uniform Push {
    mat4 inverseViewProj;   ///< Clip space -> world space.
    vec4 origin[3];         ///< Per level. xyz: world position of the corner. w: size of a cell.
    vec4 params;            ///< x: cells along each side of a level.
} pc;

layout(location = 0) out vec4 outColor;

const int LEVELS = 3;

/// Reads the field at a world point: x is the distance, y the size of a cell
/// of the level it came from. Beyond every level: far from everything.
vec2 fieldSample(vec3 p) {
    const float n = pc.params.x;
    for (int l = 0; l < LEVELS; ++l) {
        const float cell = pc.origin[l].w;
        const vec3  u    = (p - pc.origin[l].xyz) / (n * cell);

        // Four cells away from the edge: the filter never reaches the border
        // of the level, nor the neighbouring level.
        const float margin = 4.0 / n;
        if (all(greaterThan(u, vec3(margin))) && all(lessThan(u, vec3(1.0 - margin)))) {
            const vec3 uv = vec3(clamp(u.xy * n, vec2(0.5), vec2(n - 0.5)) / n,
                                 (float(l) * n + clamp(u.z * n, 0.5, n - 0.5)) / (n * float(LEVELS)));
            return vec2(textureLod(globalField, uv, 0.0).r, cell);
        }
    }
    return vec2(1.0e4, pc.origin[LEVELS - 1].w);
}

vec3 fieldNormal(vec3 p, float cell) {
    const float e = 0.5 * cell;
    return normalize(vec3(
        fieldSample(p + vec3(e, 0, 0)).x - fieldSample(p - vec3(e, 0, 0)).x,
        fieldSample(p + vec3(0, e, 0)).x - fieldSample(p - vec3(0, e, 0)).x,
        fieldSample(p + vec3(0, 0, e)).x - fieldSample(p - vec3(0, 0, e)).x));
}

void main() {
    vec4 nearH = pc.inverseViewProj * vec4(ndc, 0.0, 1.0);
    vec4 farH  = pc.inverseViewProj * vec4(ndc, 1.0, 1.0);
    vec3 origin = nearH.xyz / nearH.w;
    vec3 dir    = normalize(farH.xyz / farH.w - origin);

    vec3 sky = mix(vec3(0.05, 0.07, 0.10), vec3(0.16, 0.22, 0.32), clamp(dir.y * 0.5 + 0.5, 0.0, 1.0));

    // The farthest the biggest level reaches from the camera.
    const float reach = 0.5 * pc.params.x * pc.origin[LEVELS - 1].w;

    float t = 0.0;
    int steps = 0;
    bool hit = false;
    vec2 s = vec2(0.0);
    for (; steps < 192 && t < reach; ++steps) {
        s = fieldSample(origin + dir * t);
        if (s.x < 0.002 * t + 0.01 * s.y) { hit = true; break; }
        t += max(s.x, 0.05 * s.y);
    }

    if (!hit) { outColor = vec4(sky, 1.0); return; }

    vec3 p = origin + dir * t;
    vec3 n = fieldNormal(p, s.y);
    float light = clamp(dot(n, normalize(vec3(0.4, 0.8, 0.3))), 0.0, 1.0);
    float cheapAO = 1.0 - float(steps) / 192.0;   // many steps: a tight corner
    vec3 colour = vec3(0.75, 0.78, 0.85) * (0.18 + 0.82 * light) * (0.4 + 0.6 * cheapAO);
    outColor = vec4(colour, 1.0);
}
