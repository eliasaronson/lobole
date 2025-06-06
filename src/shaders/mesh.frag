#version 450

layout(location = 0) in vec3 fragColor;
layout(location = 1) in vec3 fragNormal;
layout(location = 2) in vec3 fragLightDir;
layout(location = 3) in vec3 fragWorldPos;
layout(location = 4) in vec3 fragViewPos;

layout(location = 0) out vec4 outColor;

void main() {
    // Normalize vectors
    vec3 normal = normalize(fragNormal);
    vec3 lightDir = normalize(fragLightDir);
    vec3 viewDir = normalize(fragViewPos - fragWorldPos);
    
    // Material properties for aluminum/metal
    vec3 albedo = fragColor;
    float metallic = 0.8;
    float roughness = 0.3;
    float ao = 1.0;
    
    // Light properties
    vec3 lightColor = vec3(1.0, 0.95, 0.8); // Slightly warm white
    float lightIntensity = 1.2;
    
    // Ambient lighting
    vec3 ambient = vec3(0.15, 0.17, 0.2) * albedo * ao; // Slight blue ambient
    
    // Diffuse lighting (Lambert)
    float NdotL = max(dot(normal, lightDir), 0.0);
    vec3 diffuse = lightColor * lightIntensity * NdotL * albedo;
    
    // Specular lighting (Blinn-Phong)
    vec3 halfwayDir = normalize(lightDir + viewDir);
    float NdotH = max(dot(normal, halfwayDir), 0.0);
    float shininess = mix(32.0, 128.0, 1.0 - roughness);
    float spec = pow(NdotH, shininess);
    vec3 specular = lightColor * spec * mix(vec3(0.04), albedo, metallic);
    
    // Fresnel effect for metallics
    float VdotH = max(dot(viewDir, halfwayDir), 0.0);
    vec3 F0 = mix(vec3(0.04), albedo, metallic);
    vec3 fresnel = F0 + (1.0 - F0) * pow(1.0 - VdotH, 5.0);
    
    // Combine lighting
    vec3 finalColor = ambient + (1.0 - metallic) * diffuse + fresnel * specular;
    
    // Tone mapping and gamma correction
    finalColor = finalColor / (finalColor + vec3(1.0));
    finalColor = pow(finalColor, vec3(1.0/2.2));
    
    outColor = vec4(finalColor, 1.0);
}