#version 450

layout(push_constant) uniform Push {
    mat4 model;
    vec4 baseColor;
} pc;

layout(location = 0) in vec3 fragNormal;
layout(location = 1) in vec3 fragWorld;
layout(location = 2) in vec4 fragClipDist;

layout(location = 0) out vec4 outColor;

// Deliberately flat and matte. The stock is a stand-in for material that is
// being taken away — it wants to read as a solid block whose steps and floors
// are legible, not as a finished part competing with the model beside it.
// Three fixed directional terms, no IBL, no shadows: this is the FieldRenderer
// precedent (its own pipeline, its own simple shading) and the same trade.
const vec3 kKey  = normalize(vec3(-0.45, 0.75, 0.5));
const vec3 kFill = normalize(vec3(0.6, 0.35, 0.3));
const vec3 kRim  = normalize(vec3(0.2, -0.8, -0.4));

void main() {
    if (any(greaterThan(fragClipDist, vec4(0.0)))) discard; // section planes
    vec3 n = normalize(fragNormal);
    // Two-sided: a wall inside a pocket is seen from whichever side the
    // camera is on, and a black face there reads as a hole in the block.
    if (!gl_FrontFacing) n = -n;

    float key  = max(dot(n, kKey), 0.0);
    float fill = max(dot(n, kFill), 0.0) * 0.35;
    float rim  = max(dot(n, kRim), 0.0) * 0.15;
    vec3 lit = pc.baseColor.rgb * (0.30 + key * 0.75 + fill + rim);
    outColor = vec4(lit, pc.baseColor.a);
}
