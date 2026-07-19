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
    float sum = 0.0;
    float weightSum = 0.0;
    for (int x = -2; x < 2; x++) {
        for (int y = -2; y < 2; y++) {
            vec2 uv = fragUV + (vec2(x, y) + 0.5) * pc.params.xy;
            float w = exp(-abs(texture(uDepth, uv).r - centerDepth) *
                          pc.params.z);
            sum += texture(uAO, uv).r * w;
            weightSum += w;
        }
    }
    outAO = sum / max(weightSum, 1e-5);
}
