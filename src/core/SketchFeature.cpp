#include "core/SketchFeature.h"

#include <limits>
#include <map>
#include <vector>

SketchFeature::SketchFeature(const std::string& name, const SketchPlane& p)
    : plane(p) {
  m_name = name;
}

uint32_t SketchFeature::AddEntity(sketch::Entity e) {
  const uint32_t id = nextEntityId++;
  sketch::SetEntityId(e, id);
  entities.push_back(std::move(e));
  return id;
}

size_t SketchFeature::IndexOfEntity(uint32_t id) const {
  if (id == 0)
    return std::numeric_limits<size_t>::max();
  for (size_t i = 0; i < entities.size(); i++)
    if (sketch::EntityId(entities[i]) == id)
      return i;
  return std::numeric_limits<size_t>::max();
}

namespace {
// Does constraint kind `k` read its second ref (`b`)? Horizontal/Vertical/
// Radius are whole-entity on `a` only; the rest use both.
bool UsesSecondRef(sketch::ConstraintKind k) {
  using K = sketch::ConstraintKind;
  return k != K::Horizontal && k != K::Vertical && k != K::Radius;
}
// Coincident/Horizontal/Vertical are "trivial" — losing them to a delete isn't
// worth warning the user about (matches SolveSpace's PruneRequestsAndConstraints).
bool IsTrivial(sketch::ConstraintKind k) {
  using K = sketch::ConstraintKind;
  return k == K::Coincident || k == K::Horizontal || k == K::Vertical;
}
} // namespace

size_t SketchFeature::DeleteEntity(uint32_t id) {
  const size_t idx = IndexOfEntity(id);
  if (idx == std::numeric_limits<size_t>::max())
    return 0;

  using K = sketch::ConstraintKind;
  auto onDeleted = [&](const sketch::PointRef& r) { return r.entity == id; };

  // 1. Per point of the dying entity (keyed by `which`), collect the surviving
  //    points directly coincident with it, so the chain A~B~C survives as A~C
  //    once B is gone (transitive coincidence repair).
  std::map<uint8_t, std::vector<sketch::PointRef>> survivors;
  for (const auto& c : constraints) {
    if (c.kind != K::Coincident)
      continue;
    const bool aDel = onDeleted(c.a), bDel = onDeleted(c.b);
    if (aDel && !bDel)
      survivors[c.a.which].push_back(c.b);
    else if (bDel && !aDel)
      survivors[c.b.which].push_back(c.a);
  }

  // 2. Drop every constraint that references the dying entity (only via the
  //    refs its kind actually reads); count non-trivial losses for a warning.
  size_t droppedNonTrivial = 0;
  std::vector<sketch::Constraint> kept;
  kept.reserve(constraints.size());
  for (const auto& c : constraints) {
    const bool hit =
        c.a.entity == id || (UsesSecondRef(c.kind) && c.b.entity == id);
    if (hit) {
      if (!IsTrivial(c.kind))
        droppedNonTrivial++;
      continue;
    }
    kept.push_back(c);
  }

  // 3. Re-link the salvaged survivors pairwise.
  for (auto& [which, pts] : survivors) {
    (void)which;
    for (size_t i = 1; i < pts.size(); i++)
      kept.push_back({K::Coincident, pts[i - 1], pts[i], 0.0});
  }

  constraints = std::move(kept);
  entities.erase(entities.begin() + idx);
  return droppedNonTrivial;
}
