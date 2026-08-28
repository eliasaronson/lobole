#version 460
#extension GL_EXT_ray_tracing : require

struct RayPayload {
    vec3 radiance;
    uint rngState;
};
layout(location = 0) rayPayloadInEXT RayPayload payload;

layout(binding = 2, set = 0) uniform UniformBufferObject {
    mat4 viewInverse;
    mat4 projInverse;
    vec4 viewPos;
    vec4 lightDir;
    vec4 lightColor;
    vec4 ambientColor;
    vec4 params;      // x = prefilter mip count
} ubo;

// Full-resolution source HDRI (the prefilter chain's base mip is only 128x64,
// which upscales to a mushy background).
layout(binding = 8, set = 0) uniform sampler2D uEnvironment;

const float PI = 3.14159265359;
// Must match IBL.cpp DirToEquirectUV exactly (including the world -> env
// swizzle: the HDRI is Y-polar, the world Z-up).
vec2 dirToUV(vec3 d) {
    d = normalize(d);
    d = vec3(d.x, d.z, -d.y); // world -> env
    float u = atan(d.z, d.x) / (2.0 * PI) + 0.5;
    float v = acos(clamp(d.y, -1.0, 1.0)) / PI;
    return vec2(u, v);
}

void main() {
    vec3 dir = normalize(gl_WorldRayDirectionEXT);
    payload.radiance = textureLod(uEnvironment, dirToUV(dir), 0.0).rgb;
}
