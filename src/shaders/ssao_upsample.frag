#version 450

// Joint-bilateral upsample of the blurred half-res AO to full resolution
// (docs/rendering/ssao-upsample.md). Each full-res pixel blends the four
// surrounding half-res AO texels weighted by bilinear proximity x depth
// similarity — so AO comes only from THIS pixel's surface, which is exactly
// what plain bilinear gets wrong at silhouettes (halos, contact smear).
// Weighting convention matches ssao_blur.frag: raw [0,1] depth difference,
// exp falloff, sharpness in the push constant.
layout(binding = 0) uniform sampler2D uAO;    // blurred half-res AO
layout(binding = 1) uniform sampler2D uDepth; // prepass depth (full-res)

layout(push_constant) uniform PushConstants {
    // xy = AO (half-res) texel size, z = depth sharpness, w = unused
    vec4 params;
} pc;

layout(location = 0) in vec2 fragUV;

layout(location = 0) out float outAO;

void main() {
    float centerDepth = texture(uDepth, fragUV).r;
    vec2 texel = pc.params.xy;
    // The 2x2 half-res texel neighborhood around this full-res pixel.
    vec2 pos = fragUV / texel - 0.5;
    vec2 base = (floor(pos) + 0.5) * texel;
    vec2 f = fract(pos);
    vec4 bilinear = vec4((1.0 - f.x) * (1.0 - f.y), f.x * (1.0 - f.y),
                         (1.0 - f.x) * f.y, f.x * f.y);
    vec2 offsets[4] = vec2[](vec2(0.0), vec2(texel.x, 0.0),
                             vec2(0.0, texel.y), texel);
    float sum = 0.0;
    float weightSum = 0.0;
    float nearest = 1.0;
    float nearestW = -1.0;
    for (int i = 0; i < 4; i++) {
        vec2 uv = base + offsets[i];
        float dw = exp(-abs(texture(uDepth, uv).r - centerDepth) *
                       pc.params.z);
        float ao = texture(uAO, uv).r;
        sum += ao * bilinear[i] * dw;
        weightSum += bilinear[i] * dw;
        if (dw > nearestW) {
            nearestW = dw;
            nearest = ao;
        }
    }
    // A pixel whose whole 2x2 neighborhood lies across a depth edge (a thin
    // silhouette against far background): the depth-nearest texel beats a
    // 0/0 blend — the case plain bilinear is maximally wrong about.
    outAO = weightSum > 1e-4 ? sum / weightSum : nearest;
}
