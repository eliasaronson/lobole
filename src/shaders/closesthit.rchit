#version 460
#extension GL_EXT_ray_tracing : require
#extension GL_EXT_buffer_reference2 : require
#extension GL_GOOGLE_include_directive : require

#include "grain.glsl"  // procedural wood/plywood grain, shared with mesh.frag

// Radiance + RNG state (shared with raygen).
struct RayPayload {
    vec3 radiance;
    uint rngState;
};
layout(location = 0) rayPayloadInEXT RayPayload payload;
layout(location = 1) rayPayloadEXT bool isShadowed;

hitAttributeEXT vec3 attribs;

layout(binding = 0, set = 0) uniform accelerationStructureEXT topLevelAS;

layout(binding = 2, set = 0) uniform UniformBufferObject {
    mat4 viewInverse;
    mat4 projInverse;
    vec4 viewPos;
    vec4 lightDir;
    vec4 lightColor;
    vec4 ambientColor;
    vec4 params;      // x = prefilter mips, y = ray far, z = AO radius
} ubo;

// Per-body vertex/index data reached by device address. std430 float/uint
// arrays have stride 4, matching the 6-float Vertex3D layout exactly.
layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer FloatBuf { float f[]; };
layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer UintBuf { uint u[]; };

// Geometry behavior flags (must match RTGeometryFlags in RayTracingRenderer.h).
const uint kFlagShadowCatcher = 1u;

struct GeometryInfo {
    FloatBuf verts;
    UintBuf inds;
    uint materialIndex;
    uint flags;
    uint pad0;
    uint pad1;
};
layout(binding = 3, set = 0, std430) readonly buffer GeometryInfos {
    GeometryInfo geoms[];
};

struct Material {
    vec3 baseColor;
    float metallic;
    vec3 emissive;
    float roughness;
    vec3 specular;
    float transmission;
    // Procedural grain: x = pattern (0 flat/1 wood/2 plywood), y = grainScale,
    // z = grainContrast, w = grain-axis index (0/1/2). Matches RTMaterial.
    vec4 procParams;
};
layout(binding = 4, set = 0, std430) readonly buffer Materials {
    Material materials[];
};

// Shared IBL maps (same textures the raster viewport uses; equirectangular).
layout(binding = 5, set = 0) uniform sampler2D uIrradiance;
layout(binding = 6, set = 0) uniform sampler2D uPrefilter;
layout(binding = 7, set = 0) uniform sampler2D uBrdfLut;
layout(binding = 8, set = 0) uniform sampler2D uEnvironment; // full-res HDRI

struct Vertex {
    vec3 pos;
    vec3 normal;
};

const float PI = 3.14159265359;
const float kSunCos = 0.9994; // shadow-ray cone, ~2 degree half-angle
// Ambient-occlusion reach and the shadow rays' far limit both arrive in the
// UBO (params.z / params.y), sized from the scene: as literals they were a
// length in whatever unit the model happened to use.

// Must match IBL.cpp DirToEquirectUV / mesh.frag dirToUV exactly (including
// the world -> env swizzle: the HDRI is Y-polar, the world Z-up).
vec2 dirToUV(vec3 d) {
    d = normalize(d);
    d = vec3(d.x, d.z, -d.y); // world -> env
    float u = atan(d.z, d.x) / (2.0 * PI) + 0.5;
    float v = acos(clamp(d.y, -1.0, 1.0)) / PI;
    return vec2(u, v);
}

uint nextRand(inout uint s) {
    s = s * 747796405u + 2891336453u;
    uint w = ((s >> ((s >> 28u) + 4u)) ^ s) * 277803737u;
    return (w >> 22u) ^ w;
}
float nextFloat(inout uint s) {
    return float(nextRand(s)) * (1.0 / 4294967296.0);
}

// Frisvad orthonormal basis around n.
void basis(vec3 n, out vec3 t, out vec3 b) {
    float sgn = n.z >= 0.0 ? 1.0 : -1.0;
    float a = -1.0 / (sgn + n.z);
    float bb = n.x * n.y * a;
    t = vec3(1.0 + sgn * n.x * n.x * a, sgn * bb, -sgn * n.x);
    b = vec3(bb, sgn + n.y * n.y * a, -n.y);
}

vec3 cosineSample(vec3 n, inout uint s) {
    float u1 = nextFloat(s), u2 = nextFloat(s);
    float r = sqrt(u1);
    float phi = 2.0 * PI * u2;
    vec3 t, b;
    basis(n, t, b);
    return normalize(t * (r * cos(phi)) + b * (r * sin(phi)) +
                     n * sqrt(max(0.0, 1.0 - u1)));
}

// Uniform direction within a cone of half-angle acos(cosMax) around dir.
vec3 coneSample(vec3 dir, float cosMax, inout uint s) {
    float u1 = nextFloat(s), u2 = nextFloat(s);
    float cosT = mix(cosMax, 1.0, u1);
    float sinT = sqrt(max(0.0, 1.0 - cosT * cosT));
    float phi = 2.0 * PI * u2;
    vec3 t, b;
    basis(dir, t, b);
    return normalize(t * (sinT * cos(phi)) + b * (sinT * sin(phi)) +
                     dir * cosT);
}

Vertex unpackVertex(GeometryInfo g, uint index) {
    uint vi = g.inds.u[index];
    uint o = vi * 6u; // 3 pos + 3 normal
    Vertex v;
    v.pos = vec3(g.verts.f[o + 0], g.verts.f[o + 1], g.verts.f[o + 2]);
    v.normal = vec3(g.verts.f[o + 3], g.verts.f[o + 4], g.verts.f[o + 5]);
    return v;
}

// Whether a ray from `origin` along `dir` hits any geometry within `tMax`.
bool traceOcclusion(vec3 origin, vec3 dir, float tMax) {
    isShadowed = true;
    uint flags = gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsOpaqueEXT |
                 gl_RayFlagsSkipClosestHitShaderEXT;
    // sbtRecordOffset 0 keeps the ray in the single hit region; missIndex 1 is
    // the shadow-miss shader, which clears isShadowed on a miss.
    traceRayEXT(topLevelAS, flags, 0xff, 0, 0, 1, origin, 0.001, dir, tMax, 1);
    return isShadowed;
}

// Blinn-Phong direct term (mirrors mesh.frag's analytic key light).
vec3 directLight(vec3 N, vec3 V, vec3 L, vec3 lightColor, Material mat,
                 float roughness) {
    float NdotL = max(dot(N, L), 0.0);
    vec3 diffuse = mat.baseColor * lightColor * NdotL;

    vec3 H = normalize(L + V);
    float NdotH = max(dot(N, H), 0.0);
    float shininess = mix(16.0, 256.0, 1.0 - roughness);
    float spec = pow(NdotH, shininess);

    float VdotH = max(dot(V, H), 0.0);
    vec3 F0 = mix(vec3(0.04), mat.baseColor, mat.metallic);
    vec3 fresnel = F0 + (1.0 - F0) * pow(1.0 - VdotH, 5.0);
    vec3 specular = lightColor * spec * fresnel * mat.specular;

    return mix(diffuse, vec3(0.0), mat.metallic) + specular;
}

void main() {
    GeometryInfo g = geoms[gl_InstanceCustomIndexEXT];
    uint prim = gl_PrimitiveID;
    Vertex v0 = unpackVertex(g, prim * 3 + 0);
    Vertex v1 = unpackVertex(g, prim * 3 + 1);
    Vertex v2 = unpackVertex(g, prim * 3 + 2);

    vec3 bc = vec3(1.0 - attribs.x - attribs.y, attribs.x, attribs.y);
    vec3 pos = v0.pos * bc.x + v1.pos * bc.y + v2.pos * bc.z;
    vec3 nrm = normalize(v0.normal * bc.x + v1.normal * bc.y + v2.normal * bc.z);

    vec3 worldPos = vec3(gl_ObjectToWorldEXT * vec4(pos, 1.0));
    vec3 N = normalize(mat3(gl_ObjectToWorldEXT) * nrm);
    vec3 V = normalize(ubo.viewPos.xyz - worldPos);
    if (dot(N, V) < 0.0) N = -N;

    // ---- Shadow catcher (the ground plane): show the environment as if the
    // ray had missed, darkened by key-light shadow + contact occlusion. Keeps
    // the backdrop photographic while giving bodies something to sit on. ----
    if ((g.flags & kFlagShadowCatcher) != 0u) {
        vec3 dir = normalize(gl_WorldRayDirectionEXT);
        vec3 env = textureLod(uEnvironment, dirToUV(dir), 0.0).rgb;
        vec3 Lc = normalize(-ubo.lightDir.xyz);
        vec3 Lcs = coneSample(Lc, kSunCos, payload.rngState);
        float sun = traceOcclusion(worldPos + N * 0.001, Lcs, ubo.params.y) ? 0.0 : 1.0;
        vec3 aoDir = cosineSample(N, payload.rngState);
        float ao = traceOcclusion(worldPos + N * 0.001, aoDir, ubo.params.z) ? 0.0 : 1.0;
        const float kShadowStrength = 0.55; // how dark the umbra gets
        const float kAOStrength = 0.35;     // extra contact darkening
        float shade = mix(1.0, sun, kShadowStrength) * mix(1.0, ao, kAOStrength);
        payload.radiance = env * shade;
        return;
    }

    Material mat = materials[g.materialIndex];
    float roughness = clamp(mat.roughness, 0.03, 1.0);

    // Procedural wood/plywood grain. The still render uses identity instance
    // transforms, so `pos`/`nrm` are already part-local (object space) — exactly
    // what the raster path feeds as fragLocalPos. Modifies mat.baseColor +
    // roughness in place so both the direct and IBL terms below pick it up.
    grain_apply(mat.baseColor, roughness, mat.procParams, pos,
                abs(dot(nrm, grain_axisUnit(int(mat.procParams.w + 0.5)))));

    // ---- Direct key light with a soft (area) shadow ----
    vec3 L = normalize(-ubo.lightDir.xyz);
    vec3 lightColor = ubo.lightColor.rgb * ubo.lightDir.w;
    // Jitter the shadow ray in a small cone -> penumbrae as samples accumulate.
    vec3 Ls = coneSample(L, kSunCos, payload.rngState);
    float shadow = traceOcclusion(worldPos + N * 0.001, Ls, ubo.params.y) ? 0.0 : 1.0;
    vec3 direct = directLight(N, V, L, lightColor, mat, roughness) * shadow;

    // ---- IBL ambient (split-sum, shared with the raster viewport) ----
    vec3 R = reflect(-V, N);
    float NdotV = max(dot(N, V), 0.0);
    vec3 F0 = mix(vec3(0.04), mat.baseColor, mat.metallic);
    vec3 F = F0 + (max(vec3(1.0 - roughness), F0) - F0) * pow(1.0 - NdotV, 5.0);
    vec3 kD = (1.0 - F) * (1.0 - mat.metallic);

    vec3 irradiance = texture(uIrradiance, dirToUV(N)).rgb;
    vec3 diffuseIBL = irradiance * mat.baseColor;

    float maxLod = max(ubo.params.x - 1.0, 0.0);
    vec3 prefiltered = textureLod(uPrefilter, dirToUV(R), roughness * maxLod).rgb;
    // Near-mirror surfaces reflect the full-res environment instead of the
    // low-res prefilter base, so chrome shows a crisp room.
    float sharp = 1.0 - smoothstep(0.0, 0.15, roughness);
    if (sharp > 0.0)
        prefiltered = mix(prefiltered,
                          textureLod(uEnvironment, dirToUV(R), 0.0).rgb, sharp);
    vec2 envBRDF = texture(uBrdfLut, vec2(NdotV, roughness)).rg;
    vec3 specularIBL = prefiltered * (F * envBRDF.x + envBRDF.y);

    // ---- Ambient occlusion: one cosine-hemisphere ray, local radius ----
    vec3 aoDir = cosineSample(N, payload.rngState);
    float ao = traceOcclusion(worldPos + N * 0.001, aoDir, ubo.params.z) ? 0.0 : 1.0;

    vec3 ambient = (kD * diffuseIBL + specularIBL) * ao;

    // Direct dialed back (IBL supplies the environment fill), matching mesh.frag.
    payload.radiance = ambient + 0.5 * direct + mat.emissive;
}
