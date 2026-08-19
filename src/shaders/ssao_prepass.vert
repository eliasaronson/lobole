#version 450

// Depth+normal prepass variant of mesh.vert (kept in lockstep with its
// instance-transform + section-clip conventions): renders the opaque scene
// into a non-MSAA depth + view-space-normal target that the SSAO pass reads.
layout(binding = 0) uniform PrepassUBO {
    mat4 view;
    mat4 proj;
    vec4 clipPlanes[4]; // section clips (world); an all-zero slot is off
} ubo;

layout(push_constant) uniform PushConstants {
    mat4 model;
} pc;

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;

layout(location = 0) out vec3 fragViewNormal;
layout(location = 1) out vec4 fragClipDist;

void main() {
    vec4 worldPos = pc.model * vec4(inPosition, 1.0);
    gl_Position = ubo.proj * ubo.view * worldPos;

    // View-space normal (normal matrix for correctness under non-uniform
    // scale, same as mesh.vert's world-space one).
    mat3 normalMatrix = transpose(inverse(mat3(ubo.view * pc.model)));
    fragViewNormal = normalize(normalMatrix * inNormal);

    // Same sign convention as mesh.vert; the fragment stage discards > 0 so
    // sectioned-away material does not occlude.
    for (int i = 0; i < 4; i++) {
        fragClipDist[i] =
            dot(ubo.clipPlanes[i].xyz, ubo.clipPlanes[i].xyz) > 0.0
                ? dot(ubo.clipPlanes[i].xyz, worldPos.xyz) - ubo.clipPlanes[i].w
                : -1e9;
    }
}
