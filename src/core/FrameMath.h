#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cmath>

// One frame builder for everything that turns "an origin, a Z, and an X
// hint" into a placement: mate connector frames (AssemblySolve), the Frame
// feature, and the `frame` query. Columns are X, Y, Z, origin. X is
// re-orthogonalized against Z so the result is exactly orthonormal even when
// the hint drifted; a degenerate hint (zero, or parallel to Z) picks a
// deterministic perpendicular rather than failing, because a frame with an
// arbitrary-but-stable X is still a usable frame.
namespace frame {

inline glm::dmat4 Build(const glm::dvec3& origin, const glm::dvec3& xHint,
                        const glm::dvec3& zAxis) {
  const glm::dvec3 z = glm::normalize(zAxis);
  glm::dvec3 x = xHint - glm::dot(xHint, z) * z;
  if (glm::dot(x, x) < 1e-12) {
    x = std::abs(z.y) < 0.9 ? glm::dvec3(0, 1, 0) : glm::dvec3(1, 0, 0);
    x = x - glm::dot(x, z) * z;
  }
  x = glm::normalize(x);
  const glm::dvec3 y = glm::cross(z, x);
  glm::dmat4 m(1.0);
  m[0] = glm::dvec4(x, 0.0);
  m[1] = glm::dvec4(y, 0.0);
  m[2] = glm::dvec4(z, 0.0);
  m[3] = glm::dvec4(origin, 1.0);
  return m;
}

inline glm::dvec3 Origin(const glm::dmat4& m) { return glm::dvec3(m[3]); }
inline glm::dvec3 XAxis(const glm::dmat4& m) { return glm::dvec3(m[0]); }
inline glm::dvec3 YAxis(const glm::dmat4& m) { return glm::dvec3(m[1]); }
inline glm::dvec3 ZAxis(const glm::dmat4& m) { return glm::dvec3(m[2]); }

} // namespace frame
