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
    int flags;         // FieldStyle: 1 edges, 2 plain, 4 lit, 8 unclipped,
                       // 16 reversed
} pc;

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inDisplacement;
layout(location = 2) in float inScalar;
layout(location = 3) in vec3 inEdge;

layout(location = 0) out float fragScalar;
layout(location = 1) out vec4 fragClipDist;
layout(location = 2) out vec3 fragEdge;
layout(location = 3) out vec3 fragViewPos;

void main() {
    vec3 world = inPosition + pc.deformScale * inDisplacement;
    vec4 viewPos = ubo.view * vec4(world, 1.0);
    gl_Position = ubo.proj * viewPos;
    fragScalar = inScalar;
    fragEdge = inEdge;
    fragViewPos = viewPos.xyz;
    // Section clip (shared with the mesh pipeline): fragments past the plane
    // are discarded in the fragment shader. The mesh view's crinkle cut is
    // made of whole elements on the CPU, and is not clipped again.
    for (int i = 0; i < 4; i++) {
        fragClipDist[i] =
            (pc.flags & 8) == 0 &&
                    dot(ubo.clipPlanes[i].xyz, ubo.clipPlanes[i].xyz) > 0.0
                ? dot(ubo.clipPlanes[i].xyz, world) - ubo.clipPlanes[i].w
                : -1e9;
    }
}
