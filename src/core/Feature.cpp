#include "core/Feature.h"

#include "Logger.h"

BoxFeature::BoxFeature(const std::string& name, const glm::vec3& position,
                       double width, double height, double depth)
    : position(position), width(width), height(height), depth(depth) {
  m_name = name;
}

bool BoxFeature::Execute(std::vector<Body>& bodies) {
  CADShape shape = CADKernel::Instance().CreateBox(width, height, depth);
  if (!shape.IsValid()) {
    LOG_ERROR("BoxFeature '{}': invalid dimensions {}x{}x{}", m_name, width,
              height, depth);
    return false;
  }
  shape.Translate(position);
  bodies.push_back({m_name, std::move(shape)});
  return true;
}
