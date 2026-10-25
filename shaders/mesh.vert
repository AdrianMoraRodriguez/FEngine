#version 450

/// Per-vertex input.
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec3 inColor;
layout(location = 3) in vec2 inUv;

/// Interpolated outputs to the fragment shader.
layout(location = 0) out vec3 fragNormal;     ///< Normal in world space.
layout(location = 1) out vec3 fragColor;
layout(location = 2) out vec3 fragWorldPos;   ///< Position in world space, for point and spot lights.
layout(location = 3) out vec2 fragUv;         ///< Texture coordinates (not sampled yet).

/// Push constants: per-object data, rewritten for every draw call.
/// Exactly 128 bytes, the smallest push constant space a GPU must offer.
layout(push_constant) uniform PushConstants {
    mat4 mvp;           ///< Model-View-Projection matrix.
    vec4 modelRows[3];  ///< First three rows of the object-to-world matrix (the last row of an affine matrix is always 0,0,0,1, so it is not sent).
    vec4 tint;          ///< rgb: material tint (sRGB), multiplies the vertex colour.
} pc;

void main() {
    mat4 model = transpose(mat4(pc.modelRows[0], pc.modelRows[1], pc.modelRows[2],
                                vec4(0.0, 0.0, 0.0, 1.0)));

    gl_Position = pc.mvp * vec4(inPosition, 1.0);

    // Normals are transformed by the inverse transpose of the model matrix.
    // With a plain model matrix they would stop being perpendicular to the
    // surface as soon as the object is scaled non-uniformly.
    fragNormal   = normalize(transpose(inverse(mat3(model))) * inNormal);
    fragColor    = inColor * pc.tint.rgb;
    fragUv       = inUv;
    fragWorldPos = (model * vec4(inPosition, 1.0)).xyz;
}
