#version 450

/// Only the position matters when drawing depth.
layout(location = 0) in vec3 inPosition;

/// One block per draw call.
layout(push_constant) uniform PushConstants {
    mat4 lightMvp;   ///< Light view-projection * object-to-world.
} pc;

void main() {
    gl_Position = pc.lightMvp * vec4(inPosition, 1.0);
}
