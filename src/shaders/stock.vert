#version 450

struct Light {
    vec4 direction; // xyz = toward the light, w = intensity
    vec4 color;     // rgb
};

layout(binding = 0) uniform UniformBufferObject {
    mat4 view;
    mat4 proj;
    vec4 clipPlanes[4]; // section clips (world); an all-zero slot is off
    // The SAME three lights the part is lit by (core/Conventions.h). This
    // shader used to carry its own directions, so the block in the simulation
    // was lit from somewhere the model beside it was not.
    Light key;
    Light fill;
    Light rim;
    vec4 ambient;   // rgb = colour, a = intensity
} ubo;

layout(push_constant) uniform Push {
    mat4 model;    // setup space -> world (an indexed setup leans the block)
    vec4 baseColor;
} pc;

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;

layout(location = 0) out vec3 fragNormal;
layout(location = 1) out vec3 fragWorld;
layout(location = 2) out vec4 fragClipDist;
layout(location = 3) out Light fragKey;
layout(location = 5) out Light fragFill;
layout(location = 7) out Light fragRim;
layout(location = 9) out vec4 fragAmbient;

void main() {
    vec4 world = pc.model * vec4(inPosition, 1.0);
    gl_Position = ubo.proj * ubo.view * world;
    // The model matrix is a rotation about X plus a translation, so it needs
    // no inverse-transpose — but writing mat3(model) keeps that assumption
    // visible rather than assumed.
    fragNormal = mat3(pc.model) * inNormal;
    fragWorld = world.xyz;
    for (int i = 0; i < 4; i++) {
        fragClipDist[i] =
            dot(ubo.clipPlanes[i].xyz, ubo.clipPlanes[i].xyz) > 0.0
                ? dot(ubo.clipPlanes[i].xyz, world.xyz) - ubo.clipPlanes[i].w
                : -1e9;
    }
    fragKey = ubo.key;
    fragFill = ubo.fill;
    fragRim = ubo.rim;
    fragAmbient = ubo.ambient;
}
