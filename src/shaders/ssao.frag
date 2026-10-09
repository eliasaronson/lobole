// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#version 450

// SSAO (docs/rendering/README.md#ssao): normal-oriented hemisphere
// sampling in view space against the prepass depth, rotated per-pixel by
// interleaved gradient noise. Output is half-res R8 occlusion, bilaterally
// blurred by ssao_blur.frag before mesh.frag multiplies it into ambient/IBL.
layout(binding = 0) uniform SSAOParams {
    mat4 proj;     // view -> clip (includes the Vulkan Y flip)
    mat4 invProj;  // clip -> view
    // x = view-space sample radius, y = depth bias, z = strength, w = unused
    vec4 params;
    // xy = noise tiling scale (aoResolution / 4), zw = unused
    vec4 params2;
} ubo;

layout(binding = 1) uniform sampler2D uDepth;   // prepass depth (full-res)
layout(binding = 2) uniform sampler2D uNormal;  // prepass view-space normals
// Bound but no longer sampled: rotation now comes from per-pixel
// interleaved gradient noise below. The 4x4 tile's period is 4 AO texels =
// 8 screen px at half res, and its residual after the blur read as visible
// STRIPES on curved surfaces (the barrels, 2026-08-26) where the same
// residual at full res passed as fine grain. Per-pixel noise has no period
// for the eye to find; the binding stays so the C++ layout is untouched.
layout(binding = 3) uniform sampler2D uNoise;

layout(location = 0) in vec2 fragUV;

layout(location = 0) out float outAO;

// 16 hemisphere samples (+Z oriented), clustered toward the origin with
// scale = lerp(0.1, 1, (i/16)^2). Values are arbitrary fixed randoms —
// regenerate freely, nothing depends on them exactly.
const vec3 kKernel[16] = vec3[](
    vec3(-0.0308, 0.0026, 0.0143), vec3(-0.0197, 0.0907, 0.0090),
    vec3(-0.0294, -0.0497, 0.0113), vec3(0.0184, -0.0169, 0.0378),
    vec3(-0.0192, -0.0494, 0.0806), vec3(-0.1147, 0.0334, 0.1230),
    vec3(-0.1725, -0.0055, 0.0216), vec3(-0.0875, 0.0918, 0.1444),
    vec3(-0.0381, 0.2416, 0.0792), vec3(-0.0622, -0.0330, 0.0990),
    vec3(-0.0172, -0.1488, 0.0361), vec3(0.1687, -0.2331, 0.0897),
    vec3(0.2530, -0.1540, 0.1545), vec3(-0.3254, -0.2693, 0.1365),
    vec3(0.1637, -0.4358, 0.0495), vec3(-0.1826, 0.0927, 0.6673)
);

// Reconstruct the view-space position under fragUV conventions shared with
// the prepass (same proj, so no Y-flip special-casing anywhere).
vec3 viewPosAt(vec2 uv, float depth) {
    vec4 clip = vec4(uv * 2.0 - 1.0, depth, 1.0);
    vec4 view = ubo.invProj * clip;
    return view.xyz / view.w;
}

void main() {
    float depth = texture(uDepth, fragUV).r;
    if (depth >= 1.0) { // background: nothing to occlude
        outAO = 1.0;
        return;
    }
    vec3 origin = viewPosAt(fragUV, depth);
    vec3 normal = normalize(texture(uNormal, fragUV).xyz * 2.0 - 1.0);
    // Interleaved gradient noise (Jimenez): a well-distributed per-pixel
    // rotation angle. gl_FragCoord is in AO-target pixels here.
    float ign = fract(52.9829189 *
                      fract(0.06711056 * gl_FragCoord.x +
                            0.00583715 * gl_FragCoord.y));
    float angle = 6.2831853 * ign;
    vec3 rand = vec3(cos(angle), sin(angle), 0.0);

    // Per-pixel-rotated tangent basis (Gram-Schmidt); fall back to a fixed
    // axis in the rare case the noise vector is parallel to the normal.
    vec3 tangent = rand - normal * dot(rand, normal);
    tangent = dot(tangent, tangent) > 1e-5
                  ? normalize(tangent)
                  : normalize(cross(normal, vec3(0.577, 0.577, 0.577)));
    mat3 tbn = mat3(tangent, cross(normal, tangent), normal);

    float radius = ubo.params.x;
    float bias = ubo.params.y;
    float occlusion = 0.0;
    for (int i = 0; i < 16; i++) {
        vec3 samplePos = origin + tbn * (kKernel[i] * radius);
        vec4 clip = ubo.proj * vec4(samplePos, 1.0);
        vec2 uv = (clip.xy / clip.w) * 0.5 + 0.5;
        float sceneZ = viewPosAt(uv, texture(uDepth, uv).r).z;
        // View space looks down -Z: nearer surfaces have GREATER z, so the
        // sample is occluded when the scene surface is in front of it. The
        // range window zeroes anything more than 2 radii away: an occluder
        // that far in front is a separate surface (a neighboring body's
        // silhouette), not a crevice wall — the old 1/distance falloff let
        // those bleed dark arcs onto flat faces behind them (S3 halo fix).
        float rangeCheck =
            1.0 - smoothstep(radius, 2.0 * radius, abs(origin.z - sceneZ));
        occlusion += (sceneZ >= samplePos.z + bias ? 1.0 : 0.0) * rangeCheck;
    }
    outAO = 1.0 - ubo.params.z * (occlusion / 16.0);
}
