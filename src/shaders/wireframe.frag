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
    vec4 clipPlane;
} ubo;

layout(location = 0) in vec3 fragWorldPos;
layout(location = 0) out vec4 fragColor;

void main() {
    if (dot(ubo.clipPlane.xyz, ubo.clipPlane.xyz) > 0.0 &&
        dot(fragWorldPos, ubo.clipPlane.xyz) > ubo.clipPlane.w) {
        discard;
    }
    fragColor = ubo.wireframeColor;
}