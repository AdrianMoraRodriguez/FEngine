#version 450

/// Depth-only vertex shader for cut-out materials: like shadow.vert, but it
/// also passes the texture coordinates on, so that shadow_mask.frag can look
/// at the albedo's alpha.
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec2 inUv;

/// One block per draw call.
layout(push_constant) uniform PushConstants {
    mat4 lightMvp;   ///< Light view-projection * object-to-world.
} pc;

layout(location = 0) out vec2 fragUv;

void main() {
    gl_Position = pc.lightMvp * vec4(inPosition, 1.0);
    fragUv      = inUv;
}
