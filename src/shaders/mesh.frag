#version 450
#extension GL_GOOGLE_include_directive : require

#include "grain.glsl"  // procedural wood/plywood grain, shared with the ray tracer

struct Light {
    vec4 direction;  // xyz = direction, w = intensity
    vec4 color;      // rgb = color, a = unused
};

layout(location = 0) in vec3 fragColor;
layout(location = 1) in vec3 fragNormal;
layout(location = 2) in vec3 fragWorldPos;
layout(location = 3) in vec3 fragViewPos;
layout(location = 4) in Light fragPrimaryLight;
layout(location = 6) in Light fragFillLight;
layout(location = 8) in Light fragRimLight;
layout(location = 10) in vec3 fragAmbientColor;
layout(location = 11) in float fragHighlight;
layout(location = 12) in vec4 fragClipDist;
layout(location = 13) in vec4 fragShadowCoord;
layout(location = 14) in vec4 fragShadowParams;
layout(location = 15) in vec4 fragAOParams;
layout(location = 18) in vec4 fragAOParams2;
layout(location = 16) in vec3 fragLocalPos;  // object space, for procedural grain

layout(location = 0) out vec4 outColor;

layout(push_constant) uniform PushConstants {
    mat4 model;        // world transform (also read here for the plywood axis)
    uvec4 highlight;   // used in the vertex stage
    vec4 material;      // x=metallic, y=roughness, z=specular, w=reflectance
    vec4 baseColor;     // per-instance albedo (used in the vertex stage)
    // Procedural surface pattern: x=GRAIN_PATTERN_* (see grain.glsl),
    // y=patternScale, z=patternContrast, w=pattern-axis index (0/1/2).
    // See the grain helpers below.
    vec4 procMaterial;
} pc;

// Image-based lighting maps (equirectangular; see IBL.cpp for the convention).
layout(set = 0, binding = 1) uniform sampler2D uIrradiance;  // diffuse env
layout(set = 0, binding = 2) uniform sampler2D uPrefilter;   // specular mips
layout(set = 0, binding = 3) uniform sampler2D uBrdfLut;     // env BRDF (RG)
// Key-light shadow map (ShadowRenderer, S1): compare sampler with a white
// border, so anything outside the light frustum reads unshadowed.
layout(set = 0, binding = 4) uniform sampler2DShadow uShadowMap;
// Blurred SSAO (SSAORenderer, S2): half-res, sampled by framebuffer position.
layout(set = 0, binding = 5) uniform sampler2D uAO;

const float PI = 3.14159265359;
// Must match IBL.cpp DirToEquirectUV exactly. The HDRI format is Y-polar;
// the world is Z-up, so world dirs swizzle into env space first.
vec2 dirToUV(vec3 d) {
    d = normalize(d);
    d = vec3(d.x, d.z, -d.y); // world -> env
    float u = atan(d.z, d.x) / (2.0 * PI) + 0.5;
    float v = acos(clamp(d.y, -1.0, 1.0)) / PI;
    return vec2(u, v);
}

// ACES filmic tone mapping (Narkowicz fit): maps HDR [0,inf) into [0,1] with a
// filmic shoulder so highlights roll off instead of clipping to flat white.
vec3 ACESFilm(vec3 x) {
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

// Shadow factor for the primary light: 1 = fully lit, 1 - strength = fully
// shadowed. 3x3 PCF on top of the sampler's hardware 2x2 compare gives the
// soft penumbra; the normal-offset bias is applied in the vertex stage.
float primaryShadow() {
    if (fragShadowParams.w < 0.5) return 1.0;
    vec3 sc = fragShadowCoord.xyz; // ortho projection: w == 1
    if (sc.z <= 0.0 || sc.z >= 1.0) return 1.0; // outside the light's depth range
    vec2 uv = sc.xy * 0.5 + 0.5;
    float texel = fragShadowParams.z;
    float lit = 0.0;
    for (int x = -1; x <= 1; x++) {
        for (int y = -1; y <= 1; y++) {
            lit += texture(uShadowMap, vec3(uv + vec2(x, y) * texel, sc.z));
        }
    }
    return 1.0 - fragShadowParams.x * (1.0 - lit / 9.0);
}

// Enhanced PBR lighting calculation
vec3 calculateLight(Light light, vec3 normal, vec3 viewDir, vec3 albedo, float metallic, float roughness, float specularScale, float reflectance) {
    vec3 lightDir = normalize(light.direction.xyz);
    float lightIntensity = light.direction.w;
    vec3 lightColor = light.color.rgb;
    
    // Diffuse
    float NdotL = max(dot(normal, lightDir), 0.0);
    vec3 diffuse = lightColor * lightIntensity * NdotL * albedo;
    
    // Specular (Blinn-Phong)
    vec3 halfwayDir = normalize(lightDir + viewDir);
    float NdotH = max(dot(normal, halfwayDir), 0.0);
    float shininess = mix(16.0, 256.0, 1.0 - roughness);
    float spec = pow(NdotH, shininess);
    
    // Fresnel effect (dielectric F0 from the material's reflectance)
    float VdotH = max(dot(viewDir, halfwayDir), 0.0);
    vec3 F0 = mix(vec3(reflectance), albedo, metallic);
    vec3 fresnel = F0 + (1.0 - F0) * pow(1.0 - VdotH, 5.0);

    vec3 specular = lightColor * lightIntensity * spec * fresnel * specularScale;
    
    // Combine diffuse and specular based on metallicness
    return mix(diffuse, vec3(0.0), metallic) + specular;
}

void main() {
    // Section view: clip fragments past ANY enabled plane (sections
    // intersect). Distances come interpolated from the vertex stage;
    // disabled planes carry a large negative value.
    if (any(greaterThan(fragClipDist, vec4(0.0)))) {
        discard;
    }
    // Normalize vectors
    vec3 normal = normalize(fragNormal);
    vec3 viewDir = normalize(fragViewPos - fragWorldPos);
    
    // Albedo from the per-instance base color (via the vertex stage).
    vec3 albedo = fragColor;

    // Real PBR params from the assigned material (no more color guessing).
    float metallic = pc.material.x;
    float roughness = clamp(pc.material.y, 0.03, 1.0);
    float specularScale = pc.material.z;
    float reflectance = pc.material.w;

    // Procedural surface pattern (object space so it tracks the part; shared
    // with the ray tracer via grain.glsl). facing = how parallel the surface
    // is to the pattern axis — the plywood layup, or a weave's sheet normal.
    // Taken against the geometric normal, BEFORE the weave's relief bends it.
    vec3 patternAxis = grain_axisUnit(int(pc.procMaterial.w + 0.5));
    float patternFacing =
        abs(dot(normal, normalize(mat3(pc.model) * patternAxis)));
    vec3 dNormalObj;
    grain_apply(albedo, roughness, dNormalObj, pc.procMaterial, fragLocalPos,
                patternFacing);
    if (dot(dNormalObj, dNormalObj) > 0.0)
        normal = normalize(normal + mat3(pc.model) * dNormalObj);

    // Image-based ambient/environment lighting (split-sum). Replaces the flat
    // ambient term: diffuse from the irradiance map, specular from the
    // prefiltered map + BRDF LUT.
    vec3 R = reflect(-viewDir, normal);
    float NdotV = max(dot(normal, viewDir), 0.0);
    vec3 F0 = mix(vec3(reflectance), albedo, metallic);
    // Roughness-aware Fresnel (Fdez-Aguera) so grazing angles don't over-brighten.
    vec3 F = F0 + (max(vec3(1.0 - roughness), F0) - F0) * pow(1.0 - NdotV, 5.0);
    vec3 kD = (1.0 - F) * (1.0 - metallic);

    vec3 irradiance = texture(uIrradiance, dirToUV(normal)).rgb;
    vec3 diffuseIBL = irradiance * albedo;

    const float kMaxReflectionLod = 4.0; // kPrefilterMips - 1
    vec3 prefiltered =
        textureLod(uPrefilter, dirToUV(R), roughness * kMaxReflectionLod).rgb;
    vec2 envBRDF = texture(uBrdfLut, vec2(NdotV, roughness)).rg;
    // Split-sum reconstruction uses F0, NOT the grazing-boosted F: the LUT's
    // B term already integrates the Fresnel-grazing response, so multiplying
    // by F double-counted it — rough dark dielectrics (plastic_black) washed
    // out to near-palette grey from the inflated, albedo-independent env
    // specular (S3 dark-dielectric fix). F stays correct for the kD split.
    vec3 specularIBL = prefiltered * (F0 * envBRDF.x + envBRDF.y);

    // SSAO attenuates only the ambient/IBL terms — direct light occlusion is
    // the shadow map's job. gl_FragCoord * (1/framebufferSize) lands on the
    // matching texel of the (normalized-uv) AO target; the MSAA fragments of
    // one pixel share the single 1x AO value (accepted by design).
    float ao = 1.0;
    if (fragAOParams2.x > 0.5) {
        // Map the fragment through the 3D VIEWPORT rect, not the framebuffer:
        // the scene occupies a sub-rect (panels take the rest) while the SSAO
        // prepass fills its whole target, so a plain gl_FragCoord/framebuffer
        // divide sampled the wrong texel and ghosted nearby geometry onto
        // flat faces (visible as rails smeared across a deck).
        vec2 aoUV = gl_FragCoord.xy * fragAOParams.xy + fragAOParams.zw;
        ao = texture(uAO, aoUV).r;
    }
    // Specular occlusion (Lagarde/Frostbite): rough surfaces lose ambient
    // specular in occluded areas faster than smooth ones; identity when
    // ao == 1, so AO-off behavior is unchanged.
    float so = clamp(pow(NdotV + ao, exp2(-16.0 * roughness - 1.0)) - 1.0 + ao,
                     0.0, 1.0);

    vec3 ambient = ao * kD * diffuseIBL + so * specularIBL;

    // Direct analytic lights add crisp key/fill/rim highlights on top of IBL.
    // Dialed back since IBL now supplies the environment fill.
    vec3 primaryLighting = calculateLight(fragPrimaryLight, normal, viewDir, albedo, metallic, roughness, specularScale, reflectance);
    vec3 fillLighting = calculateLight(fragFillLight, normal, viewDir, albedo, metallic, roughness, specularScale, reflectance);
    vec3 rimLighting = calculateLight(fragRimLight, normal, viewDir, albedo, metallic, roughness, specularScale, reflectance);

    // Combine IBL ambient + direct lights (direct scaled down to avoid
    // double-lighting now that IBL carries the environment). Only the primary
    // (key) light is shadowed; fill/rim/IBL are the soft fill that keeps
    // shadows from going black.
    vec3 finalColor = ambient + 0.5 * (primaryShadow() * primaryLighting +
                                       fillLighting + rimLighting);

    // GLASS (MeshRenderer::SetInstanceGlass marks it with a NEGATIVE alpha;
    // the push constant has no room for a flag). Clear glass scatters almost
    // nothing: what a pane shows is what is BEHIND it (the blend supplies
    // that), the environment REFLECTED — rising to a mirror at grazing
    // angles (Schlick at glass's F0) — and the lights' highlights, the glare,
    // which are not see-through at all. Shading it as a lit white surface
    // at low alpha, as a plain ghost is, read every pane as a grey veil.
    // bodyAlpha keeps only a faint, unlit tint of the glass's own colour.
    float outAlpha = pc.baseColor.a;
    if (pc.baseColor.a < 0.0) {
        float tint = -pc.baseColor.a;
        const float kGlassF0 = 0.04; // ((1.52 - 1) / (1.52 + 1))^2
        float Fv = kGlassF0 + (1.0 - kGlassF0) * pow(1.0 - NdotV, 5.0);
        vec3 mirror = textureLod(uPrefilter, dirToUV(R), 0.0).rgb;
        // The glare: the KEY light's glint, as the still draws it — a mirror
        // highlight sharp enough that a flat pane shows it only where it
        // really reflects the light. Fill and rim at an ordinary shininess
        // lit the whole face of a pane evenly and read as a grey sheen.
        vec3 Lk = normalize(fragPrimaryLight.direction.xyz);
        vec3 Hk = normalize(Lk + viewDir);
        float FH = kGlassF0 +
                   (1.0 - kGlassF0) * pow(1.0 - max(dot(viewDir, Hk), 0.0), 5.0);
        vec3 glare = primaryShadow() * fragPrimaryLight.color.rgb *
                     fragPrimaryLight.direction.w *
                     pow(max(dot(normal, Hk), 0.0), 2048.0) * FH * 4.0;
        float glareLuma = dot(glare, vec3(0.2126, 0.7152, 0.0722));
        float tintAlpha = 0.35 * tint;
        outAlpha = clamp(tintAlpha + Fv + glareLuma, 0.02, 1.0);
        vec3 premultiplied =
            albedo * 0.5 * tintAlpha + mirror * Fv + glare;
        finalColor = premultiplied / outAlpha;
    }

    // ACES filmic tone mapping, then gamma to sRGB.
    finalColor = ACESFilm(finalColor);
    finalColor = pow(finalColor, vec3(1.0/2.2));
    
    // Slight brightness boost for better visibility
    finalColor *= 1.1;

    // Face highlight tint: selected (1.0) strong, hovered (0.5) subtle
    const vec3 highlightColor = vec3(1.0, 0.65, 0.15);
    finalColor = mix(finalColor, highlightColor, fragHighlight * 0.55);

    // Extra multi-select sub-draws (highlight.z sentinel): the geometry was
    // already shaded by the main draw; keep ONLY this range's tinted
    // fragments, or the redraw's plain fragments would overwrite the other
    // ranges' tint at equal depth.
    if (pc.highlight.z == 0xFFFFFFFFu && fragHighlight < 0.75)
        discard;

    // Per-instance alpha (ghosted in-context instances, A4; glass, above).
    // Opaque instances carry 1.0 and render through a blend-disabled
    // pipeline, so this only takes effect on the ghost pass.
    outColor = vec4(finalColor, outAlpha);
}