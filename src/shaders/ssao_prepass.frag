#version 450

layout(location = 0) in vec3 fragViewNormal;
layout(location = 1) in float fragClipDist;

layout(location = 0) out vec4 outNormal;

void main() {
    if (fragClipDist > 0.0) {
        discard;
    }
    // View-space normal packed [-1,1] -> [0,1] for the RGBA8 target.
    outNormal = vec4(normalize(fragViewNormal) * 0.5 + 0.5, 1.0);
}
