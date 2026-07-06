#include "core/Feature.h"

#include "Logger.h"

const Feature* RegenContext::FindById(uint64_t id) const {
  if (id == 0)
    return nullptr;
  for (size_t i = 0; i < currentIndex && i < features.size(); i++) {
    if (features[i]->GetId() == id)
      return features[i].get();
  }
  return nullptr;
}

BoxFeature::BoxFeature(const std::string& name, const glm::vec3& position,
                       double width, double height, double depth)
    : position(position), width(width), height(height), depth(depth) {
  m_name = name;
}

bool BoxFeature::Execute(RegenContext& ctx) {
  auto& bodies = ctx.bodies;
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

SphereFeature::SphereFeature(const std::string& name, const glm::vec3& center,
                             double radius)
    : center(center), radius(radius) {
  m_name = name;
}

bool SphereFeature::Execute(RegenContext& ctx) {
  auto& bodies = ctx.bodies;
  CADShape shape = CADKernel::Instance().CreateSphere(center, radius);
  if (!shape.IsValid()) {
    LOG_ERROR("SphereFeature '{}': invalid radius {}", m_name, radius);
    return false;
  }
  bodies.push_back({m_name, std::move(shape)});
  return true;
}

CylinderFeature::CylinderFeature(const std::string& name,
                                 const glm::vec3& center,
                                 const glm::vec3& direction, double radius,
                                 double height)
    : center(center), direction(direction), radius(radius), height(height) {
  m_name = name;
}

bool CylinderFeature::Execute(RegenContext& ctx) {
  auto& bodies = ctx.bodies;
  CADShape shape =
      CADKernel::Instance().CreateCylinder(center, direction, radius, height);
  if (!shape.IsValid()) {
    LOG_ERROR("CylinderFeature '{}': invalid radius {} / height {}", m_name,
              radius, height);
    return false;
  }
  bodies.push_back({m_name, std::move(shape)});
  return true;
}

ImportFeature::ImportFeature(const std::string& name, const std::string& path)
    : path(path) {
  m_name = name;
}

bool ImportFeature::Execute(RegenContext& ctx) {
  auto& bodies = ctx.bodies;
  std::vector<CADShape> shapes;
  if (!CADKernel::Instance().LoadSTEP(path, shapes)) {
    LOG_ERROR("ImportFeature '{}': failed to load '{}'", m_name, path);
    return false;
  }

  // Combined bounding box over valid shapes, so the whole import is centered
  // as one unit and shapes keep their relative placement.
  glm::vec3 combinedMin(0.0f), combinedMax(0.0f);
  bool haveBounds = false;
  for (const auto& shape : shapes) {
    if (!shape.IsValid() || shape.Volume() <= 1e-6)
      continue;
    glm::vec3 min, max;
    shape.GetBoundingBox(min, max);
    combinedMin = haveBounds ? glm::min(combinedMin, min) : min;
    combinedMax = haveBounds ? glm::max(combinedMax, max) : max;
    haveBounds = true;
  }
  if (!haveBounds) {
    LOG_ERROR("ImportFeature '{}': no valid shapes in '{}'", m_name, path);
    return false;
  }
  const glm::vec3 center = (combinedMin + combinedMax) * 0.5f;

  constexpr size_t kMaxShapes = 20;
  if (shapes.size() > kMaxShapes) {
    LOG_WARN("ImportFeature '{}': '{}' has {} shapes, importing first {}",
             m_name, path, shapes.size(), kMaxShapes);
  }

  size_t added = 0;
  for (auto& shape : shapes) {
    if (added >= kMaxShapes)
      break;
    if (!shape.IsValid() || shape.Volume() <= 1e-6)
      continue;
    shape.Translate(-center);
    added++;
    bodies.push_back(
        {m_name + " " + std::to_string(added), std::move(shape)});
  }
  return true;
}
