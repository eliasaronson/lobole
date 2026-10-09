// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#version 450

layout(binding = 0) uniform UniformBufferObject {
    mat4 view;
    mat4 proj;
    vec4 clipPlanes[4]; // section clips (world); an all-zero slot is off
} ubo;

layout(push_constant) uniform Push {
    float deformScale; // displacement warp multiplier
} pc;

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inDisplacement;
layout(location = 2) in float inScalar;

layout(location = 0) out float fragScalar;
layout(location = 1) out vec4 fragClipDist;

void main() {
    vec3 world = inPosition + pc.deformScale * inDisplacement;
    gl_Position = ubo.proj * ubo.view * vec4(world, 1.0);
    fragScalar = inScalar;
    // Section clip (shared with the mesh pipeline): fragments past the plane
    // are discarded in the fragment shader.
    for (int i = 0; i < 4; i++) {
        fragClipDist[i] =
            dot(ubo.clipPlanes[i].xyz, ubo.clipPlanes[i].xyz) > 0.0
                ? dot(ubo.clipPlanes[i].xyz, world) - ubo.clipPlanes[i].w
                : -1e9;
    }
}
