#version 450

// Depth-only shadow-map variant of mesh.vert (kept in lockstep with its
// instance-transform + section-clip conventions). The instance model matrix
// is premultiplied into lightMVP and the world-space section plane is
// pre-transformed into object space on the CPU, so per-draw state fits in 80
// bytes of push constants and no UBO is needed.
layout(push_constant) uniform PushConstants {
    mat4 lightMVP;      // lightViewProj * instance model
    vec4 objClipPlane;  // section plane in object space; 0 = off
} pc;

layout(location = 0) in vec3 inPosition;

layout(location = 0) out float fragClipDist;

void main() {
    gl_Position = pc.lightMVP * vec4(inPosition, 1.0);
    // Signed distance past the section plane (same sign convention as
    // mesh.vert); the fragment stage discards where it is > 0 so
    // sectioned-away material does not cast.
    fragClipDist = dot(pc.objClipPlane, vec4(inPosition, 1.0));
}
