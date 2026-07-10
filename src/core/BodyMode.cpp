#include "core/BodyMode.h"

#include "Logger.h"

#include <algorithm>

namespace {
// Do two axis-aligned bounding boxes overlap?
bool BoxesOverlap(const glm::vec3& minA, const glm::vec3& maxA,
                  const glm::vec3& minB, const glm::vec3& maxB) {
  return minA.x <= maxB.x && maxA.x >= minB.x && minA.y <= maxB.y &&
         maxA.y >= minB.y && minA.z <= maxB.z && maxA.z >= minB.z;
}
} // namespace

bool ApplyBodyMode(std::vector<Body>& bodies, CADShape shape, ExtrudeMode mode,
                   const std::string& name, const std::string& material,
                   uint64_t ownerId) {
  switch (mode) {
  case ExtrudeMode::NewBody:
    bodies.push_back({name, std::move(shape), material, ownerId});
    return true;

  case ExtrudeMode::Add: {
    if (bodies.empty()) {
      LOG_WARN("ApplyBodyMode '{}': Add with no bodies, creating one", name);
      bodies.push_back({name, std::move(shape), material, ownerId});
      return true;
    }
    // Fuse into the last body whose bbox overlaps the shape, else the last.
    glm::vec3 pmin, pmax;
    shape.GetBoundingBox(pmin, pmax);
    int target = static_cast<int>(bodies.size()) - 1;
    for (int i = static_cast<int>(bodies.size()) - 1; i >= 0; i--) {
      glm::vec3 bmin, bmax;
      bodies[i].shape.GetBoundingBox(bmin, bmax);
      if (BoxesOverlap(pmin, pmax, bmin, bmax)) {
        target = i;
        break;
      }
    }
    CADShape fused = bodies[target].shape.Union(shape);
    if (!fused.IsValid()) {
      LOG_ERROR("ApplyBodyMode '{}': fuse failed", name);
      return false;
    }
    bodies[target].shape = std::move(fused);
    return true;
  }

  case ExtrudeMode::Cut: {
    if (bodies.empty()) {
      LOG_ERROR("ApplyBodyMode '{}': Cut with no bodies", name);
      return false;
    }
    for (auto& body : bodies) {
      CADShape cut = body.shape.Difference(shape);
      if (cut.IsValid()) {
        body.shape = std::move(cut);
      }
    }
    // Drop bodies the cut fully consumed.
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
