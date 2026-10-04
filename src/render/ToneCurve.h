#pragma once

// The display transform: scene radiance (linear, as the lights and the
// materials' LINEAR base colours produce it) -> an 8-bit sRGB value. ONE
// curve for every picture the app makes of a model — the viewport and the
// quick still (src/shaders/mesh.frag) and the path-traced still
// (RayTracingRenderer::ReadbackStill) — so a colour reads the same in all
// three (docs/rendering/README.md#tone-mapping).
//
//   exposure 0.6 -> ACES filmic, Narkowicz's fit -> the sRGB transfer function
//
// The 0.6 is not a taste knob: Narkowicz fitted his curve to the ACES RRT+ODT
// AFTER a 0.6 exposure, and without it the fit is ~1.7x too bright through the
// mid-tones. The encode is sRGB's piecewise curve, which is what an 8-bit PNG
// and a UNORM swapchain under SRGB_NONLINEAR are read as — not pow(1/2.2),
// which lifts the darks. Brightness is the lights' job (conv::kKeyLight & co.),
// never a lift after the encode.
//
// The GPU copy is src/shaders/tonecurve.glsl. GLSL cannot include this file,
// so its constants are written out twice and test_tone_curve.cpp holds them
// to each other through kShaderConstants.

#include <algorithm>
#include <cmath>
#include <cstdint>

#include <glm/glm.hpp>

namespace tone {

inline constexpr float kExposure = 0.6f;

// Narkowicz 2015, "ACES Filmic Tone Mapping Curve":
//   (x (a x + b)) / (x (c x + d) + e)
inline constexpr float kAcesA = 2.51f;
inline constexpr float kAcesB = 0.03f;
inline constexpr float kAcesC = 2.43f;
inline constexpr float kAcesD = 0.59f;
inline constexpr float kAcesE = 0.14f;

// IEC 61966-2-1 (sRGB) encode.
inline constexpr float kSrgbCutoff = 0.0031308f;
inline constexpr float kSrgbLinearSlope = 12.92f;
inline constexpr float kSrgbScale = 1.055f;
inline constexpr float kSrgbOffset = 0.055f;
inline constexpr float kSrgbGamma = 2.4f;

// Each constant under the name tonecurve.glsl gives it.
struct ShaderConstant {
  const char* name;
  float value;
};
inline constexpr ShaderConstant kShaderConstants[] = {
    {"TONE_EXPOSURE", kExposure}, {"ACES_A", kAcesA},
    {"ACES_B", kAcesB},           {"ACES_C", kAcesC},
    {"ACES_D", kAcesD},           {"ACES_E", kAcesE},
    {"SRGB_CUTOFF", kSrgbCutoff}, {"SRGB_LINEAR_SLOPE", kSrgbLinearSlope},
    {"SRGB_SCALE", kSrgbScale},   {"SRGB_OFFSET", kSrgbOffset},
    {"SRGB_GAMMA", kSrgbGamma},
};

inline float AcesFilm(float x) {
  const float v =
      (x * (kAcesA * x + kAcesB)) / (x * (kAcesC * x + kAcesD) + kAcesE);
  return std::clamp(v, 0.0f, 1.0f);
}

inline float SrgbEncode(float linear) {
  const float v = std::clamp(linear, 0.0f, 1.0f);
  return v <= kSrgbCutoff
             ? kSrgbLinearSlope * v
             : kSrgbScale * std::pow(v, 1.0f / kSrgbGamma) - kSrgbOffset;
}

// Radiance -> display value in [0,1].
inline float ToDisplay(float radiance) {
  return SrgbEncode(AcesFilm(radiance * kExposure));
}
inline glm::vec3 ToDisplay(const glm::vec3& radiance) {
  return {ToDisplay(radiance.r), ToDisplay(radiance.g), ToDisplay(radiance.b)};
}

// A display value in [0,1] as the byte a PNG stores.
inline uint8_t ToByte(float display) {
  return static_cast<uint8_t>(std::clamp(display, 0.0f, 1.0f) * 255.0f + 0.5f);
}

} // namespace tone
