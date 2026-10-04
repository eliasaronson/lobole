#version 460
#extension GL_EXT_ray_tracing : require

// Radiance + RNG state, plus the primary-hit G-buffer the outline pass reads.
// The viewport draws crisp edges over every body and the still drew none, so a
// row of butted deck boards came back as one continuous slab. gnormal is the
// FACE normal, not the interpolated one: a hard edge is then an exact
// discontinuity, while a finely tessellated cylinder is not.
struct RayPayload {
    vec3 radiance;
    uint rngState;
    vec3 gnormal;
    float hitT;   // < 0.0 on a miss
    uint instId;  // 0 = miss or shadow catcher, else geometry index + 1
    // A path through glass (docs/rendering/materials.md#glass): closest-hit sets
    // kFlagContinue and the next ray, and raygen's loop traces it, weighting
    // what it finds by `throughput`. SHADOW rays carry this payload too, so
    // the one hit group's any-hit can read it: then radiance is the ray's
    // transmittance and kFlagBlocked says something opaque was in the way.
    // Identical in raygen, closesthit, miss, shadow.rmiss and section.rahit.
    uint flags;
    vec3 throughput;
    vec3 nextOrigin;
    vec3 nextDir;
};
const uint kFlagContinue = 1u;
const uint kFlagReflected = 2u; // the path has reflected: miss shows the HDRI
const uint kFlagBlocked = 4u;
layout(location = 0) rayPayloadInEXT RayPayload payload;

layout(binding = 2, set = 0) uniform UniformBufferObject {
    mat4 viewInverse;
    mat4 projInverse;
    vec4 viewPos;
    vec4 lightDir;
    vec4 lightColor;
    vec4 ambientColor;
    vec4 params;      // x = prefilter mips, y = ray far, z = AO, w = backdrop
    vec4 fillDir;     // unused here; the block must match closesthit.rchit
    vec4 fillColor;
    vec4 rimDir;
    vec4 rimColor;
    // Section planes; a hit is removed where dot(n, p) > w. clipInfo.x is how
    // many are live, so a scene with no section costs one compare per ray.
    vec4 clipPlanes[4];
    vec4 clipInfo;
    // World direction -> environment-map direction (core/LightRig.h): the
    // studio turns with the camera unless the lights are locked to the model.
    mat4 envRotation;
} ubo;

// Full-resolution source HDRI (the prefilter chain's base mip is only 128x64,
// which upscales to a mushy background).
layout(binding = 8, set = 0) uniform sampler2D uEnvironment;

const float PI = 3.14159265359;
// Must match IBL.cpp DirToEquirectUV exactly (including the world -> env
// swizzle: the HDRI is Y-polar, the world Z-up).
vec2 dirToUV(vec3 d) {
    d = normalize(mat3(ubo.envRotation) * d); // the rig's turn of the studio
    d = vec3(d.x, d.z, -d.y); // world -> env
    float u = atan(d.z, d.x) / (2.0 * PI) + 0.5;
    float v = acos(clamp(d.y, -1.0, 1.0)) / PI;
    return vec2(u, v);
}

// World up, matching conv::kWorldUp (core/Conventions.h).
const vec3 kWorldUp = vec3(0.0, 0.0, 1.0);

// The studio backdrop (params.w != 0): what the camera sees where it sees no
// geometry. The HDRI stays the light source and stays in every reflection —
// it is simply not the picture's background any more. A 1024x512 photograph
// stretched across the frame is a smear, and a part standing in a photograph
// of a room has no scale relationship with it; a product still is shot
// against a sweep for exactly these two reasons.
//
// Vertical gradient in WORLD up, so the horizon sits where the model's floor
// is rather than sliding with the camera: dark at the base, opening out
// above, which is what gives a sweep its depth.
//
// Radiance, so it passes through the tone curve (tonecurve.glsl) like the
// model. These are the values that land on the bytes the sweep was designed
// at — floor 73, horizon 162, top 227 — under exposure 0.6 + ACES + sRGB.
vec3 studioBackdrop(vec3 dir) {
    const vec3 kFloor = vec3(0.108, 0.112, 0.121);
    const vec3 kHorizon = vec3(0.402, 0.425, 0.472);
    const vec3 kSky = vec3(1.432, 1.528, 1.739);
    float h = clamp(dot(normalize(dir), kWorldUp), -1.0, 1.0);
    return h < 0.0 ? mix(kHorizon, kFloor, pow(-h, 0.35))
                   : mix(kHorizon, kSky, pow(h, 0.55));
}

void main() {
    // No geometry: the outline pass reads this as the background, so anything
    // drawn against it gets a silhouette.
    payload.gnormal = vec3(0.0);
    payload.hitT = -1.0;
    payload.instId = 0u;

    vec3 dir = normalize(gl_WorldRayDirectionEXT);
    // A path that has REFLECTED (off glass) shows the lighting environment,
    // as every other reflection in the still does through the IBL: the
    // backdrop is the picture's background, not what a pane reflects.
    bool reflected = (payload.flags & kFlagReflected) != 0u;
    payload.radiance = ubo.params.w != 0.0 && !reflected
        ? studioBackdrop(dir)
        : textureLod(uEnvironment, dirToUV(dir), 0.0).rgb;
}
