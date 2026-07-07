#include "core/SketchFeature.h"

#include "core/SketchProfile.h" // SegmentIntersect

#include <algorithm>
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

std::array<uint32_t, 4> SketchFeature::AddRectangle(glm::vec2 c1, glm::vec2 c2) {
  using K = sketch::ConstraintKind;
  const glm::vec2 a{c1.x, c1.y}, b{c2.x, c1.y}, c{c2.x, c2.y}, d{c1.x, c2.y};
  const uint32_t l0 = AddEntity(sketch::Line{a, b}); // bottom (horizontal)
  const uint32_t l1 = AddEntity(sketch::Line{b, c}); // right (vertical)
  const uint32_t l2 = AddEntity(sketch::Line{c, d}); // top (horizontal)
  const uint32_t l3 = AddEntity(sketch::Line{d, a}); // left (vertical)
  // Corner coincidents: end of each line meets the start of the next.
  constraints.push_back({K::Coincident, {l0, 1}, {l1, 0}, 0.0});
  constraints.push_back({K::Coincident, {l1, 1}, {l2, 0}, 0.0});
  constraints.push_back({K::Coincident, {l2, 1}, {l3, 0}, 0.0});
  constraints.push_back({K::Coincident, {l3, 1}, {l0, 0}, 0.0});
  // Axis alignment on the whole sides.
  constraints.push_back({K::Horizontal, {l0, 0}, {0, 0}, 0.0});
  constraints.push_back({K::Horizontal, {l2, 0}, {0, 0}, 0.0});
  constraints.push_back({K::Vertical, {l1, 0}, {0, 0}, 0.0});
  constraints.push_back({K::Vertical, {l3, 0}, {0, 0}, 0.0});
  return {l0, l1, l2, l3};
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

uint32_t SketchFeature::SplitEntity(uint32_t id, glm::vec2 P) {
  const size_t idx = IndexOfEntity(id);
  if (idx == std::numeric_limits<size_t>::max())
    return 0;
  auto* line = std::get_if<sketch::Line>(&entities[idx]);
  if (!line)
    return 0;
  const glm::vec2 a = line->a, b = line->b;
  const float eps = 1e-4f;
  if (glm::length(P - a) < eps || glm::length(P - b) < eps)
    return 0; // degenerate: split at an endpoint

  using K = sketch::ConstraintKind;
  line->b = P;                                       // piece 1: a -> P (keeps id)
  const uint32_t newId = AddEntity(sketch::Line{P, b}); // piece 2: P -> b

  // Move every reference to the original far endpoint (id, 1) onto piece 2.
  // This carries endpoint coincidents/point-on-line and, for a whole-edge
  // distance {id,0}-{id,1}, re-anchors it to span {id,0}-{newId,1}.
  for (auto& c : constraints) {
    if (c.a.entity == id && c.a.which == 1)
      c.a = {newId, 1};
    if (c.b.entity == id && c.b.which == 1)
      c.b = {newId, 1};
  }
  // Copy axis/parallel constraints onto piece 2 so it stays aligned like the
  // piece that kept the id (H/V keep a trimmed rectangle side straight).
  std::vector<sketch::Constraint> copies;
  for (const auto& c : constraints) {
    if (c.kind != K::Horizontal && c.kind != K::Vertical &&
        c.kind != K::Parallel && c.kind != K::Perpendicular)
      continue;
    sketch::Constraint copy = c;
    bool refs = false;
    if (copy.a.entity == id) {
      copy.a.entity = newId;
      refs = true;
    }
    if (UsesSecondRef(copy.kind) && copy.b.entity == id) {
      copy.b.entity = newId;
      refs = true;
    }
    if (refs)
      copies.push_back(copy);
  }
  for (const auto& c : copies)
    constraints.push_back(c);
  // Join the two pieces at the cut.
  constraints.push_back({K::Coincident, {id, 1}, {newId, 0}, 0.0});
  return newId;
}

bool SketchFeature::TrimBracket(uint32_t id, glm::vec2 clickUV, glm::vec2& a,
                                glm::vec2& ab, float& tLo, float& tHi) const {
  const size_t idx = IndexOfEntity(id);
  if (idx == std::numeric_limits<size_t>::max())
    return false;
  auto* line = std::get_if<sketch::Line>(&entities[idx]);
  if (!line)
    return false;
  a = line->a;
  ab = line->b - line->a;
  const float len2 = glm::dot(ab, ab);
  if (len2 < 1e-12f)
    return false;

  // Cut parameters along [a,b]: interior intersections with other lines.
  std::vector<float> cuts = {0.0f, 1.0f};
  for (const auto& e : entities) {
    if (sketch::EntityId(e) == id)
      continue;
    if (auto* o = std::get_if<sketch::Line>(&e)) {
      if (auto t = sketch::SegmentIntersect(a, line->b, o->a, o->b))
        if (*t > 1e-4f && *t < 1.0f - 1e-4f)
          cuts.push_back(*t);
    }
  }
  std::sort(cuts.begin(), cuts.end());
  cuts.erase(std::unique(cuts.begin(), cuts.end(),
                         [](float x, float y) { return std::abs(x - y) < 1e-4f; }),
             cuts.end());

  const float tClick = glm::clamp(glm::dot(clickUV - a, ab) / len2, 0.0f, 1.0f);
  tLo = 0.0f;
  tHi = 1.0f;
  for (size_t i = 0; i + 1 < cuts.size(); i++) {
    if (tClick >= cuts[i] - 1e-6f && tClick <= cuts[i + 1] + 1e-6f) {
      tLo = cuts[i];
      tHi = cuts[i + 1];
      break;
    }
  }
  return true;
}

std::optional<std::pair<glm::vec2, glm::vec2>>
SketchFeature::TrimSpanAt(uint32_t id, glm::vec2 clickUV) const {
  glm::vec2 a, ab;
  float tLo, tHi;
  if (!TrimBracket(id, clickUV, a, ab, tLo, tHi))
    return std::nullopt;
  return std::make_pair(a + tLo * ab, a + tHi * ab);
}

bool SketchFeature::TrimLineAt(uint32_t id, glm::vec2 clickUV) {
  glm::vec2 a, ab;
  float tLo, tHi;
  if (!TrimBracket(id, clickUV, a, ab, tLo, tHi))
    return false;
  auto at = [&](float t) { return a + t * ab; };
  if (tLo <= 1e-4f && tHi >= 1.0f - 1e-4f) {
    DeleteEntity(id); // no bracketing cut — remove the whole line
    return true;
  }
  // Split off the clicked interval [tLo, tHi] and delete just that piece.
  uint32_t clicked = id;
  if (tHi < 1.0f - 1e-4f)
    SplitEntity(id, at(tHi)); // id becomes [0, tHi]
  if (tLo > 1e-4f)
    clicked = SplitEntity(id, at(tLo)); // new piece is [tLo, tHi]
  DeleteEntity(clicked);
  return true;
}
