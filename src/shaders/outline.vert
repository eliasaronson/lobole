#version 450 core

layout(binding = 0) uniform UniformBufferObject {
    mat4 model;
    mat4 view;
    mat4 proj;
    vec4 viewPos;
    vec4 outlineColor;
    float outlineWidth;
} ubo;

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec3 inColor;

void main() {
    vec4 worldPos = ubo.model * vec4(inPosition, 1.0);
    vec4 viewPos = ubo.view * worldPos;
    
    // Expand vertex along normal in view space for outline
    vec3 viewNormal = normalize(mat3(ubo.view * ubo.model) * inNormal);
    viewPos.xyz += viewNormal * ubo.outlineWidth * 0.01;
    
    gl_Position = ubo.proj * viewPos;
}