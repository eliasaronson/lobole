#version 450

struct Light {
    vec4 direction;  // xyz = direction, w = intensity
    vec4 color;      // rgb = color, a = unused
};

layout(binding = 0) uniform UniformBufferObject {
    mat4 view;
    mat4 proj;
    vec4 viewPos;      // Camera position
    Light primaryLight; // Main directional light
    Light fillLight;    // Fill light
    Light rimLight;     // Rim light
    vec4 ambientColor;  // rgb = ambient color, a = intensity
    // Section clip planes (world space): xyz = normal, w = distance. Global
    // for the frame, so they live here rather than in per-draw push
    // constants. An all-zero slot is disabled. Sections INTERSECT: a fragment
    // survives only where it is on the kept side of every enabled plane.
    vec4 clipPlanes[4];
    // Key-light shadow map (S1): world -> light clip space, and params
    // x = strength, y = normal-offset world distance, z = texel size (UV),
    // w = enable. Forwarded to the fragment stage as varyings.
    mat4 lightViewProj;
    vec4 shadowParams;
    // SSAO (S2): xy = 1 / framebuffer size (gl_FragCoord -> AO uv),
    // z = enable, w = unused. Forwarded as a varying like shadowParams.
    vec4 aoParams;
    vec4 aoParams2;
    // World -> environment-map direction (core/LightRig.h). Read by
    // mesh.frag, which declares this same block.
    mat4 envRotation;
} ubo;

layout(push_constant) uniform PushConstants {
    mat4 model;
    // Vertex ranges [x,y) selected, [z,w) hovered for face highlighting
    uvec4 highlight;
    // PBR: x=metallic, y=roughness, z=specular, w=reflectance (used in frag)
    vec4 material;
    // Per-instance albedo (rgb); instances of one asset can differ in color
    vec4 baseColor;
    // Procedural surface pattern: x=GRAIN_PATTERN_* (see grain.glsl),
    // y=patternScale, z=patternContrast, w=pattern-axis index (0/1/2).
    vec4 procMaterial;
} pc;

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;

layout(location = 0) out vec3 fragColor;
layout(location = 1) out vec3 fragNormal;
layout(location = 2) out vec3 fragWorldPos;
layout(location = 3) out vec3 fragViewPos;
layout(location = 4) out Light fragPrimaryLight;
layout(location = 6) out Light fragFillLight;
layout(location = 8) out Light fragRimLight;
layout(location = 10) out vec3 fragAmbientColor;
layout(location = 11) out float fragHighlight;
// One signed distance per plane. Each is linear in world position, so
// interpolation is exact — taking a max here instead would clip wrong along
// the crease where two sections meet.
layout(location = 12) out vec4 fragClipDist;
layout(location = 13) out vec4 fragShadowCoord;
layout(location = 14) out vec4 fragShadowParams;
layout(location = 15) out vec4 fragAOParams;
layout(location = 18) out vec4 fragAOParams2;
// Local (object-space) position, for procedural grain that stays glued to the
// part under any model/assembly transform (see mesh.frag).
layout(location = 16) out vec3 fragLocalPos;

void main() {
    // Transform position to world space
    vec4 worldPos = pc.model * vec4(inPosition, 1.0);
    fragWorldPos = worldPos.xyz;
    // Object-space position drives procedural grain in the fragment stage.
    fragLocalPos = inPosition;

    // Final position
    gl_Position = ubo.proj * ubo.view * worldPos;

    // Transform normal to world space (use normal matrix for proper scaling)
    mat3 normalMatrix = transpose(inverse(mat3(pc.model)));
    fragNormal = normalize(normalMatrix * inNormal);

    // Face highlight: 0.5 = hovered; 1 + tint = selected, where the tint
    // (0 selection amber, 1 fixture, 2 load, 3 keep, 4 bond -- the palette
    // in mesh.frag) rides in the top four bits of the selected range's end,
    // because the push constant is full (MeshRenderer::SetFaceHighlight).
    uint v = uint(gl_VertexIndex);
    uint selEnd = pc.highlight.y & 0x0FFFFFFFu;
    uint tint = pc.highlight.y >> 28;
    fragHighlight = 0.0;
    if (v >= pc.highlight.z && v < pc.highlight.w) fragHighlight = 0.5;
    if (v >= pc.highlight.x && v < selEnd) fragHighlight = 1.0 + float(tint);

    // Signed distance past the section plane (linear in world pos, so the
    // interpolated value is exact); frag discards where it is > 0.
    for (int i = 0; i < 4; i++) {
        fragClipDist[i] =
            dot(ubo.clipPlanes[i].xyz, ubo.clipPlanes[i].xyz) > 0.0
                ? dot(ubo.clipPlanes[i].xyz, worldPos.xyz) - ubo.clipPlanes[i].w
                : -1e9; // disabled slot: never clips
    }

    // Key-light shadow lookup position, offset along the normal to fight
    // acne (normal-offset bias, ~1.5 shadow texels in world units).
    fragShadowCoord = ubo.lightViewProj *
                      vec4(worldPos.xyz + fragNormal * ubo.shadowParams.y, 1.0);
    fragShadowParams = ubo.shadowParams;
    fragAOParams = ubo.aoParams;
    fragAOParams2 = ubo.aoParams2;

    // Albedo from the per-instance push constant, plus camera position
    fragColor = pc.baseColor.rgb;
    fragViewPos = ubo.viewPos.xyz;

    // Pass through light data
    fragPrimaryLight = ubo.primaryLight;
    fragFillLight = ubo.fillLight;
    fragRimLight = ubo.rimLight;
    fragAmbientColor = ubo.ambientColor.rgb * ubo.ambientColor.a;
}
