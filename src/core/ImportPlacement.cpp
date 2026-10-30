// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/ImportPlacement.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <stdexcept>

namespace importplace {

namespace {

constexpr double kPi = 3.14159265358979323846;

double Rad(double deg) { return deg * kPi / 180.0; }

// glm is column-major: m[col][row].
glm::dmat3 AboutX(double a) {
  const double c = std::cos(a), s = std::sin(a);
  return glm::dmat3(1, 0, 0, 0, c, s, 0, -s, c);
}
glm::dmat3 AboutY(double a) {
  const double c = std::cos(a), s = std::sin(a);
  return glm::dmat3(c, 0, -s, 0, 1, 0, s, 0, c);
}
glm::dmat3 AboutZ(double a) {
  const double c = std::cos(a), s = std::sin(a);
  return glm::dmat3(c, s, 0, -s, c, 0, 0, 0, 1);
}

// To 1e-9 degree, and never -0.
double Clean(double deg) {
  const double r = std::round(deg * 1e9) / 1e9;
  return r == 0.0 ? 0.0 : r;
}

} // namespace

bool Placement::IsIdentity() const {
  return translateMm == glm::dvec3(0.0) && rotateDeg == glm::dvec3(0.0) &&
         scale == 1.0;
}

glm::dmat3 Placement::Rotation() const {
  return AboutZ(Rad(rotateDeg.z)) * AboutY(Rad(rotateDeg.y)) *
         AboutX(Rad(rotateDeg.x));
}

glm::dmat4 Placement::Rigid() const {
  glm::dmat4 m(Rotation());
  m[3] = glm::dvec4(translateMm, 1.0);
  return m;
}

glm::dmat4 Placement::Matrix() const {
  glm::dmat4 m(Rotation() * scale);
  m[3] = glm::dvec4(translateMm, 1.0);
  return m;
}

Placement Placement::Then(const glm::dvec3& moveMm, const glm::dmat3& turn,
                          double factor) const {
  // T(d)·Rd·S(f) · T(t)·R·S(s) = T(d + f·Rd·t) · (Rd·R) · S(f·s).
  Placement out;
  out.translateMm = moveMm + factor * (turn * translateMm);
  out.rotateDeg = EulerDeg(turn * Rotation());
  out.scale = factor * scale;
  for (int i = 0; i < 3; i++)
    out.translateMm[i] =
        std::abs(out.translateMm[i]) < 1e-12 ? 0.0 : out.translateMm[i];
  return out;
}

glm::dvec3 EulerDeg(const glm::dmat3& r) {
  // R = Rz(c)·Ry(b)·Rx(a): R20 = -sin b, R21 = cos b sin a, R22 = cos b cos
  // a, R10 = sin c cos b, R00 = cos c cos b (row, column; glm reads
  // r[column][row]).
  const double r20 = r[0][2];
  double a = 0.0, b = 0.0, c = 0.0;
  if (std::abs(r20) < 1.0 - 1e-12) {
    b = std::asin(-r20);
    a = std::atan2(r[1][2], r[2][2]);
    c = std::atan2(r[0][1], r[0][0]);
  } else {
    // Gimbal lock: X and Z turn about the same axis; all of it goes to X.
    b = r20 < 0.0 ? kPi / 2.0 : -kPi / 2.0;
    c = 0.0;
    a = std::atan2(-r[2][1], r[1][1]);
  }
  return {Clean(a * 180.0 / kPi), Clean(b * 180.0 / kPi),
          Clean(c * 180.0 / kPi)};
}

void ReadPlacement(const nlohmann::json& pl, Placement& out,
                   const std::string& who) {
  if (!pl.is_object())
    throw std::runtime_error(who +
                             ": placement is {translate_mm: [x, y, z], "
                             "rotate_deg: [x, y, z]}, not " +
                             pl.dump());
  for (const auto& [key, value] : pl.items())
    if (key != "translate_mm" && key != "rotate_deg")
      throw std::runtime_error(who + ": placement has no '" + key +
                               "'; it takes translate_mm and rotate_deg");
  const auto triple = [&](const char* key, glm::dvec3& into) {
    if (!pl.contains(key))
      return;
    const nlohmann::json& v = pl.at(key);
    bool ok = v.is_array() && v.size() == 3;
    for (size_t i = 0; ok && i < 3; i++)
      ok = v[i].is_number() && std::isfinite(v[i].get<double>());
    if (!ok)
      throw std::runtime_error(who + ": placement " + key +
                               " is [x, y, z], three finite numbers, not " +
                               v.dump());
    into = {v[0].get<double>(), v[1].get<double>(), v[2].get<double>()};
  };
  triple("translate_mm", out.translateMm);
  triple("rotate_deg", out.rotateDeg);
}

void ReadScale(const nlohmann::json& sc, Placement& out,
               const std::string& who) {
  if (!sc.is_number() || !std::isfinite(sc.get<double>()) ||
      !(sc.get<double>() > 0.0))
    throw std::runtime_error(who +
                             ": scale is a factor greater than 0 (a mirror "
                             "is not a placement), not " +
                             sc.dump());
  out.scale = sc.get<double>();
}

} // namespace importplace
