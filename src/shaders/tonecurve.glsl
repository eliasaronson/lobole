// The display transform: scene radiance -> sRGB-encoded display value.
// The GPU copy of src/render/ToneCurve.h, which says why each step is there
// (docs/rendering/README.md#tone-mapping); the path-traced still applies the
// same curve on the CPU. Change a constant in BOTH files —
// test_tone_curve.cpp compares them by name.

const float TONE_EXPOSURE = 0.6;

const float ACES_A = 2.51;
const float ACES_B = 0.03;
const float ACES_C = 2.43;
const float ACES_D = 0.59;
const float ACES_E = 0.14;

const float SRGB_CUTOFF = 0.0031308;
const float SRGB_LINEAR_SLOPE = 12.92;
const float SRGB_SCALE = 1.055;
const float SRGB_OFFSET = 0.055;
const float SRGB_GAMMA = 2.4;

vec3 toneAcesFilm(vec3 x) {
    return clamp((x * (ACES_A * x + ACES_B)) / (x * (ACES_C * x + ACES_D) + ACES_E),
                 0.0, 1.0);
}

vec3 toneSrgbEncode(vec3 linear) {
    vec3 v = clamp(linear, 0.0, 1.0);
    vec3 lo = SRGB_LINEAR_SLOPE * v;
    vec3 hi = SRGB_SCALE * pow(v, vec3(1.0 / SRGB_GAMMA)) - SRGB_OFFSET;
    return mix(hi, lo, lessThanEqual(v, vec3(SRGB_CUTOFF)));
}

vec3 toneToDisplay(vec3 radiance) {
    return toneSrgbEncode(toneAcesFilm(radiance * TONE_EXPOSURE));
}
