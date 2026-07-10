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

// Sharpest prefiltered environment mip == the studio environment itself, so the
// background and mirror reflections show the real HDRI.
layout(binding = 6, set = 0) uniform sampler2D uPrefilter;

const float PI = 3.14159265359;
vec2 dirToUV(vec3 d) {
    d = normalize(d);
    float u = atan(d.z, d.x) / (2.0 * PI) + 0.5;
    float v = acos(clamp(d.y, -1.0, 1.0)) / PI;
    return vec2(u, v);
}

void main() {
    vec3 dir = normalize(gl_WorldRayDirectionEXT);
    payload.radiance = textureLod(uPrefilter, dirToUV(dir), 0.0).rgb;
}
