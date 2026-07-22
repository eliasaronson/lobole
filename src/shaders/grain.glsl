// Procedural wood / plywood grain, shared by the raster (mesh.frag) and
// ray-traced (closesthit.rchit) shaders so both render an identical figure.
// Everything is evaluated in OBJECT (part-local) space so the grain stays glued
// to the part under any transform. No textures / UVs. Included via
// GL_GOOGLE_include_directive; grain-axis selection (proc.w) is done CPU-side.
#ifndef GRAIN_GLSL
#define GRAIN_GLSL

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
// Split object-space pos into along-grain t and the cross-section plane uv.
void grain_coords(vec3 lp, int axis, out float t, out vec2 uv) {
    if (axis == 0)      { t = lp.x; uv = lp.yz; }
    else if (axis == 1) { t = lp.y; uv = lp.xz; }
    else                { t = lp.z; uv = lp.xy; }
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
// Figure in [0,1] for the proc params (x = pattern, y = scale, z = contrast,
// w = grain-axis index). `facing` = |dot(surfaceNormal, layup axis)|, used only
// for plywood; each shader computes it in whatever frame it has (object or
// world — equal for rigid transforms).
float grain_figure(vec4 proc, vec3 lp, float facing) {
    int pattern = int(proc.x + 0.5);
    float scale = proc.y;
    int axis = int(proc.w + 0.5);
    if (pattern == 2) {
        // Plywood: cut edges show ply stripes, faces show a subtle veneer grain,
        // blended by how parallel the surface is to the layup axis.
        float t; vec2 uv; grain_coords(lp, axis, t, uv);
        float faceFig = 0.5 * grain_woodFigure(lp, scale * 0.15, axis);
        float edgeFig = grain_plyFigure(t, scale);
        return mix(edgeFig, faceFig, smoothstep(0.35, 0.65, facing));
    }
    return grain_woodFigure(lp, scale, axis);
}
// Darken albedo toward the grain lines and roughen the latewood a touch.
// No-op when pattern == 0 (every non-wood material, and MDF).
void grain_apply(inout vec3 albedo, inout float roughness, vec4 proc,
                 vec3 lp, float facing) {
    if (proc.x < 0.5) return;
    float figure = grain_figure(proc, lp, facing);
    albedo *= (1.0 - proc.z * figure);
    roughness = clamp(roughness + 0.12 * figure, 0.03, 1.0);
}

#endif  // GRAIN_GLSL
