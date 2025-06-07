#version 450

struct Light {
    vec4 direction;  // xyz = direction, w = intensity
    vec4 color;      // rgb = color, a = unused
};

layout(binding = 0) uniform UniformBufferObject {
    mat4 model;
    mat4 view;
    mat4 proj;
    vec4 viewPos;      // Camera position
    Light primaryLight; // Main directional light
    Light fillLight;    // Fill light
    Light rimLight;     // Rim light
    vec4 ambientColor;  // rgb = ambient color, a = intensity
} ubo;

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec3 inColor;

layout(location = 0) out vec3 fragColor;
layout(location = 1) out vec3 fragNormal;
layout(location = 2) out vec3 fragWorldPos;
layout(location = 3) out vec3 fragViewPos;
layout(location = 4) out Light fragPrimaryLight;
layout(location = 6) out Light fragFillLight;
layout(location = 8) out Light fragRimLight;
layout(location = 10) out vec3 fragAmbientColor;

void main() {
    // Transform position to world space
    vec4 worldPos = ubo.model * vec4(inPosition, 1.0);
    fragWorldPos = worldPos.xyz;
    
    // Final position
    gl_Position = ubo.proj * ubo.view * worldPos;
    
    // Transform normal to world space (use normal matrix for proper scaling)
    mat3 normalMatrix = transpose(inverse(mat3(ubo.model)));
    fragNormal = normalize(normalMatrix * inNormal);
    
    // Pass through color and camera position
    fragColor = inColor;
    fragViewPos = ubo.viewPos.xyz;
    
    // Pass through light data
    fragPrimaryLight = ubo.primaryLight;
    fragFillLight = ubo.fillLight;
    fragRimLight = ubo.rimLight;
    fragAmbientColor = ubo.ambientColor.rgb * ubo.ambientColor.a;
}