#version 450

layout(location = 0) in vec3 fragColor;
layout(location = 1) in vec3 fragNormal;
layout(location = 2) in vec3 fragLightDir;

layout(location = 0) out vec4 outColor;

void main() {
    // Simple diffuse lighting
    float diffuse = max(dot(fragNormal, fragLightDir), 0.0);
    
    // Ambient + diffuse lighting
    vec3 ambient = 0.3 * fragColor;
    vec3 diffuseColor = diffuse * fragColor;
    
    vec3 finalColor = ambient + diffuseColor;
    
    outColor = vec4(finalColor, 1.0);
}