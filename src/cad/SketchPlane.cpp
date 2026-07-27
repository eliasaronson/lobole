#include "cad/SketchPlane.h"
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

void SketchPlane::SetNameFromType() {
  switch (m_type) {
  case PlaneType::XY:
    m_name = "Front Plane";
    break;
  case PlaneType::XZ:
    m_name = "Top Plane";
    break;
  case PlaneType::YZ:
    m_name = "Right Plane";
    break;
  }
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