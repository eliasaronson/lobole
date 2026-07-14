#version 450

layout(binding = 0) uniform UniformBufferObject {
    mat4 view;
    mat4 proj;
    vec4 clipPlane; // xyz = normal, w = dist; 0 = off
} ubo;

layout(push_constant) uniform Push {
    float deformScale; // displacement warp multiplier
} pc;

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inDisplacement;
layout(location = 2) in float inScalar;

layout(location = 0) out float fragScalar;
layout(location = 1) out float fragClipDist;

void main() {
    vec3 world = inPosition + pc.deformScale * inDisplacement;
    gl_Position = ubo.proj * ubo.view * vec4(world, 1.0);
    fragScalar = inScalar;
    // Section clip (shared with the mesh pipeline): fragments past the plane
    // are discarded in the fragment shader.
    fragClipDist = dot(ubo.clipPlane.xyz, world) - ubo.clipPlane.w;
}
