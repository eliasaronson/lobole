#version 450

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
layout(location = 12) in float fragClipDist;
layout(location = 13) in vec4 fragShadowCoord;
layout(location = 14) in vec4 fragShadowParams;
layout(location = 15) in vec4 fragAOParams;
layout(location = 16) in vec3 fragLocalPos;  // object space, for procedural grain

layout(location = 0) out vec4 outColor;

layout(push_constant) uniform PushConstants {
    mat4 model;        // world transform (also read here for the plywood axis)
    uvec4 highlight;   // used in the vertex stage
    vec4 material;      // x=metallic, y=roughness, z=specular, w=reflectance
    vec4 baseColor;     // per-instance albedo (used in the vertex stage)
    // Procedural grain: x=pattern (0 flat/1 wood/2 plywood), y=grainScale,
    // z=grainContrast, w=grain-axis index (0/1/2). See the grain helpers below.
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
// Must match IBL.cpp DirToEquirectUV exactly.
vec2 dirToUV(vec3 d) {
    d = normalize(d);
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

// ---- Procedural wood / plywood grain (pc.procMaterial) ---------------------
// Everything below is evaluated in object space (fragLocalPos) so the figure
// stays glued to the part under any model/assembly transform. No textures/UVs.
float ph13(vec3 p) {
    p = fract(p * 0.1031);
    p += dot(p, p.yzx + 33.33);
    return fract((p.x + p.y) * p.z);
}
// Trilinearly-interpolated value noise on the integer lattice.
float vnoise(vec3 p) {
    vec3 i = floor(p), f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    float n000 = ph13(i),               n100 = ph13(i + vec3(1, 0, 0));
    float n010 = ph13(i + vec3(0,1,0)), n110 = ph13(i + vec3(1, 1, 0));
    float n001 = ph13(i + vec3(0,0,1)), n101 = ph13(i + vec3(1, 0, 1));
    float n011 = ph13(i + vec3(0,1,1)), n111 = ph13(i + vec3(1, 1, 1));
    return mix(mix(mix(n000, n100, f.x), mix(n010, n110, f.x), f.y),
               mix(mix(n001, n101, f.x), mix(n011, n111, f.x), f.y), f.z);
}
float fbm(vec3 p) {
    float s = 0.0, a = 0.5;
    for (int k = 0; k < 4; k++) { s += a * vnoise(p); p *= 2.0; a *= 0.5; }
    return s;
}
// Split object-space pos into along-grain t and the cross-section plane uv.
void grainCoords(vec3 lp, int axis, out float t, out vec2 uv) {
    if (axis == 0)      { t = lp.x; uv = lp.yz; }
    else if (axis == 1) { t = lp.y; uv = lp.xz; }
    else                { t = lp.z; uv = lp.xy; }
}
// Solid-wood figure in [0,1]: 0 = light earlywood, 1 = dark latewood/grain line.
float woodFigure(vec3 lp, float scale, int axis) {
    float t; vec2 uv; grainCoords(lp, axis, t, uv);
    // Warp the rings off perfect circles and let them drift along the grain.
    float warp = (fbm(vec3(uv * 0.5, t * 0.1)) - 0.5) * 3.0;
    float rings = fract((length(uv) + warp) * scale);
    float band = smoothstep(0.5, 0.95, rings);   // one narrow dark band per ring
    float fine = fbm(vec3(uv * 24.0, t * 1.5)) - 0.5;  // close-up axial figure
    return clamp(band + 0.25 * fine, 0.0, 1.0);
}
// Plywood ply-stripe figure for cut edges: dark glue lines between light plies.
float plyFigure(float coordAlongLayup, float scale) {
    float layer = fract(coordAlongLayup * scale);
    float body = smoothstep(0.0, 0.08, layer) * (1.0 - smoothstep(0.92, 1.0, layer));
    return clamp(1.0 - body, 0.0, 1.0);          // dark at the ply boundaries
}

void main() {
    // Section view: clip fragments past the plane. The signed distance comes
    // interpolated from the vertex stage (0 everywhere when clipping is off).
    if (fragClipDist > 0.0) {
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

    // Procedural wood / plywood grain: darken albedo toward the grain lines and
    // roughen the latewood a touch, in object space so it tracks the part.
    int pattern = int(pc.procMaterial.x + 0.5);
    if (pattern > 0) {
        float scale = pc.procMaterial.y;
        float contrast = pc.procMaterial.z;
        int axis = int(pc.procMaterial.w + 0.5);
        float figure;
        if (pattern == 2) {
            // Plywood: cut edges show ply stripes, faces show a subtle veneer
            // grain — blended by how parallel the surface is to the layup axis.
            vec3 axisUnit = axis == 0 ? vec3(1, 0, 0)
                          : axis == 1 ? vec3(0, 1, 0) : vec3(0, 0, 1);
            vec3 axisWorld = normalize(mat3(pc.model) * axisUnit);
            float facing = abs(dot(normal, axisWorld));  // ~1 on veneer face
            float t; vec2 uv; grainCoords(fragLocalPos, axis, t, uv);
            float faceFig = 0.5 * woodFigure(fragLocalPos, scale * 0.15, axis);
            float edgeFig = plyFigure(t, scale);
            figure = mix(edgeFig, faceFig, smoothstep(0.35, 0.65, facing));
        } else {
            figure = woodFigure(fragLocalPos, scale, axis);
        }
        albedo *= (1.0 - contrast * figure);
        roughness = clamp(roughness + 0.12 * figure, 0.03, 1.0);
    }

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
    if (fragAOParams.z > 0.5) {
        ao = texture(uAO, gl_FragCoord.xy * fragAOParams.xy).r;
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
    
    // ACES filmic tone mapping, then gamma to sRGB.
    finalColor = ACESFilm(finalColor);
    finalColor = pow(finalColor, vec3(1.0/2.2));
    
    // Slight brightness boost for better visibility
    finalColor *= 1.1;

    // Face highlight tint: selected (1.0) strong, hovered (0.5) subtle
    const vec3 highlightColor = vec3(1.0, 0.65, 0.15);
    finalColor = mix(finalColor, highlightColor, fragHighlight * 0.55);

    // Per-instance alpha (ghosted in-context instances, A4). Opaque instances
    // carry 1.0 and render through a blend-disabled pipeline, so this only
    // takes effect on the ghost pass.
    outColor = vec4(finalColor, pc.baseColor.a);
}