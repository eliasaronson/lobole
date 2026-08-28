#include "cad/SketchPlane.h"
#include "core/Conventions.h"
#include <array>
#include <glm/gtc/matrix_transform.hpp>

SketchPlane::SketchPlane(PlaneType type, const glm::vec3& origin)
    : m_type(type), m_origin(origin) {

  // Set up standard plane orientations
  switch (type) {
  case PlaneType::XY: // Front plane
    m_normal = glm::vec3(0, 0, 1);
    m_uAxis = glm::vec3(1, 0, 0);
    break;
  case PlaneType::XZ: // Top plane
    m_normal = glm::vec3(0, 1, 0);
    m_uAxis = glm::vec3(1, 0, 0);
    break;
  case PlaneType::YZ: // Right plane
    m_normal = glm::vec3(1, 0, 0);
    m_uAxis = glm::vec3(0, 1, 0);
    break;
  }

  UpdateAxes();
  SetNameFromType();
}

SketchPlane::SketchPlane(const glm::vec3& origin, const glm::vec3& normal,
                         const glm::vec3& uAxis)
    : m_type(PlaneType::XY), m_origin(origin), m_normal(glm::normalize(normal)),
      m_uAxis(glm::normalize(uAxis)) {

  UpdateAxes();
  m_name = "Custom Plane";
}

void SketchPlane::UpdateAxes() {
  // Ensure normal is normalized
  m_normal = glm::normalize(m_normal);

  // Make sure U axis is perpendicular to normal
  m_uAxis = glm::normalize(m_uAxis - glm::dot(m_uAxis, m_normal) * m_normal);

  // V axis is cross product of normal and U axis
  m_vAxis = glm::cross(m_normal, m_uAxis);
}

const char* SketchPlane::StandardPlaneName(PlaneType type) {
  // The name follows the world frame, not the geometry: the plane whose
  // normal is the world up axis is the one a person calls "Top", whichever
  // coordinate plane that happens to be. FeatureTreePanel lists the base
  // planes through this too, so the tree rows and the plane objects cannot
  // disagree.
  const glm::dvec3 n = type == PlaneType::XY   ? glm::dvec3(0, 0, 1)
                       : type == PlaneType::XZ ? glm::dvec3(0, 1, 0)
                                               : glm::dvec3(1, 0, 0);
  if (glm::abs(glm::dot(n, conv::kWorldUp)) > 0.9)
    return "Top Plane";
  if (glm::abs(glm::dot(n, conv::kWorldFront)) > 0.9)
    return "Front Plane";
  return "Right Plane";
}

void SketchPlane::SetNameFromType() { m_name = StandardPlaneName(m_type); }

const char* SketchPlane::BasePlaneLabel(int index) {
  // Built from StandardPlaneName so a combo row can never disagree with the
  // plane object it selects ("Top" is whichever plane the frame says).
  static const std::array<std::string, 3> kLabels = [] {
    const char* suffix[3] = {" (XY)", " (XZ)", " (YZ)"};
    std::array<std::string, 3> a;
    for (int i = 0; i < 3; i++) {
      std::string name = StandardPlaneName(static_cast<PlaneType>(i));
      a[static_cast<size_t>(i)] =
          name.substr(0, name.find(' ')) + suffix[i]; // "Top Plane" -> "Top"
    }
    return a;
  }();
  return kLabels[static_cast<size_t>(index)].c_str();
}

glm::mat4 SketchPlane::GetPlaneMatrix() const {
  // Create transformation matrix from plane coordinates to world coordinates
  glm::mat4 transform(1.0f);

  // Set the columns of the matrix
  transform[0] = glm::vec4(m_uAxis, 0.0f);  // X axis in plane coordinates
  transform[1] = glm::vec4(m_vAxis, 0.0f);  // Y axis in plane coordinates
  transform[2] = glm::vec4(m_normal, 0.0f); // Z axis in plane coordinates
  transform[3] = glm::vec4(m_origin, 1.0f); // Translation

  return transform;
}

glm::mat4 SketchPlane::GetInversePlaneMatrix() const {
  return glm::inverse(GetPlaneMatrix());
}

glm::vec2 SketchPlane::WorldToPlane(const glm::vec3& worldPos) const {
  glm::vec3 localPos = worldPos - m_origin;

  float u = glm::dot(localPos, m_uAxis);
  float v = glm::dot(localPos, m_vAxis);

  return glm::vec2(u, v);
}

glm::vec3 SketchPlane::PlaneToWorld(const glm::vec2& planePos) const {
  return m_origin + planePos.x * m_uAxis + planePos.y * m_vAxis;
}