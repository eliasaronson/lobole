#version 450

layout(binding = 0) uniform UniformBufferObject {
    mat4 model;
    mat4 view;
    mat4 proj;
    vec4 lightDir;
    vec4 lightColor;
    vec4 viewPos; // Camera position
} ubo;

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec3 inColor;

layout(location = 0) out vec3 fragColor;
layout(location = 1) out vec3 fragNormal;
layout(location = 2) out vec3 fragLightDir;
layout(location = 3) out vec3 fragWorldPos;
layout(location = 4) out vec3 fragViewPos;

void main() {
    // Transform position to world space
    vec4 worldPos = ubo.model * vec4(inPosition, 1.0);
    fragWorldPos = worldPos.xyz;
    
    // Final position
    gl_Position = ubo.proj * ubo.view * worldPos;
    
    // Transform normal to world space (use normal matrix for proper scaling)
    mat3 normalMatrix = transpose(inverse(mat3(ubo.model)));
    fragNormal = normalize(normalMatrix * inNormal);
    
    // Pass through color, light direction, and camera position
    fragColor = inColor;
    fragLightDir = normalize(ubo.lightDir.xyz);
    fragViewPos = ubo.viewPos.xyz;
}