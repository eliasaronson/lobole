#version 460
#extension GL_EXT_ray_tracing : require

layout(location = 0) rayPayloadInEXT vec3 hitValue;

layout(binding = 2, set = 0) uniform UniformBufferObject {
    mat4 viewInverse;
    mat4 projInverse;
    vec4 viewPos;
    vec4 lightDir;
    vec4 lightColor;
    vec4 ambientColor;
} ubo;

void main() {
    // Sky gradient - professional CAD environment
    vec3 unitDirection = normalize(gl_WorldRayDirectionEXT);
    float t = 0.5 * (unitDirection.y + 1.0);
    
    // Professional gray gradient background
    vec3 skyColor = mix(vec3(0.9, 0.9, 0.92), vec3(0.7, 0.75, 0.8), t);
    
    hitValue = skyColor * ubo.ambientColor.rgb;
}