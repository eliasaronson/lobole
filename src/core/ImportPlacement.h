// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// Where an Import puts what it read
// (docs/interchange/README.md#placing-an-import): scaled about the part origin,
// then turned about the world X, Y and Z axes in that order, then moved.
// Applied after the up axis and the centring, so a placement of zero is the
// import as it always stood. The model is double: every term here is.

#include <glm/glm.hpp>
#include <nlohmann/json_fwd.hpp>

#include <string>

namespace importplace {

struct Placement {
  glm::dvec3 translateMm{0.0};
  glm::dvec3 rotateDeg{0.0}; // about world X, then Y, then Z
  double scale = 1.0;

  [[nodiscard]] bool IsIdentity() const;
  // Rz · Ry · Rx.
  [[nodiscard]] glm::dmat3 Rotation() const;
  // T · R: the rigid part, after the scale.
  [[nodiscard]] glm::dmat4 Rigid() const;
  // T · R · S.
  [[nodiscard]] glm::dmat4 Matrix() const;
  // This placement followed by T(moveMm) · turn · S(factor): what a new
  // alignment of a body already standing at this placement writes.
  [[nodiscard]] Placement Then(const glm::dvec3& moveMm, const glm::dmat3& turn,
                               double factor) const;

  bool operator==(const Placement& o) const {
    return translateMm == o.translateMm && rotateDeg == o.rotateDeg &&
           scale == o.scale;
  }
};

// The angles (degrees, about X then Y then Z) of a rotation matrix: the
// inverse of Placement::Rotation, each rounded to 1e-9 degree so an exact
// quarter turn reads as 90.
[[nodiscard]] glm::dvec3 EulerDeg(const glm::dmat3& rotation);

// Reads `placement` {translate_mm?: [x, y, z], rotate_deg?: [x, y, z]} into
// `out` and `scale` (a factor > 0) into `out.scale`, each refused naming
// what it takes, `who` leading the message ("Import", "import_step").
void ReadPlacement(const nlohmann::json& placement, Placement& out,
                   const std::string& who);
void ReadScale(const nlohmann::json& scale, Placement& out,
               const std::string& who);

} // namespace importplace
