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
    vec4 clipPlane; // section clip (world): xyz=normal, w=dist
} ubo;

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec3 inColor;

layout(location = 0) out vec3 fragWorldPos;

void main() {
    vec4 worldPos = ubo.model * vec4(inPosition, 1.0);
    fragWorldPos = worldPos.xyz;
    gl_Position = ubo.proj * ubo.view * worldPos;
}