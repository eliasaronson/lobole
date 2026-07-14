#version 450 core

layout(binding = 0) uniform UniformBufferObject {
    mat4 model;
    mat4 view;
    mat4 proj;
    vec4 viewPos;
    vec4 outlineColor;
    float outlineWidth;
} ubo;

// Per-mesh model matrix (A2.5) — see wireframe.vert. UBO `model` kept as legacy
// padding to preserve std140 offsets; the real model is the push constant.
layout(push_constant) uniform PushConstants {
    mat4 model;
} pc;

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;

void main() {
    vec4 worldPos = pc.model * vec4(inPosition, 1.0);
    vec4 viewPos = ubo.view * worldPos;

    // Expand vertex along normal in view space for outline
    vec3 viewNormal = normalize(mat3(ubo.view * pc.model) * inNormal);
    viewPos.xyz += viewNormal * ubo.outlineWidth * 0.1;

    gl_Position = ubo.proj * viewPos;
}