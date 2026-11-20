// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/fem/FemFields.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace fem {

const std::vector<ResultFieldSpec>& ResultFieldSpecs() {
  static const std::vector<ResultFieldSpec> k = {
      {ResultField::VonMises, "von_mises", "von Mises", "MPa", false, false},
      {ResultField::Displacement, "displacement", "displacement", "mm", false,
       false},
      {ResultField::PrincipalMax, "principal_max", "max principal", "MPa",
       false, true},
      {ResultField::PrincipalMid, "principal_mid", "mid principal", "MPa",
       false, true},
      {ResultField::PrincipalMin, "principal_min", "min principal", "MPa",
       false, true},
      {ResultField::AcrossLayers, "across_layers", "across layers", "MPa",
       false, true},
      {ResultField::InterlayerFactor, "interlayer_factor", "interlayer factor",
       "", true, false},
      {ResultField::FactorOfSafety, "factor_of_safety", "factor of safety", "",
       true, false},
      {ResultField::PlasticStrain, "plastic_strain", "plastic strain", "",
       false, false},
      {ResultField::ContactPressure, "contact_pressure", "contact pressure",
       "MPa", false, false},
      {ResultField::Temperature, "temperature", "temperature",
       "\xC2\xB0"
       "C",
       false, true},
      {ResultField::HeatFlux, "heat_flux", "heat flux", "W/m\xC2\xB2", false,
       false},
  };
  return k;
}

const ResultFieldSpec& SpecOf(ResultField f) {
  for (const ResultFieldSpec& s : ResultFieldSpecs())
    if (s.field == f)
      return s;
  return ResultFieldSpecs().front();
}

std::optional<ResultField> ResultFieldFromKey(const std::string& key) {
  for (const ResultFieldSpec& s : ResultFieldSpecs())
    if (key == s.key)
      return s.field;
  return std::nullopt;
}

std::string ResultFieldKeys() {
  std::string out;
  for (const ResultFieldSpec& s : ResultFieldSpecs())
    out += (out.empty() ? "" : " | ") + std::string(s.key);
  return out;
}

std::array<double, 3> Principals(const Stress& s) {
  // The roots of the characteristic cubic of a symmetric 3x3 (Smith 1961),
  // as MaxPrincipal takes its largest.
  const double xx = s[0], yy = s[1], zz = s[2], xy = s[3], yz = s[4], zx = s[5];
  const double off = xy * xy + yz * yz + zx * zx;
  const double q = (xx + yy + zz) / 3.0;
  const double a = xx - q, b = yy - q, c = zz - q;
  const double p2 = a * a + b * b + c * c + 2.0 * off;
  if (p2 <= 1e-30 * (q * q + 1e-300))
    return {q, q, q};
  const double p = std::sqrt(p2 / 6.0);
  const double det =
      a * (b * c - yz * yz) - xy * (xy * c - yz * zx) + zx * (xy * yz - b * zx);
  const double r = std::clamp(det / (2.0 * p * p * p), -1.0, 1.0);
  const double phi = std::acos(r) / 3.0;
  const double hi = q + 2.0 * p * std::cos(phi);
  const double lo = q + 2.0 * p * std::cos(phi + 2.0 * std::numbers::pi / 3.0);
  return {hi, 3.0 * q - hi - lo, lo};
}

std::vector<double> NodalField(const FemResults& r, ResultField f,
                               const FieldInputs& in, std::string* why) {
  const auto none = [&](const char* m) {
    if (why)
      *why = m;
    return std::vector<double>{};
  };
  const auto needStress = [&]() { return !r.stress.empty(); };
  static const char* kNoStress =
      "the result keeps no stress tensor (a modal or thermal result, or one "
      "saved before v172)";
  std::vector<double> out;
  switch (f) {
  case ResultField::VonMises:
    if (r.vonMises.empty())
      return none("the result has no stress (a modal or thermal result)");
    return r.vonMises;
  case ResultField::Displacement:
    if (r.displacement.empty())
      return none("the result has no displacement");
    out.reserve(r.displacement.size());
    for (const glm::dvec3& u : r.displacement)
      out.push_back(glm::length(u));
    return out;
  case ResultField::PrincipalMax:
  case ResultField::PrincipalMid:
  case ResultField::PrincipalMin: {
    if (!needStress())
      return none(kNoStress);
    const int k = f == ResultField::PrincipalMax   ? 0
                  : f == ResultField::PrincipalMid ? 1
                                                   : 2;
    out.reserve(r.stress.size());
    for (const Stress& s : r.stress)
      out.push_back(Principals(s)[k]);
    return out;
  }
  case ResultField::AcrossLayers:
  case ResultField::InterlayerFactor: {
    if (!needStress())
      return none(kNoStress);
    if (!(glm::dot(in.layerNormal, in.layerNormal) > 0.0))
      return none("it needs layer_normal, the build direction the layers "
                  "are stacked along");
    out = AcrossLayersField(r, in.layerNormal);
    if (f == ResultField::AcrossLayers)
      return out;
    if (!(in.interlayerMPa > 0.0))
      return none("the material has no interlayer strength (pla and petg "
                  "do)");
    for (double& v : out)
      v = v > 0.0 ? in.interlayerMPa / v : kUnboundedFactor;
    return out;
  }
  case ResultField::FactorOfSafety:
    if (r.vonMises.empty())
      return none("the result has no stress (a modal or thermal result)");
    // The solve's own: each material against its own yield where several
    // meet (docs/simulation/README.md#stress-where-materials-meet), so the
    // field's least value is FemResults::factorOfSafety.
    if (!r.nodeFactorOfSafety.empty() &&
        r.nodeFactorOfSafety.size() == r.vonMises.size())
      return r.nodeFactorOfSafety;
    if (in.severalMaterials)
      return none("a bonded group's factor field is its run's, and this "
                  "result was read from disk (it keeps the least factor, "
                  "which study_results reports): run the study again "
                  "(run_study {recompute: true}) to draw it");
    if (!(in.yieldMPa > 0.0))
      return none("the material has no yield strength");
    out.reserve(r.vonMises.size());
    for (double vm : r.vonMises)
      out.push_back(vm > 0.0 ? in.yieldMPa / vm : kUnboundedFactor);
    return out;
  case ResultField::PlasticStrain:
    if (r.plasticStrain.empty())
      return none("the result has no plastic strain (a static study with "
                  "nonlinear plasticity has)");
    return r.plasticStrain;
  case ResultField::ContactPressure:
    if (r.contactPressure.empty())
      return none("the result has no contact pressure (a study with a "
                  "contact, run in this session, has: a stored result keeps "
                  "each contact's peak, not the field)");
    return r.contactPressure;
  case ResultField::Temperature:
    if (r.temperature.empty())
      return none("the result has no temperature (a thermal study, or a "
                  "static study under a thermal load, has)");
    return r.temperature;
  case ResultField::HeatFlux:
    if (r.heatFlux.empty())
      return none("the result has no heat flux (a thermal study has)");
    out.reserve(r.heatFlux.size());
    for (const glm::dvec3& q : r.heatFlux)
      out.push_back(glm::length(q));
    return out;
  }
  return out;
}

FieldRange RangeOf(const std::vector<double>& field) {
  FieldRange r;
  for (size_t n = 0; n < field.size(); ++n) {
    const double v = field[n];
    if (!std::isfinite(v) || v >= kUnboundedFactor)
      continue;
    if (r.loNode < 0 || v < r.lo) {
      r.lo = v;
      r.loNode = static_cast<int>(n);
    }
    if (r.hiNode < 0 || v > r.hi) {
      r.hi = v;
      r.hiNode = static_cast<int>(n);
    }
  }
  return r;
}

} // namespace fem
