#include "core/Feature.h"

#include "common/Logger.h"
#include "core/ElementHistory.h"

const Feature* RegenContext::FindById(uint64_t id) const {
  if (id == 0)
    return nullptr;
  for (size_t i = 0; i < currentIndex && i < features.size(); i++) {
    // A suppressed feature is invisible to references — its consumers fail
    // with their normal missing-reference errors, same rule as deletion (R1).
    if (features[i]->GetId() == id)
      return features[i]->IsSuppressed() ? nullptr : features[i].get();
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
  ElementMap map;
  topo::NamePrimitiveElements(shape, std::to_string(GetId()), map);
  bodies.push_back({m_name, std::move(shape), m_material, GetId()});
  bodies.back().elementMap = std::move(map);
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
  ElementMap map;
  topo::NamePrimitiveElements(shape, std::to_string(GetId()), map);
  bodies.push_back({m_name, std::move(shape), m_material, GetId()});
  bodies.back().elementMap = std::move(map);
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
  ElementMap map;
  topo::NamePrimitiveElements(shape, std::to_string(GetId()), map);
  bodies.push_back({m_name, std::move(shape), m_material, GetId()});
  bodies.back().elementMap = std::move(map);
  return true;
}

ImportFeature::ImportFeature(const std::string& name, const std::string& path)
    : path(path) {
  m_name = name;
}

bool ImportFeature::Execute(RegenContext& ctx) {
  auto& bodies = ctx.bodies;
  // Structured (XCAF) read: geometry plus the names/colors STEP carries.
  // Each instance is a body — a flat part file is identity instances; an
  // assembly file imported into a PART places every occurrence (the
  // assembly-document import is a separate app-level flow, I2).
  CADKernel::ImportedModel model;
  const auto hasExt = [&](const char* ext) {
    const size_t n = std::strlen(ext);
    return path.size() > n && path.rfind(ext) == path.size() - n;
  };
  if (hasExt(".stl")) {
    // Triangle mesh → B-Rep: one planar face per triangle, sewn; a closed
    // mesh arrives as a solid (LoadSTL), an open one as a surface shell.
    // Fine for print round-trips and reference geometry; organic meshes get
    // heavy — every triangle is a face.
    CADShape shape;
    if (!CADKernel::Instance().LoadSTL(path, shape)) {
      SetError("failed to read '" + path + "' as STL");
      LOG_ERROR("ImportFeature '{}': failed to load '{}'", m_name, path);
      return false;
    }
    model.products.push_back({std::move(shape), "", false, glm::vec3(0.0f)});
    model.instances.push_back({0, glm::mat4(1.0f), ""});
  } else if (hasExt(".brep")) {
    // Native BREP sidecar (assembly import extracts per-part geometry as
    // .brep, I2): one anonymous product at identity.
    CADShape shape;
    if (!CADKernel::Instance().LoadBREP(path, shape)) {
      SetError("failed to read '" + path + "' as BREP");
      LOG_ERROR("ImportFeature '{}': failed to load '{}'", m_name, path);
      return false;
    }
    model.products.push_back({std::move(shape), "", false, glm::vec3(0.0f)});
    model.instances.push_back({0, glm::mat4(1.0f), ""});
  } else if (!CADKernel::Instance().LoadSTEPStructured(path, model)) {
    SetError("failed to read '" + path +
             "' as STEP (expected .step/.stp, .brep, or .stl)");
    LOG_ERROR("ImportFeature '{}': failed to load '{}'", m_name, path);
    return false;
  }

  // Placed shapes first, so centering covers the assembled positions.
  struct Placed {
    CADShape shape;
    size_t product;
    std::string name;
  };
  std::vector<Placed> placed;
  for (const auto& inst : model.instances) {
    const CADShape& src = model.products[inst.product].shape;
    if (!src.IsValid())
      continue;
    CADShape moved = src.Transformed(inst.transform);
    if (!moved.IsValid() || moved.Volume() <= 1e-6)
      continue;
    placed.push_back({std::move(moved), inst.product, inst.name});
  }

  // Combined bounding box over valid shapes, so the whole import is centered
  // as one unit and shapes keep their relative placement.
  glm::vec3 combinedMin(0.0f), combinedMax(0.0f);
  bool haveBounds = false;
  for (const auto& p : placed) {
    glm::vec3 min, max;
    p.shape.GetBoundingBox(min, max);
    combinedMin = haveBounds ? glm::min(combinedMin, min) : min;
    combinedMax = haveBounds ? glm::max(combinedMax, max) : max;
    haveBounds = true;
  }
  if (!haveBounds) {
    LOG_ERROR("ImportFeature '{}': no valid shapes in '{}'", m_name, path);
    return false;
  }
  const glm::vec3 center = (combinedMin + combinedMax) * 0.5f;

  // Real-world assembly STEPs run to hundreds of instances (the Waveshare
  // adapter test file has 143); the cap only guards against pathological
  // files, so it must sit above real ones.
  constexpr size_t kMaxShapes = 500;
  if (placed.size() > kMaxShapes) {
    LOG_WARN("ImportFeature '{}': '{}' has {} shapes, importing first {}",
             m_name, path, placed.size(), kMaxShapes);
  }

  size_t added = 0;
  for (auto& p : placed) {
    if (added >= kMaxShapes)
      break;
    p.shape.Translate(-center);
    added++;
    // All import bodies share createdBy, so the prefix carries the body
    // ordinal — otherwise two bodies' identical face names would resolve
    // against whichever body is found first.
    ElementMap map;
    topo::NamePrimitiveElements(
        p.shape, std::to_string(GetId()) + "/b" + std::to_string(added), map);
    // The PRODUCT name labels the body ("Widget", not the writer's
    // auto-generated instance name "Widget:1"); instance names matter for
    // assembly occurrences (I2), not part bodies.
    const auto& product = model.products[p.product];
    const std::string bodyName =
        !product.name.empty()
            ? product.name
            : (!p.name.empty() ? p.name : m_name + " " + std::to_string(added));
    bodies.push_back({bodyName, std::move(p.shape), m_material, GetId()});
    bodies.back().elementMap = std::move(map);
    if (m_material.empty()) {
      if (product.hasColor) {
        bodies.back().hasDisplayColor = true;
        bodies.back().displayColor = product.color;
      } else if (hasColor) { // feature-level color (assembly-import sidecars)
        bodies.back().hasDisplayColor = true;
        bodies.back().displayColor = color;
      }
    }
  }
  return true;
}
