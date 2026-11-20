// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#version 450

layout(location = 0) in float fragScalar;
layout(location = 1) in vec4 fragClipDist;
layout(location = 2) in vec3 fragEdge;
layout(location = 3) in vec3 fragViewPos;

layout(push_constant) uniform Push {
    float deformScale;
    int flags; // FieldStyle: 1 edges, 2 plain, 4 lit, 8 unclipped, 16 reversed
} pc;

layout(location = 0) out vec4 outColor;

// Analytic Turbo colormap (Google's Turbo, polynomial approximation). Perceptual,
// colorblind-safer than jet: dark blue (low) -> green -> yellow -> red (high).
vec3 turbo(float x) {
    x = clamp(x, 0.0, 1.0);
    const vec4 kRedVec4   = vec4(0.13572138, 4.61539260, -42.66032258, 132.13108234);
    const vec4 kGreenVec4 = vec4(0.09140261, 2.19418839, 4.84296658, -14.18503333);
    const vec4 kBlueVec4  = vec4(0.10667330, 12.64194608, -60.58204836, 110.36276771);
    const vec2 kRedVec2   = vec2(-152.94239396, 59.28637943);
    const vec2 kGreenVec2 = vec2(4.27729857, 2.82956604);
    const vec2 kBlueVec2  = vec2(-89.90310912, 27.34824973);
    vec4 v4 = vec4(1.0, x, x * x, x * x * x);
    vec2 v2 = v4.zw * v4.z;
    return vec3(dot(v4, kRedVec4)   + dot(v2, kRedVec2),
                dot(v4, kGreenVec4) + dot(v2, kGreenVec2),
                dot(v4, kBlueVec4)  + dot(v2, kBlueVec2));
}

// The colours past the legend's ends (FieldStyle::kOver / kUnder).
const vec3 kOver = vec3(1.0, 0.35, 0.9);
const vec3 kUnder = vec3(0.42, 0.44, 0.48);

void main() {
    if (any(greaterThan(fragClipDist, vec4(0.0)))) discard; // section planes
    vec3 color;
    if ((pc.flags & 2) != 0) {
        color = vec3(0.74, 0.76, 0.80); // the mesh view's plain body
    } else if (fragScalar > 1.0005) {
        // Past the hot end pink, past the other grey: a reversed map's hot
        // end is its low one.
        color = (pc.flags & 16) != 0 ? kUnder : kOver;
    } else if (fragScalar < -0.0005) {
        color = (pc.flags & 16) != 0 ? kOver : kUnder;
    } else {
        color = turbo((pc.flags & 16) != 0 ? 1.0 - fragScalar : fragScalar);
    }
    if ((pc.flags & 4) != 0) {
        // A headlight on the facet: its normal from the surface itself, so
        // every element face reads as a facet.
        vec3 n = normalize(cross(dFdx(fragViewPos), dFdy(fragViewPos)));
        color *= 0.5 + 0.5 * abs(dot(n, normalize(vec3(0.35, 0.5, 0.8))));
    }
    if ((pc.flags & 1) != 0) {
        // Element edges: where a barycentric coordinate nears zero, at a
        // width constant on screen.
        vec3 w = fwidth(fragEdge);
        vec3 a = smoothstep(vec3(0.0), w * 1.25, fragEdge);
        float inside = min(a.x, min(a.y, a.z));
        color = mix(vec3(0.07, 0.08, 0.10), color, 0.2 + 0.8 * inside);
    }
    outColor = vec4(color, 1.0);
}
