#version 450 core

layout(binding = 0) uniform UniformBufferObject {
    mat4 model;
    mat4 view;
    mat4 proj;
    vec4 viewPos;
    vec4 wireframeColor;
    float lineWidth;
} ubo;

layout(location = 0) out vec4 fragColor;

void main() {
    fragColor = ubo.wireframeColor;
}