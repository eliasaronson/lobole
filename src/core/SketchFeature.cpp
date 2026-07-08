#include "core/SketchFeature.h"

#include "core/SketchProfile.h" // SegmentIntersect

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <vector>

namespace {
// Angles (radians) where segment [la,lb] crosses the circle (center, r), for
// intersection params strictly inside the segment. 0, 1, or 2 values.
std::vector<double> LineCircleAngles(glm::vec2 center, double r, glm::vec2 la,
                                     glm::vec2 lb) {
  std::vector<double> out;
  const glm::vec2 d = lb - la, f = la - center;
  const double A = glm::dot(d, d);
  if (A < 1e-12)
    return out;
  const double B = 2.0 * glm::dot(f, d);
  const double C = glm::dot(f, f) - r * r;
  double disc = B * B - 4.0 * A * C;
  if (disc < 0.0)
    return out;
  disc = std::sqrt(disc);
  for (double t : {(-B - disc) / (2.0 * A), (-B + disc) / (2.0 * A)}) {
    if (t > 1e-4 && t < 1.0 - 1e-4) {
      glm::vec2 p = la + static_cast<float>(t) * d;
      out.push_back(std::atan2(p.y - center.y, p.x - center.x));
    }
  }
  return out;
}

// Params t in (0,1) along segment [a, a+d] where it crosses circle (center, r).
std::vector<float> LineCircleParams(glm::vec2 a, glm::vec2 d, glm::vec2 center,
                                    double r) {
  std::vector<float> out;
  const double A = glm::dot(d, d);
  if (A < 1e-12)
    return out;
  const glm::vec2 f = a - center;
  const double B = 2.0 * glm::dot(f, d);
  const double C = glm::dot(f, f) - r * r;
  double disc = B * B - 4.0 * A * C;
  if (disc < 0.0)
    return out;
  disc = std::sqrt(disc);
  for (double t : {(-B - disc) / (2.0 * A), (-B + disc) / (2.0 * A)})
    if (t > 1e-4 && t < 1.0 - 1e-4)
      out.push_back(static_cast<float>(t));
  return out;
}

// True if angle `ang` lies within an arc's CCW sweep [startAngle, endAngle].
bool AngleInArc(double ang, const sketch::Arc& arc) {
  double s = arc.startAngle, en = arc.endAngle;
  while (en <= s)
    en += 2.0 * M_PI;
  while (ang < s)
    ang += 2.0 * M_PI;
  while (ang >= s + 2.0 * M_PI)
    ang -= 2.0 * M_PI;
  return ang <= en + 1e-6;
}
} // namespace

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

  // Cut parameters along [a,b]: interior intersections with other lines, and
  // where circles/arcs cross the line (so a curve laid over a line splits it).
  std::vector<float> cuts = {0.0f, 1.0f};
  for (const auto& e : entities) {
    if (sketch::EntityId(e) == id)
      continue;
    if (auto* o = std::get_if<sketch::Line>(&e)) {
      if (auto t = sketch::SegmentIntersect(a, line->b, o->a, o->b))
        if (*t > 1e-4f && *t < 1.0f - 1e-4f)
          cuts.push_back(*t);
    } else if (auto* c = std::get_if<sketch::Circle>(&e)) {
      for (float t : LineCircleParams(a, ab, c->center, c->radius))
        cuts.push_back(t);
    } else if (auto* ar = std::get_if<sketch::Arc>(&e)) {
      for (float t : LineCircleParams(a, ab, ar->center, ar->radius)) {
        const glm::vec2 p = a + t * ab;
        if (AngleInArc(std::atan2(p.y - ar->center.y, p.x - ar->center.x), *ar))
          cuts.push_back(t);
      }
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

bool SketchFeature::TrimCurveAt(uint32_t id, glm::vec2 clickUV) {
  const size_t idx = IndexOfEntity(id);
  if (idx == std::numeric_limits<size_t>::max())
    return false;

  // Extract center/radius and the angular window (a full circle, or the arc's
  // own sweep). Angles below are all measured relative to `base` so the window
  // is the half-open interval [base, base + span).
  glm::vec2 center;
  double radius, base, span;
  const bool isCircle = std::holds_alternative<sketch::Circle>(entities[idx]);
  if (auto* c = std::get_if<sketch::Circle>(&entities[idx])) {
    center = c->center;
    radius = c->radius;
    base = 0.0;
    span = 2.0 * M_PI;
  } else if (auto* ar = std::get_if<sketch::Arc>(&entities[idx])) {
    center = ar->center;
    radius = ar->radius;
    base = ar->startAngle;
    span = ar->endAngle - ar->startAngle;
  } else {
    return false; // not a curve
  }

  auto rel = [&](double ang) { // ang → offset in [0, 2π) from base
    double x = ang - base;
    while (x < 0.0)
      x += 2.0 * M_PI;
    while (x >= 2.0 * M_PI)
      x -= 2.0 * M_PI;
    return x;
  };

  // Interior cut offsets, plus the window ends (a circle wraps; an arc doesn't).
  std::vector<double> cuts;
  if (!isCircle) {
    cuts.push_back(0.0);
    cuts.push_back(span);
  }
  for (const auto& e : entities) {
    if (sketch::EntityId(e) == id)
      continue;
    auto* o = std::get_if<sketch::Line>(&e);
    if (!o)
      continue;
    for (double ang : LineCircleAngles(center, radius, o->a, o->b)) {
      double off = rel(ang);
      if (off > 1e-4 && off < span - 1e-4)
        cuts.push_back(off);
      else if (isCircle && off <= 1e-4)
        cuts.push_back(off); // keep a crossing exactly at base for circles
    }
  }
  std::sort(cuts.begin(), cuts.end());
  cuts.erase(std::unique(cuts.begin(), cuts.end(),
                         [](double x, double y) { return std::abs(x - y) < 1e-4; }),
             cuts.end());

  const double click = rel(std::atan2(clickUV.y - center.y, clickUV.x - center.x));

  const bool construction = sketch::IsConstruction(entities[idx]);
  auto makeArc = [&](double offLo, double offHi) {
    sketch::Entity e = sketch::Arc{center, radius, base + offLo, base + offHi};
    sketch::SetConstruction(e, construction);
    return e;
  };

  if (isCircle) {
    if (cuts.size() < 2) { // not enough crossings to bracket a span
      DeleteEntity(id);
      return true;
    }
    // Find the cyclic interval [cuts[i], cuts[i+1]] holding the click; keep its
    // complement as one arc (from the upper cut CCW around to the lower cut).
    for (size_t i = 0; i < cuts.size(); i++) {
      double lo = cuts[i];
      double hi = (i + 1 < cuts.size()) ? cuts[i + 1] : cuts[0] + 2.0 * M_PI;
      if (click + (click < lo ? 2.0 * M_PI : 0.0) >= lo - 1e-6 &&
          click + (click < lo ? 2.0 * M_PI : 0.0) <= hi + 1e-6) {
        entities[idx] = makeArc(hi, lo + 2.0 * M_PI); // complement, CCW
        sketch::SetEntityId(entities[idx], id);
        return true;
      }
    }
    return false;
  }

  // Arc: bracket the click between adjacent cuts (which include the endpoints),
  // drop that sub-span, and rebuild the surviving spans as arcs.
  double lo = 0.0, hi = span;
  for (size_t i = 0; i + 1 < cuts.size(); i++)
    if (click >= cuts[i] - 1e-6 && click <= cuts[i + 1] + 1e-6) {
      lo = cuts[i];
      hi = cuts[i + 1];
      break;
    }
  std::vector<std::pair<double, double>> survivors;
  if (lo > 1e-4)
    survivors.push_back({0.0, lo});
  if (hi < span - 1e-4)
    survivors.push_back({hi, span});
  if (survivors.empty()) {
    DeleteEntity(id);
    return true;
  }
  entities[idx] = makeArc(survivors[0].first, survivors[0].second);
  sketch::SetEntityId(entities[idx], id);
  for (size_t i = 1; i < survivors.size(); i++)
    AddEntity(makeArc(survivors[i].first, survivors[i].second));
  return true;
}

std::vector<uint32_t>
SketchFeature::MirrorEntities(const std::vector<uint32_t>& sourceIds,
                             uint32_t axisLineId, bool link) {
  using K = sketch::ConstraintKind;
  std::vector<uint32_t> generated;
  // Axis geometry comes from the datum helpers for the X/Y axes, else from the
  // axis line entity. The linked Symmetric constraints reference axisLineId
  // either way — the solver resolves a datum axis id via its registered line.
  glm::vec2 a, b;
  if (sketch::IsDatum(axisLineId)) {
    const auto ep = sketch::DatumAxisEndpoints(axisLineId);
    a = ep.first;
    b = ep.second;
  } else {
    const size_t axisIdx = IndexOfEntity(axisLineId);
    if (axisIdx == std::numeric_limits<size_t>::max())
      return generated;
    auto* axis = std::get_if<sketch::Line>(&entities[axisIdx]);
    if (!axis)
      return generated;
    a = axis->a;
    b = axis->b;
  }

  std::vector<std::pair<uint32_t, uint32_t>> pairs; // (source id, copy id)
  for (uint32_t sid : sourceIds) {
    const size_t si = IndexOfEntity(sid);
    if (si == std::numeric_limits<size_t>::max() || sid == axisLineId)
      continue;
    sketch::Entity copy = sketch::ReflectEntity(entities[si], a, b);
    // Perturb a reflected arc so the solver can distinguish Symmetry from Equal
    // (FreeCAD issue #13551).
    if (link)
      if (auto* arc = std::get_if<sketch::Arc>(&copy)) {
        arc->startAngle += 1e-6;
        arc->endAngle += 1e-6;
      }
    const uint32_t cid = AddEntity(copy);
    pairs.emplace_back(sid, cid);
    generated.push_back(cid);
  }
  if (!link)
    return generated;

  auto onAxis = [&](glm::vec2 p) {
    return glm::length(p - sketch::ReflectPoint(p, a, b)) < 1e-6f;
  };
  for (auto [sid, cid] : pairs) {
    const sketch::Entity& src = entities[IndexOfEntity(sid)];
    // Symmetric pins source point `sw` to copy point `cw` about the axis; a
    // point already on the axis is its own mirror → use Coincident instead.
    auto symPair = [&](uint8_t sw, uint8_t cw) {
      if (onAxis(sketch::EntityPointUV(src, sw)))
        constraints.push_back({K::Coincident, {sid, sw}, {cid, cw}, 0.0});
      else
        constraints.push_back(
            {K::Symmetric, {sid, sw}, {cid, cw}, 0.0, {axisLineId, 0}});
    };
    std::visit(
        [&](const auto& g) {
          using T = std::decay_t<decltype(g)>;
          if constexpr (std::is_same_v<T, sketch::Point>) {
            symPair(0, 0);
          } else if constexpr (std::is_same_v<T, sketch::Line>) {
            symPair(0, 0);
            symPair(1, 1);
          } else if constexpr (std::is_same_v<T, sketch::Circle>) {
            constraints.push_back({K::Equal, {sid, 0}, {cid, 0}, 0.0});
            symPair(0, 0); // centres
          } else { // Arc: reflection swaps start/end (winding flips)
            constraints.push_back({K::Equal, {sid, 0}, {cid, 0}, 0.0});
            symPair(0, 0); // centres
            symPair(1, 2);
            symPair(2, 1);
          }
        },
        src);
  }
  return generated;
}

uint32_t SketchFeature::FilletCorner(uint32_t lineId1, uint32_t lineId2,
                                     double radius) {
  using K = sketch::ConstraintKind;
  if (radius <= 0.0)
    return 0;
  const size_t i1 = IndexOfEntity(lineId1), i2 = IndexOfEntity(lineId2);
  if (i1 == std::numeric_limits<size_t>::max() ||
      i2 == std::numeric_limits<size_t>::max())
    return 0;
  auto* l1 = std::get_if<sketch::Line>(&entities[i1]);
  auto* l2 = std::get_if<sketch::Line>(&entities[i2]);
  if (!l1 || !l2)
    return 0;

  // The shared corner: the endpoint pair (one per line) that coincides. c1/c2
  // are the which-index (0=a, 1=b) of the corner endpoint on each line.
  const float tol = 1e-4f;
  const glm::vec2 e1[2] = {l1->a, l1->b}, e2[2] = {l2->a, l2->b};
  int c1 = -1, c2 = -1;
  for (int i = 0; i < 2 && c1 < 0; i++)
    for (int j = 0; j < 2; j++)
      if (glm::length(e1[i] - e2[j]) < tol) {
        c1 = i;
        c2 = j;
        break;
      }
  if (c1 < 0)
    return 0; // no shared corner

  const glm::vec2 C = e1[c1];
  glm::vec2 d1 = e1[1 - c1] - C, d2 = e2[1 - c2] - C;
  const float len1 = glm::length(d1), len2 = glm::length(d2);
  if (len1 < tol || len2 < tol)
    return 0;
  d1 /= len1;
  d2 /= len2;
  const double theta = std::acos(glm::clamp(glm::dot(d1, d2), -1.0f, 1.0f));
  if (theta < 1e-3 || theta > M_PI - 1e-3)
    return 0; // parallel / collinear
  const double dTan = radius / std::tan(theta / 2.0);
  if (dTan > len1 - tol || dTan > len2 - tol)
    return 0; // radius too large for a segment
  const glm::vec2 bis = glm::normalize(d1 + d2);
  const glm::vec2 center = C + bis * static_cast<float>(radius / std::sin(theta / 2.0));
  const glm::vec2 T1 = C + d1 * static_cast<float>(dTan);
  const glm::vec2 T2 = C + d2 * static_cast<float>(dTan);

  // Trim each line's corner endpoint to its tangent point.
  (c1 == 0 ? l1->a : l1->b) = T1;
  (c2 == 0 ? l2->a : l2->b) = T2;

  // Minor arc T1→T2 (CCW-always), and which endpoint the arc starts at.
  auto wrap = [](double x) {
    while (x < 0)
      x += 2.0 * M_PI;
    while (x >= 2.0 * M_PI)
      x -= 2.0 * M_PI;
    return x;
  };
  const double aT1 = std::atan2(T1.y - center.y, T1.x - center.x);
  const double aT2 = std::atan2(T2.y - center.y, T2.x - center.x);
  const bool startIsT1 = wrap(aT2 - aT1) <= M_PI;
  sketch::Arc arc = startIsT1
                        ? sketch::Arc{center, radius, aT1, aT1 + wrap(aT2 - aT1)}
                        : sketch::Arc{center, radius, aT2, aT2 + wrap(aT1 - aT2)};
  const uint32_t arcId = AddEntity(arc);

  // Remove the old corner coincident joining the two lines.
  const uint8_t w1 = static_cast<uint8_t>(c1), w2 = static_cast<uint8_t>(c2);
  auto refIs = [](const sketch::PointRef& r, uint32_t id, uint8_t w) {
    return r.entity == id && r.which == w;
  };
  constraints.erase(
      std::remove_if(constraints.begin(), constraints.end(),
                     [&](const sketch::Constraint& con) {
                       return con.kind == K::Coincident &&
                              ((refIs(con.a, lineId1, w1) &&
                                refIs(con.b, lineId2, w2)) ||
                               (refIs(con.a, lineId2, w2) &&
                                refIs(con.b, lineId1, w1)));
                     }),
      constraints.end());

  // Join the trimmed line ends to the arc endpoints; tangency does the rest.
  const uint32_t startLine = startIsT1 ? lineId1 : lineId2;
  const uint8_t startWhich = startIsT1 ? w1 : w2;
  const uint32_t endLine = startIsT1 ? lineId2 : lineId1;
  const uint8_t endWhich = startIsT1 ? w2 : w1;
  constraints.push_back({K::Coincident, {startLine, startWhich}, {arcId, 1}, 0.0});
  constraints.push_back({K::Coincident, {endLine, endWhich}, {arcId, 2}, 0.0});
  constraints.push_back({K::Tangent, {lineId1, 0}, {arcId, 0}, 0.0});
  constraints.push_back({K::Tangent, {lineId2, 0}, {arcId, 0}, 0.0});
  return arcId;
}

void SketchFeature::RegenerateDerived() {
  for (auto& op : derived) {
    auto transform = [&](const sketch::Entity& src, int k) -> sketch::Entity {
      if (op.kind == sketch::DerivedKind::LinearPattern)
        return sketch::TranslateEntity(src, static_cast<float>(k) * op.vec);
      return sketch::RotateEntity(src, op.vec,
                                  static_cast<double>(k) * op.scalar);
    };
    std::vector<sketch::DerivedOp::Inst> keep;
    for (uint32_t sid : op.sourceIds) {
      const size_t si = IndexOfEntity(sid);
      if (si == std::numeric_limits<size_t>::max())
        continue; // source removed → its instances get pruned below
      const sketch::Entity srcCopy = entities[si]; // copy before any AddEntity
      for (int k = 1; k < op.count; k++) {
        uint32_t genId = 0;
        for (const auto& in : op.instances)
          if (in.sourceId == sid && in.k == k) {
            genId = in.genId;
            break;
          }
        sketch::Entity g = transform(srcCopy, k);
        const size_t gi = (genId != 0) ? IndexOfEntity(genId)
                                       : std::numeric_limits<size_t>::max();
        if (gi != std::numeric_limits<size_t>::max()) {
          sketch::SetEntityId(g, genId); // reuse the entity, keep its id
          entities[gi] = g;
        } else {
          genId = AddEntity(g); // new instance (first regen, or count grew)
        }
        keep.push_back({sid, k, genId});
      }
    }
    // Prune instances no longer produced (count shrank or a source vanished).
    for (const auto& in : op.instances) {
      bool kept = false;
      for (const auto& kp : keep)
        if (kp.genId == in.genId) {
          kept = true;
          break;
        }
      if (!kept)
        DeleteEntity(in.genId);
    }
    op.instances = std::move(keep);
  }
}

std::vector<uint32_t>
SketchFeature::AddLinearPattern(const std::vector<uint32_t>& sourceIds,
                               glm::vec2 step, int count, bool link) {
  std::vector<uint32_t> gen;
  if (count < 2 || sourceIds.empty())
    return gen;
  if (!link) {
    for (uint32_t sid : sourceIds) {
      const size_t si = IndexOfEntity(sid);
      if (si == std::numeric_limits<size_t>::max())
        continue;
      const sketch::Entity srcCopy = entities[si];
      for (int k = 1; k < count; k++)
        gen.push_back(AddEntity(
            sketch::TranslateEntity(srcCopy, static_cast<float>(k) * step)));
    }
    return gen;
  }
  sketch::DerivedOp op;
  op.kind = sketch::DerivedKind::LinearPattern;
  op.sourceIds = sourceIds;
  op.vec = step;
  op.count = count;
  derived.push_back(op);
  RegenerateDerived();
  for (const auto& in : derived.back().instances)
    gen.push_back(in.genId);
  return gen;
}

std::vector<uint32_t>
SketchFeature::AddCircularPattern(const std::vector<uint32_t>& sourceIds,
                                 glm::vec2 center, double stepAngle, int count,
                                 bool link) {
  std::vector<uint32_t> gen;
  if (count < 2 || sourceIds.empty())
    return gen;
  if (!link) {
    for (uint32_t sid : sourceIds) {
      const size_t si = IndexOfEntity(sid);
      if (si == std::numeric_limits<size_t>::max())
        continue;
      const sketch::Entity srcCopy = entities[si];
      for (int k = 1; k < count; k++)
        gen.push_back(AddEntity(sketch::RotateEntity(
            srcCopy, center, static_cast<double>(k) * stepAngle)));
    }
    return gen;
  }
  sketch::DerivedOp op;
  op.kind = sketch::DerivedKind::CircularPattern;
  op.sourceIds = sourceIds;
  op.vec = center;
  op.scalar = stepAngle;
  op.count = count;
  derived.push_back(op);
  RegenerateDerived();
  for (const auto& in : derived.back().instances)
    gen.push_back(in.genId);
  return gen;
}

std::vector<uint32_t>
SketchFeature::OffsetChain(const std::vector<uint32_t>& chainIds,
                           double distance, bool joinArc) {
  std::vector<uint32_t> gen;
  std::vector<sketch::Entity> chain;
  for (uint32_t id : chainIds) {
    const size_t i = IndexOfEntity(id);
    if (i != std::numeric_limits<size_t>::max())
      chain.push_back(entities[i]);
  }
  for (auto& e : sketch::OffsetLoop(plane, chain, distance, joinArc))
    gen.push_back(AddEntity(e));
  return gen;
}
