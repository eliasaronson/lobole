// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#version 450

// Depth-aware 4x4 blur over the raw AO, sized to the 4x4 noise tile so the
// rotation pattern averages out; the depth weight keeps occlusion from
// bleeding across silhouette edges.
layout(binding = 0) uniform sampler2D uAO;     // raw half-res AO
layout(binding = 1) uniform sampler2D uDepth;  // prepass depth (full-res)

layout(push_constant) uniform PushConstants {
    // xy = AO-target texel size, z = depth sharpness (weight falloff per
    // unit of raw [0,1] depth difference), w = unused
    vec4 params;
} pc;

layout(location = 0) in vec2 fragUV;

layout(location = 0) out float outAO;

void main() {
    float centerDepth = texture(uDepth, fragUV).r;
    // Compare each neighbor against the local depth PLANE, not the center
    // value: on a sloped or curved surface the raw comparison down-weights
    // half the window, the 4x4 noise tile stops averaging out, and
    // tile-period banding appears (the barrels, 2026-08-26). Derivatives
    // are per blur-target pixel — exactly the offset unit below. Clamped so
    // a silhouette's huge gradient cannot predict across the edge: past the
    // weight's own tolerance a prediction could only resurrect neighbors
    // the raw test rightly rejects.
    vec2 grad = vec2(dFdx(centerDepth), dFdy(centerDepth));
    float maxGrad = 1.6 / pc.params.z;
    grad = clamp(grad, vec2(-maxGrad), vec2(maxGrad));
    float sum = 0.0;
    float weightSum = 0.0;
    for (int x = -2; x < 2; x++) {
        for (int y = -2; y < 2; y++) {
            vec2 off = vec2(x, y) + 0.5;
            vec2 uv = fragUV + off * pc.params.xy;
            float predicted = centerDepth + dot(grad, off);
            float w = exp(-abs(texture(uDepth, uv).r - predicted) *
                          pc.params.z);
            sum += texture(uAO, uv).r * w;
            weightSum += w;
        }
    }
    outAO = sum / max(weightSum, 1e-5);
}
