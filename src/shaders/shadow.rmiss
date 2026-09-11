#version 460
#extension GL_EXT_ray_tracing : require

struct RayPayload {
    vec3 radiance;
    uint rngState;
    vec3 gnormal;
    float hitT;   // < 0.0 on a miss
    uint instId;  // 0 = miss or shadow catcher, else geometry index + 1
    // A path through glass (docs/rendering/glass-plan.md): closest-hit sets
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
layout(location = 1) rayPayloadInEXT RayPayload shadowRay;

void main() {
    // Nothing opaque in the way; whatever glass it crossed is already in
    // shadowRay.radiance (section.rahit).
    shadowRay.flags &= ~kFlagBlocked;
}
