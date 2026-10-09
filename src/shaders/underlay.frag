// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#version 450

// The image's pixels are sampled as-is (UNORM) and written directly, matching
// how the sketch line pipeline writes raw colors; per-draw opacity fades the
// underlay so sketch geometry stays legible on top.
layout(set = 1, binding = 0) uniform sampler2D tex;

layout(push_constant) uniform PushConstants {
    float opacity;
} pc;

layout(location = 0) in vec2 fragUV;
layout(location = 0) out vec4 outColor;

void main() {
    vec4 texel = texture(tex, fragUV);
    outColor = vec4(texel.rgb, texel.a * pc.opacity);
}
