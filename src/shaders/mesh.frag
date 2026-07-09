#version 450

struct Light {
    vec4 direction;  // xyz = direction, w = intensity
    vec4 color;      // rgb = color, a = unused
};

layout(location = 0) in vec3 fragColor;
layout(location = 1) in vec3 fragNormal;
layout(location = 2) in vec3 fragWorldPos;
layout(location = 3) in vec3 fragViewPos;
layout(location = 4) in Light fragPrimaryLight;
layout(location = 6) in Light fragFillLight;
layout(location = 8) in Light fragRimLight;
layout(location = 10) in vec3 fragAmbientColor;
layout(location = 11) in float fragHighlight;

layout(location = 0) out vec4 outColor;

layout(push_constant) uniform PushConstants {
    mat4 model;        // used in the vertex stage
    uvec4 highlight;   // used in the vertex stage
    vec4 material;      // x=metallic, y=roughness, z=specular, w=reflectance
} pc;

// ACES filmic tone mapping (Narkowicz fit): maps HDR [0,inf) into [0,1] with a
// filmic shoulder so highlights roll off instead of clipping to flat white.
vec3 ACESFilm(vec3 x) {
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

// Enhanced PBR lighting calculation
vec3 calculateLight(Light light, vec3 normal, vec3 viewDir, vec3 albedo, float metallic, float roughness, float specularScale, float reflectance) {
    vec3 lightDir = normalize(light.direction.xyz);
    float lightIntensity = light.direction.w;
    vec3 lightColor = light.color.rgb;
    
    // Diffuse
    float NdotL = max(dot(normal, lightDir), 0.0);
    vec3 diffuse = lightColor * lightIntensity * NdotL * albedo;
    
    // Specular (Blinn-Phong)
    vec3 halfwayDir = normalize(lightDir + viewDir);
    float NdotH = max(dot(normal, halfwayDir), 0.0);
    float shininess = mix(16.0, 256.0, 1.0 - roughness);
    float spec = pow(NdotH, shininess);
    
    // Fresnel effect (dielectric F0 from the material's reflectance)
    float VdotH = max(dot(viewDir, halfwayDir), 0.0);
    vec3 F0 = mix(vec3(reflectance), albedo, metallic);
    vec3 fresnel = F0 + (1.0 - F0) * pow(1.0 - VdotH, 5.0);

    vec3 specular = lightColor * lightIntensity * spec * fresnel * specularScale;
    
    // Combine diffuse and specular based on metallicness
    return mix(diffuse, vec3(0.0), metallic) + specular;
}

void main() {
    // Normalize vectors
    vec3 normal = normalize(fragNormal);
    vec3 viewDir = normalize(fragViewPos - fragWorldPos);
    
    // Albedo from the vertex color (set from the body's material base color).
    vec3 albedo = fragColor;

    // Real PBR params from the assigned material (no more color guessing).
    float metallic = pc.material.x;
    float roughness = clamp(pc.material.y, 0.03, 1.0);
    float specularScale = pc.material.z;
    float reflectance = pc.material.w;

    // Ambient lighting
    vec3 ambient = fragAmbientColor * albedo;

    // Calculate lighting from all three light sources
    vec3 primaryLighting = calculateLight(fragPrimaryLight, normal, viewDir, albedo, metallic, roughness, specularScale, reflectance);
    vec3 fillLighting = calculateLight(fragFillLight, normal, viewDir, albedo, metallic, roughness, specularScale, reflectance);
    vec3 rimLighting = calculateLight(fragRimLight, normal, viewDir, albedo, metallic, roughness, specularScale, reflectance);
    
    // Combine all lighting
    vec3 finalColor = ambient + primaryLighting + fillLighting + rimLighting;
    
    // ACES filmic tone mapping, then gamma to sRGB.
    finalColor = ACESFilm(finalColor);
    finalColor = pow(finalColor, vec3(1.0/2.2));
    
    // Slight brightness boost for better visibility
    finalColor *= 1.1;

    // Face highlight tint: selected (1.0) strong, hovered (0.5) subtle
    const vec3 highlightColor = vec3(1.0, 0.65, 0.15);
    finalColor = mix(finalColor, highlightColor, fragHighlight * 0.55);

    outColor = vec4(finalColor, 1.0);
}