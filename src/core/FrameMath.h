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

inline glm::mat4 Build(const glm::vec3& origin, const glm::vec3& xHint,
                       const glm::vec3& zAxis) {
  const glm::vec3 z = glm::normalize(zAxis);
  glm::vec3 x = xHint - glm::dot(xHint, z) * z;
  if (glm::dot(x, x) < 1e-12f) {
    x = std::abs(z.y) < 0.9f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
    x = x - glm::dot(x, z) * z;
  }
  x = glm::normalize(x);
  const glm::vec3 y = glm::cross(z, x);
  glm::mat4 m(1.0f);
  m[0] = glm::vec4(x, 0.0f);
  m[1] = glm::vec4(y, 0.0f);
  m[2] = glm::vec4(z, 0.0f);
  m[3] = glm::vec4(origin, 1.0f);
  return m;
}

inline glm::vec3 Origin(const glm::mat4& m) { return glm::vec3(m[3]); }
inline glm::vec3 XAxis(const glm::mat4& m) { return glm::vec3(m[0]); }
inline glm::vec3 YAxis(const glm::mat4& m) { return glm::vec3(m[1]); }
inline glm::vec3 ZAxis(const glm::mat4& m) { return glm::vec3(m[2]); }

} // namespace frame
