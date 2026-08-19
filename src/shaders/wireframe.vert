#version 450 core

layout(binding = 0) uniform UniformBufferObject {
    mat4 model;
    mat4 view;
    mat4 proj;
    vec4 viewPos;
    vec4 wireframeColor;
    float lineWidth;
    // std140 auto-pads lineWidth's 16-byte slot; clipPlane lands at the next
    // vec4 boundary, matching the C++ struct's explicit _padding[3].
    vec4 clipPlanes[4]; // section clips (world); all-zero slot = off
} ubo;

// Per-mesh model matrix (A2.5): the UBO's `model` is left as legacy padding so
// std140 offsets (and the clipPlane alignment) don't shift; the real per-mesh
// model comes from the push constant, so an occurrence drag is a matrix update,
// not a vertex re-upload.
layout(push_constant) uniform PushConstants {
    mat4 model;
} pc;

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;

layout(location = 0) out vec3 fragWorldPos;

void main() {
    vec4 worldPos = pc.model * vec4(inPosition, 1.0);
    fragWorldPos = worldPos.xyz;
    gl_Position = ubo.proj * ubo.view * worldPos;
}