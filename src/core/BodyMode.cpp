#include "core/BodyMode.h"

#include "common/Logger.h"
#include "core/ElementHistory.h"

#include <algorithm>
#include <cmath>

namespace {
// Do two axis-aligned bounding boxes overlap?
bool BoxesOverlap(const glm::vec3& minA, const glm::vec3& maxA,
                  const glm::vec3& minB, const glm::vec3& maxB) {
  return minA.x <= maxB.x && maxA.x >= minB.x && minA.y <= maxB.y &&
         maxA.y >= minB.y && minA.z <= maxB.z && maxA.z >= minB.z;
}
} // namespace

namespace {
// Two volumes that agree to a relative tolerance — the boolean did nothing.
bool VolumeUnchanged(double before, double after) {
  return before > 1e-9 &&
         std::abs(before - after) < 1e-6 * std::max(before, 1.0);
}

// Route a failure reason to the caller when it wants one, else to the log
// (Document logs the caller-surfaced text again, so never both).
void Fail(std::string* error, std::string message) {
  if (error)
    *error = std::move(message);
  else
    LOG_ERROR("{}", message);
}
} // namespace

bool ApplyBodyMode(std::vector<Body>& bodies, CADShape shape,
                   ElementMap shapeMap, ExtrudeMode mode,
                   const std::string& name, const std::string& material,
                   uint64_t ownerId, std::string* warning, std::string* error) {
  switch (mode) {
  case ExtrudeMode::NewBody:
    bodies.push_back({name, std::move(shape), material, ownerId});
    bodies.back().elementMap = std::move(shapeMap);
    return true;

  case ExtrudeMode::Add: {
    // Fuse into the last body whose bbox overlaps the shape, else the last.
    // A reference body (a Derived ghost) is never a target: the first solid
    // drawn on a derived face becomes this part's own body, which is the
    // whole point of drawing there.
    glm::vec3 pmin, pmax;
    shape.GetBoundingBox(pmin, pmax);
    int target = -1;
    for (int i = static_cast<int>(bodies.size()) - 1; i >= 0; i--) {
      if (bodies[i].reference)
        continue;
      if (target < 0)
        target = i; // the last non-reference body, unless one overlaps
      glm::vec3 bmin, bmax;
      bodies[i].shape.GetBoundingBox(bmin, bmax);
      if (BoxesOverlap(pmin, pmax, bmin, bmax)) {
        target = i;
        break;
      }
    }
    if (target < 0) {
      if (!bodies.empty())
        LOG_INFO("ApplyBodyMode '{}': Add with only reference bodies, "
                 "creating one",
                 name);
      else
        LOG_WARN("ApplyBodyMode '{}': Add with no bodies, creating one", name);
      bodies.push_back({name, std::move(shape), material, ownerId});
      bodies.back().elementMap = std::move(shapeMap);
      return true;
    }
    ElementMap fusedMap;
    std::string boolErr;
    CADShape fused = topo::BooleanNamed(BooleanOp::Union, bodies[target].shape,
                                        bodies[target].elementMap, shape,
                                        shapeMap, ownerId, fusedMap, &boolErr);
    if (!fused.IsValid()) {
      Fail(error, "add failed to fuse with body '" + bodies[target].name + "'" +
                      (boolErr.empty() ? "" : ": " + boolErr));
      return false;
    }
    if (warning &&
        VolumeUnchanged(bodies[target].shape.Volume(), fused.Volume())) {
      *warning = "add merged no new material — the tool lies entirely inside "
                 "the target body (check the sketch plane and profile)";
    }
    bodies[target].shape = std::move(fused);
    bodies[target].elementMap = std::move(fusedMap); // propagated (T5)
    return true;
  }

  case ExtrudeMode::Cut: {
    // Reference bodies are not matter: a cut passes through a Derived ghost
    // as if it were not there, and a part that has only ghosts has nothing
    // to cut into.
    const bool anySolid =
        std::any_of(bodies.begin(), bodies.end(),
                    [](const Body& b) { return !b.reference; });
    if (!anySolid) {
      Fail(error, bodies.empty()
                      ? "cut with no bodies — nothing exists yet to cut into "
                        "(did an earlier feature fail?)"
                      : "cut with only reference bodies — a derived ghost is "
                        "not cut into (set reference: false to make it this "
                        "part's body)");
      return false;
    }
    double volBefore = 0.0;
    for (const auto& body : bodies) {
      if (!body.reference)
        volBefore += body.shape.Volume();
    }
    for (auto& body : bodies) {
      if (body.reference)
        continue;
      ElementMap cutMap;
      CADShape cut =
          topo::BooleanNamed(BooleanOp::Subtract, body.shape, body.elementMap,
                             shape, shapeMap, ownerId, cutMap);
      if (cut.IsValid()) {
        body.shape = std::move(cut);
        body.elementMap = std::move(cutMap); // propagated (T5)
      }
    }
    // Drop bodies the cut fully consumed.
    bodies.erase(std::remove_if(bodies.begin(), bodies.end(),
                                [](const Body& b) {
                                  return !b.shape.IsValid() ||
                                         b.shape.Volume() < 1e-9;
                                }),
                 bodies.end());
    double volAfter = 0.0;
    for (const auto& body : bodies) {
      if (!body.reference)
        volAfter += body.shape.Volume();
    }
    if (warning && VolumeUnchanged(volBefore, volAfter)) {
      *warning = "cut removed no material — the tool profile does not "
                 "intersect any body (check the sketch plane orientation and "
                 "position)";
    }
    return true;
  }
  }
  return false;
}

bool ApplyBodyModeReporting(std::vector<Body>& bodies, CADShape shape,
                            ElementMap shapeMap, ExtrudeMode mode,
                            Feature& owner) {
  std::string warning, error;
  const bool ok = ApplyBodyMode(bodies, std::move(shape), std::move(shapeMap),
                                mode, owner.GetName(), owner.GetMaterial(),
                                owner.GetId(), &warning, &error);
  if (!warning.empty()) {
    owner.SetWarning(warning);
  }
  if (!ok) {
    owner.SetError(error.empty() ? "body operation failed" : error);
  }
  return ok;
}

bool FuseInstancesReplacingSource(std::vector<Body>& bodies, uint64_t sourceId,
                                  std::vector<CADShape> instanceShapes,
                                  const std::string& name,
                                  const std::string& material, uint64_t ownerId,
                                  std::string* error) {
  if (instanceShapes.empty()) {
    Fail(error, "pattern produced no instances");
    return false;
  }
  const bool hasSource =
      std::any_of(bodies.begin(), bodies.end(),
                  [&](const Body& b) { return b.createdBy == sourceId; });
  if (!hasSource) {
    Fail(error, "source body (feature " + std::to_string(sourceId) +
                    ") not found — did the source feature fail?");
    return false;
  }

  // Fuse all instances, then split back into connected solids.
  CADShape fused = std::move(instanceShapes[0]);
  for (size_t i = 1; i < instanceShapes.size(); i++) {
    CADShape next = fused.Union(instanceShapes[i]);
    if (!next.IsValid()) {
      Fail(error,
           "fusing pattern instance " + std::to_string(i + 1) + " failed");
      return false;
    }
    fused = std::move(next);
  }
  std::vector<CADShape> solids = fused.SplitSolids();

  // Replace the seed body(ies) with the pattern's solids.
  bodies.erase(
      std::remove_if(bodies.begin(), bodies.end(),
                     [&](const Body& b) { return b.createdBy == sourceId; }),
      bodies.end());
  for (size_t i = 0; i < solids.size(); i++) {
    std::string bodyName =
        solids.size() > 1 ? name + " " + std::to_string(i + 1) : name;
    bodies.push_back(
        {std::move(bodyName), std::move(solids[i]), material, ownerId});
  }
  return true;
}
