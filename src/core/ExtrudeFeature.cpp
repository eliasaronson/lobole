#include "core/ExtrudeFeature.h"

#include "Logger.h"
#include "core/SketchFeature.h"
#include "core/SketchProfile.h"

#include <algorithm>
#include <cmath>

ExtrudeFeature::ExtrudeFeature(const std::string& name, uint64_t sketchId,
                               double distance, ExtrudeMode mode, bool reversed)
    : sketchId(sketchId), distance(distance), reversed(reversed), mode(mode) {
  m_name = name;
}

namespace {
// Do two axis-aligned bounding boxes overlap?
bool BoxesOverlap(const glm::vec3& minA, const glm::vec3& maxA,
                  const glm::vec3& minB, const glm::vec3& maxB) {
  return minA.x <= maxB.x && maxA.x >= minB.x && minA.y <= maxB.y &&
         maxA.y >= minB.y && minA.z <= maxB.z && maxA.z >= minB.z;
}
} // namespace

bool ExtrudeFeature::Execute(RegenContext& ctx) {
  const Feature* ref = ctx.FindById(sketchId);
  const auto* sketch = dynamic_cast<const SketchFeature*>(ref);
  if (!sketch) {
    LOG_ERROR("ExtrudeFeature '{}': sketch id {} not found earlier in history",
              m_name, sketchId);
    return false;
  }

  std::vector<sketch::Loop> loops = sketch::BuildLoops(sketch->entities);
  if (loops.empty()) {
    LOG_ERROR("ExtrudeFeature '{}': sketch has no closed profile", m_name);
    return false;
  }

  CADShape prism =
      sketch::BuildPrism(sketch->plane, loops, distance, reversed);
  if (!prism.IsValid()) {
    LOG_ERROR("ExtrudeFeature '{}': prism construction failed", m_name);
    return false;
  }

  auto& bodies = ctx.bodies;

  switch (mode) {
  case ExtrudeMode::NewBody:
    bodies.push_back({m_name, std::move(prism), m_material});
    return true;

  case ExtrudeMode::Add: {
    if (bodies.empty()) {
      LOG_WARN("ExtrudeFeature '{}': Add with no bodies, creating one", m_name);
      bodies.push_back({m_name, std::move(prism), m_material});
      return true;
    }
    // Fuse into the last body whose bbox overlaps the prism, else the last.
    glm::vec3 pmin, pmax;
    prism.GetBoundingBox(pmin, pmax);
    int target = static_cast<int>(bodies.size()) - 1;
    for (int i = static_cast<int>(bodies.size()) - 1; i >= 0; i--) {
      glm::vec3 bmin, bmax;
      bodies[i].shape.GetBoundingBox(bmin, bmax);
      if (BoxesOverlap(pmin, pmax, bmin, bmax)) {
        target = i;
        break;
      }
    }
    CADShape fused = bodies[target].shape.Union(prism);
    if (!fused.IsValid()) {
      LOG_ERROR("ExtrudeFeature '{}': fuse failed", m_name);
      return false;
    }
    bodies[target].shape = std::move(fused);
    return true;
  }

  case ExtrudeMode::Cut: {
    if (bodies.empty()) {
      LOG_ERROR("ExtrudeFeature '{}': Cut with no bodies", m_name);
      return false;
    }
    for (auto& body : bodies) {
      CADShape cut = body.shape.Difference(prism);
      if (cut.IsValid()) {
        body.shape = std::move(cut);
      }
    }
    // Drop bodies the cut fully consumed
    bodies.erase(std::remove_if(bodies.begin(), bodies.end(),
                                [](const Body& b) {
                                  return !b.shape.IsValid() ||
                                         b.shape.Volume() < 1e-9;
                                }),
                 bodies.end());
    return true;
  }
  }
  return false;
}
