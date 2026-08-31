#version 450

struct Light {
    vec4 direction; // xyz = toward the light, w = intensity
    vec4 color;     // rgb
};

layout(push_constant) uniform Push {
    mat4 model;
    vec4 baseColor;
} pc;

layout(location = 0) in vec3 fragNormal;
layout(location = 1) in vec3 fragWorld;
layout(location = 2) in vec4 fragClipDist;
layout(location = 3) in Light fragKey;
layout(location = 5) in Light fragFill;
layout(location = 7) in Light fragRim;
layout(location = 9) in vec4 fragAmbient;

layout(location = 0) out vec4 outColor;

// Deliberately flat and matte — no IBL, no shadows, no specular. The stock is
// a stand-in for material that is being taken away: it wants to read as a
// solid block whose steps and floors are legible, not as a finished part
// competing with the model beside it. That much is unchanged.
//
// What DID change is where the light comes from. This shader carried its own
// three directions, hard-coded, and they had drifted from the rig every other
// surface in the scene is lit by — so the block was lit from one place and
// the part beside it from another, which is most of why the two never looked
// like one picture. The rig is core/Conventions.h now and arrives in the UBO.
float diffuse(Light l, vec3 n) {
    return max(dot(n, normalize(l.direction.xyz)), 0.0) * l.direction.w;
}

// The key, WRAPPED: `dot*0.5 + 0.5` instead of `max(dot, 0)`, so a face turned
// away from it is dim rather than black. The part gets that fill from the
// irradiance map; the stock has no IBL, and with plain Lambert a block whose
// machined face happens to point away from the key rendered near-black — the
// panel in the simulation went the colour of the background. Half-Lambert is
// the cheap stand-in, and it keeps the promise this shader is written to:
// every step and floor legible from any angle.
float wrapped(Light l, vec3 n) {
    return (dot(n, normalize(l.direction.xyz)) * 0.5 + 0.5) * l.direction.w;
}

void main() {
    if (any(greaterThan(fragClipDist, vec4(0.0)))) discard; // section planes
    vec3 n = normalize(fragNormal);
    // Two-sided: a wall inside a pocket is seen from whichever side the
    // camera is on, and a black face there reads as a hole in the block.
    if (!gl_FrontFacing) n = -n;

    // Key and fill both WRAPPED — they point roughly opposite each other, so
    // wrapping the pair gives a smooth all-round base with the scene's own
    // hues, which is what the part gets from the irradiance map and the stock
    // has no way to sample. The ambient is doubled for the same reason: the
    // scene's value is calibrated to be topped up by IBL, and on its own it
    // left the block near-black whenever its machined face pointed away from
    // the key — which, for a panel laid flat, is most of the time.
    //
    // The weights are chosen so the block's brightness lands where the old
    // hard-coded rig put it. That much was right; only the DIRECTIONS were
    // wrong, and those now come from core/Conventions.h with everything else.
    vec3 lit = fragAmbient.rgb * fragAmbient.a * 3.0
             + fragKey.color.rgb * wrapped(fragKey, n) * 0.55
             + fragFill.color.rgb * wrapped(fragFill, n) * 0.45
             + fragRim.color.rgb * diffuse(fragRim, n) * 0.25;
    outColor = vec4(pc.baseColor.rgb * lit, pc.baseColor.a);
}
