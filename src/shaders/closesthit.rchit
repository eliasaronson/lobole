#version 460
#extension GL_EXT_ray_tracing : require
#extension GL_EXT_nonuniform_qualifier : enable

layout(location = 0) rayPayloadInEXT vec3 hitValue;
layout(location = 1) rayPayloadEXT bool isShadowed;

hitAttributeEXT vec3 attribs;

layout(binding = 0, set = 0) uniform accelerationStructureEXT topLevelAS;
layout(binding = 3, set = 0) buffer readonly VertexBuffer { float vertices[]; };
layout(binding = 4, set = 0) buffer readonly IndexBuffer { uint indices[]; };

layout(binding = 2, set = 0) uniform UniformBufferObject {
    mat4 viewInverse;
    mat4 projInverse;
    vec4 viewPos;
    vec4 lightDir;
    vec4 lightColor;
    vec4 ambientColor;
} ubo;

struct Vertex {
    vec3 pos;
    vec3 normal;
    vec3 color;
};

struct Material {
    vec3 baseColor;
    float metallic;
    vec3 emissive;
    float roughness;
    vec3 specular;
    float transmission;
};

// Hardcoded materials for now (will integrate with material system later)
Material getMaterial(uint materialIndex) {
    Material mat;
    if (materialIndex == 0) { // Aluminum
        mat.baseColor = vec3(0.91, 0.92, 0.92);
        mat.metallic = 0.8;
        mat.roughness = 0.1;
        mat.specular = vec3(0.95);
    } else { // Default
        mat.baseColor = vec3(0.8, 0.8, 0.8);
        mat.metallic = 0.2;
        mat.roughness = 0.5;
        mat.specular = vec3(0.5);
    }
    mat.emissive = vec3(0.0);
    mat.transmission = 0.0;
    return mat;
}

Vertex unpackVertex(uint index) {
    uint vertexIndex = indices[index];
    uint offset = vertexIndex * 9; // 3 pos + 3 normal + 3 color
    
    Vertex v;
    v.pos = vec3(vertices[offset + 0], vertices[offset + 1], vertices[offset + 2]);
    v.normal = vec3(vertices[offset + 3], vertices[offset + 4], vertices[offset + 5]);
    v.color = vec3(vertices[offset + 6], vertices[offset + 7], vertices[offset + 8]);
    return v;
}

vec3 calculateLighting(vec3 position, vec3 normal, vec3 viewDir, Material mat) {
    // Lighting calculation similar to our PBR fragment shader
    vec3 lightDir = normalize(-ubo.lightDir.xyz);
    vec3 lightColor = ubo.lightColor.rgb * ubo.lightColor.w;
    
    // Diffuse
    float NdotL = max(dot(normal, lightDir), 0.0);
    vec3 diffuse = mat.baseColor * lightColor * NdotL;
    
    // Specular (simplified Blinn-Phong)
    vec3 halfwayDir = normalize(lightDir + viewDir);
    float NdotH = max(dot(normal, halfwayDir), 0.0);
    float spec = pow(NdotH, (1.0 - mat.roughness) * 128.0);
    vec3 specular = mat.specular * spec * lightColor;
    
    // Fresnel effect for metals
    float VdotH = max(dot(viewDir, halfwayDir), 0.0);
    vec3 F0 = mix(vec3(0.04), mat.baseColor, mat.metallic);
    vec3 fresnel = F0 + (1.0 - F0) * pow(1.0 - VdotH, 5.0);
    
    vec3 finalDiffuse = mix(diffuse, vec3(0.0), mat.metallic);
    vec3 finalSpecular = mix(specular, specular * fresnel, mat.metallic);
    
    return finalDiffuse + finalSpecular + ubo.ambientColor.rgb * mat.baseColor;
}

void main() {
    // Get triangle vertices
    uint primitiveID = gl_PrimitiveID;
    uint i0 = primitiveID * 3 + 0;
    uint i1 = primitiveID * 3 + 1;
    uint i2 = primitiveID * 3 + 2;
    
    Vertex v0 = unpackVertex(i0);
    Vertex v1 = unpackVertex(i1);
    Vertex v2 = unpackVertex(i2);
    
    // Barycentric interpolation
    vec3 barycentricCoords = vec3(1.0 - attribs.x - attribs.y, attribs.x, attribs.y);
    
    vec3 position = v0.pos * barycentricCoords.x + v1.pos * barycentricCoords.y + v2.pos * barycentricCoords.z;
    vec3 normal = normalize(v0.normal * barycentricCoords.x + v1.normal * barycentricCoords.y + v2.normal * barycentricCoords.z);
    vec3 color = v0.color * barycentricCoords.x + v1.color * barycentricCoords.y + v2.color * barycentricCoords.z;
    
    // Transform to world space
    position = gl_ObjectToWorldEXT * vec4(position, 1.0);
    normal = normalize(mat3(gl_ObjectToWorldEXT) * normal);
    
    vec3 viewDir = normalize(ubo.viewPos.xyz - position);
    
    // Get material (use instance ID or hardcode for now)
    Material mat = getMaterial(gl_InstanceID);
    
    // Apply vertex color tint
    mat.baseColor *= color;
    
    // Calculate lighting
    vec3 finalColor = calculateLighting(position, normal, viewDir, mat);
    
    // Shadow ray (optional enhancement)
    vec3 lightDir = normalize(-ubo.lightDir.xyz);
    isShadowed = false;
    
    uint shadowFlags = gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsOpaqueEXT | gl_RayFlagsSkipClosestHitShaderEXT;
    traceRayEXT(topLevelAS, shadowFlags, 0xff, 1, 0, 1, position + normal * 0.001, 0.001, lightDir, 1000.0, 1);
    
    if (isShadowed) {
        finalColor *= 0.3; // Darken shadows
    }
    
    hitValue = finalColor;
}