// Procedural surface figure — wood, plywood, woven composite — shared by the
// raster (mesh.frag) and ray-traced (closesthit.rchit) shaders so both render
// an identical result. Everything is evaluated in OBJECT (part-local) space so
// the figure stays glued to the part under any transform. No textures / UVs.
// Included via GL_GOOGLE_include_directive; pattern-axis selection (proc.w) is
// done CPU-side by CADMaterials::PatternAxis.
#ifndef GRAIN_GLSL
#define GRAIN_GLSL

// Mirrors CADMaterials::SurfacePattern. Patterns are functions and materials
// are parameters to them — carbon, fibreglass and Kevlar are one WEAVE at
// three scales. Never branch on a material here.
#define GRAIN_PATTERN_FLAT    0
#define GRAIN_PATTERN_WOOD    1
#define GRAIN_PATTERN_PLYWOOD 2
#define GRAIN_PATTERN_WEAVE   3

float grain_hash13(vec3 p) {
    p = fract(p * 0.1031);
    p += dot(p, p.yzx + 33.33);
    return fract((p.x + p.y) * p.z);
}
// Trilinearly-interpolated value noise on the integer lattice.
float grain_vnoise(vec3 p) {
    vec3 i = floor(p), f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    float n000 = grain_hash13(i),                n100 = grain_hash13(i + vec3(1, 0, 0));
    float n010 = grain_hash13(i + vec3(0, 1, 0)), n110 = grain_hash13(i + vec3(1, 1, 0));
    float n001 = grain_hash13(i + vec3(0, 0, 1)), n101 = grain_hash13(i + vec3(1, 0, 1));
    float n011 = grain_hash13(i + vec3(0, 1, 1)), n111 = grain_hash13(i + vec3(1, 1, 1));
    return mix(mix(mix(n000, n100, f.x), mix(n010, n110, f.x), f.y),
               mix(mix(n001, n101, f.x), mix(n011, n111, f.x), f.y), f.z);
}
float grain_fbm(vec3 p) {
    float s = 0.0, a = 0.5;
    for (int k = 0; k < 4; k++) { s += a * grain_vnoise(p); p *= 2.0; a *= 0.5; }
    return s;
}
vec3 grain_axisUnit(int axis) {
    return axis == 0 ? vec3(1, 0, 0) : axis == 1 ? vec3(0, 1, 0) : vec3(0, 0, 1);
}
// Split object-space pos into along-axis t and the cross-section plane uv.
void grain_coords(vec3 lp, int axis, out float t, out vec2 uv) {
    if (axis == 0)      { t = lp.x; uv = lp.yz; }
    else if (axis == 1) { t = lp.y; uv = lp.xz; }
    else                { t = lp.z; uv = lp.xy; }
}
// The object-space directions uv.x and uv.y run along — the exact inverse of
// grain_coords' split, so a gradient taken in uv can be pushed back into
// object space.
void grain_planeAxes(int axis, out vec3 uDir, out vec3 vDir) {
    if (axis == 0)      { uDir = vec3(0, 1, 0); vDir = vec3(0, 0, 1); }
    else if (axis == 1) { uDir = vec3(1, 0, 0); vDir = vec3(0, 0, 1); }
    else                { uDir = vec3(1, 0, 0); vDir = vec3(0, 1, 0); }
}
// Solid-wood figure in [0,1]: 0 = light earlywood, 1 = dark latewood/grain line.
float grain_woodFigure(vec3 lp, float scale, int axis) {
    float t; vec2 uv; grain_coords(lp, axis, t, uv);
    // Warp the rings off perfect circles and let them drift along the grain.
    float warp = (grain_fbm(vec3(uv * 0.5, t * 0.1)) - 0.5) * 3.0;
    float rings = fract((length(uv) + warp) * scale);
    float band = smoothstep(0.5, 0.95, rings);   // one narrow dark band per ring
    float fine = grain_fbm(vec3(uv * 24.0, t * 1.5)) - 0.5;  // close-up axial figure
    return clamp(band + 0.25 * fine, 0.0, 1.0);
}
// Plywood ply-stripe figure for cut edges: dark glue lines between light plies.
float grain_plyFigure(float coordAlongLayup, float scale) {
    float layer = fract(coordAlongLayup * scale);
    float body = smoothstep(0.0, 0.08, layer) * (1.0 - smoothstep(0.92, 1.0, layer));
    return clamp(1.0 - body, 0.0, 1.0);          // dark at the ply boundaries
}

// 2/2 twill weave. `figure` is the tow-gap darkening in [0,1]; `dNormalObj` is
// an object-space normal perturbation, which is what actually makes a weave
// read: carbon's albedo is ~0.07, so there is nothing to darken, and the whole
// look is the anisotropic sheen of light running ALONG each tow. Bending the
// normal only ACROSS a tow produces exactly that out of an isotropic BRDF.
//
// `facing` fades it out on cut edges, where a real laminate shows a resin
// stack rather than the weave.
void grain_weave(vec3 lp, float scale, int axis, float facing,
                 out float figure, out vec3 dNormalObj) {
    float t; vec2 uv; grain_coords(lp, axis, t, uv);
    vec2 c = uv * scale;
    vec2 ci = floor(c), f = fract(c);
    // Shifting the over/under by one cell per row is what reads as twill's
    // diagonal; without the shift this is a plain (checkerboard) weave.
    bool warpOnTop = int(mod(ci.x - ci.y + 4.0, 4.0)) < 2;
    // The visible tow runs along u when the warp floats, so its cross-section
    // is sampled across v — and vice versa.
    float fCross = warpOnTop ? f.y : f.x;
    vec3 uDir, vDir; grain_planeAxes(axis, uDir, vDir);
    vec3 crossDir = warpOnTop ? vDir : uDir;

    const float kPi = 3.14159265;
    float h = sin(kPi * fCross);          // rounded tow: 0 at the gaps, 1 at the crown
    float onFace = smoothstep(0.35, 0.65, facing);

    figure = (1.0 - h) * onFace;
    // d(h)/d(object distance) — chain rule through the `* scale` above.
    const float kRelief = 0.24;
    float slope = kPi * cos(kPi * fCross) * scale * kRelief;
    dNormalObj = -slope * crossDir * onFace;
}

// Figure in [0,1] for the proc params (x = pattern, y = scale, z = contrast,
// w = pattern-axis index). `facing` = |dot(surfaceNormal, pattern axis)|, used
// by plywood and weave; each shader computes it in whatever frame it has
// (object or world — equal for rigid transforms).
float grain_figure(vec4 proc, vec3 lp, float facing) {
    int pattern = int(proc.x + 0.5);
    float scale = proc.y;
    int axis = int(proc.w + 0.5);
    if (pattern == GRAIN_PATTERN_PLYWOOD) {
        // Plywood: cut edges show ply stripes, faces show a subtle veneer grain,
        // blended by how parallel the surface is to the layup axis.
        float t; vec2 uv; grain_coords(lp, axis, t, uv);
        float faceFig = 0.5 * grain_woodFigure(lp, scale * 0.15, axis);
        float edgeFig = grain_plyFigure(t, scale);
        return mix(edgeFig, faceFig, smoothstep(0.35, 0.65, facing));
    }
    if (pattern == GRAIN_PATTERN_WEAVE) {
        float figure; vec3 dN;
        grain_weave(lp, scale, axis, facing, figure, dN);
        return figure;
    }
    return grain_woodFigure(lp, scale, axis);
}

// Darken albedo toward the figure and roughen it a touch, and hand back an
// object-space normal perturbation (zero for every pattern but WEAVE) for the
// caller to rotate into its own frame. No-op when pattern is FLAT — every
// metal, plastic and ceramic, and MDF.
void grain_apply(inout vec3 albedo, inout float roughness, out vec3 dNormalObj,
                 vec4 proc, vec3 lp, float facing) {
    dNormalObj = vec3(0.0);
    if (proc.x < 0.5) return;
    int pattern = int(proc.x + 0.5);
    float figure;
    if (pattern == GRAIN_PATTERN_WEAVE) {
        grain_weave(lp, proc.y, int(proc.w + 0.5), facing, figure, dNormalObj);
        // The weave's signal is the relief, not the tint. Roughen the
        // resin-rich gaps rather than smoothing the tow crowns: smoothing
        // raised the specular everywhere and the plate came out mid-grey,
        // reading as rubber matting instead of a near-black laminate.
        albedo *= (1.0 - proc.z * figure);
        roughness = clamp(roughness + 0.08 * figure, 0.03, 1.0);
        return;
    }
    figure = grain_figure(proc, lp, facing);
    albedo *= (1.0 - proc.z * figure);
    roughness = clamp(roughness + 0.12 * figure, 0.03, 1.0);
}

#endif  // GRAIN_GLSL
