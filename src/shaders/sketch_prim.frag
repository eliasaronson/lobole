#version 450

// SDF anti-aliasing for the expanded primitives: fade the last ~1 px at the
// edge (line half-width or dot radius) instead of a hard cut.
layout(push_constant) uniform Push {
    vec4 tint; // .a scales opacity
} push;

layout(location = 0) in vec4 fragColor;
layout(location = 1) in vec2 vLocal;   // cross-section coord in [-1, 1]
layout(location = 2) in float vHalfPx;
layout(location = 3) in float vShape;  // 0 = line, 1 = dot

layout(location = 0) out vec4 outColor;

void main() {
    // Distance from centerline (line) or center (dot) in pixels.
    float d = (vShape < 0.5 ? abs(vLocal.x) : length(vLocal)) * vHalfPx;
    float alpha = 1.0 - smoothstep(vHalfPx - 1.0, vHalfPx, d);
    alpha *= fragColor.a * push.tint.a;
    if (alpha <= 0.001)
        discard;
    outColor = vec4(fragColor.rgb, alpha);
}
