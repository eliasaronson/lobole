#include "core/features/SketchFeature.h"

#include "common/Logger.h"
#include "core/Angles.h"
#include "core/features/DatumPlaneFeature.h"
#include "core/features/FrameFeature.h"
#include "core/sketch/SketchProfile.h" // SegmentIntersect
#include "core/sketch/SketchSolver.h"
#include "core/sketch/SketchText.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <random>
#include <set>
#include <vector>

namespace {
// Angles (radians) where segment [la,lb] crosses the circle (center, r), for
// intersection params strictly inside the segment. 0, 1, or 2 values.
std::vector<double> LineCircleAngles(glm::dvec2 center, double r, glm::dvec2 la,
                                     glm::dvec2 lb) {
  std::vector<double> out;
  const glm::dvec2 d = lb - la, f = la - center;
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
      glm::dvec2 p = la + t * d;
      out.push_back(std::atan2(p.y - center.y, p.x - center.x));
    }
  }
  return out;
}

// Params t in (0,1) along segment [a, a+d] where it crosses circle (center, r).
std::vector<double> LineCircleParams(glm::dvec2 a, glm::dvec2 d,
                                     glm::dvec2 center, double r) {
  std::vector<double> out;
  const double A = glm::dot(d, d);
  if (A < 1e-12)
    return out;
  const glm::dvec2 f = a - center;
  const double B = 2.0 * glm::dot(f, d);
  const double C = glm::dot(f, f) - r * r;
  double disc = B * B - 4.0 * A * C;
  if (disc < 0.0)
    return out;
  disc = std::sqrt(disc);
  for (double t : {(-B - disc) / (2.0 * A), (-B + disc) / (2.0 * A)})
    if (t > 1e-4 && t < 1.0 - 1e-4)
      out.push_back(t);
  return out;
}

// Angles (radians) on circle (c1, r1) where circle (c2, r2) crosses it.
// Tangent/concentric/separate pairs yield none.
std::vector<double> CircleCircleAngles(glm::dvec2 c1, double r1, glm::dvec2 c2,
                                       double r2) {
  std::vector<double> out;
  const glm::dvec2 d(double(c2.x) - c1.x, double(c2.y) - c1.y);
  const double dist = std::hypot(d.x, d.y);
  if (dist < 1e-9 || dist > r1 + r2 - 1e-9 || dist < std::abs(r1 - r2) + 1e-9) {
    return out;
  }
  const double a = (r1 * r1 - r2 * r2 + dist * dist) / (2.0 * dist);
  const double h2 = r1 * r1 - a * a;
  if (h2 <= 0.0)
    return out;
  const double h = std::sqrt(h2);
  const glm::dvec2 u = d / dist;
  const glm::dvec2 mid(c1.x + u.x * a, c1.y + u.y * a);
  const glm::dvec2 perp(-u.y, u.x);
  for (const double sign : {1.0, -1.0}) {
    const glm::dvec2 p(mid.x + perp.x * h * sign, mid.y + perp.y * h * sign);
    out.push_back(std::atan2(p.y - c1.y, p.x - c1.x));
  }
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

namespace sketch {

std::optional<std::pair<size_t, bool>> PoleField(const std::string& field) {
  // "p<k>x" / "p<k>y": pole k's coordinate.
  if (field.size() < 3 || field[0] != 'p' ||
      (field.back() != 'x' && field.back() != 'y'))
    return std::nullopt;
  size_t k = 0;
  for (size_t i = 1; i + 1 < field.size(); i++) {
    if (field[i] < '0' || field[i] > '9')
      return std::nullopt;
    k = k * 10 + static_cast<size_t>(field[i] - '0');
  }
  return std::pair<size_t, bool>{k, field.back() == 'x'};
}

std::vector<std::string> EntityExprFields(const Entity& e) {
  return std::visit(
      [](const auto& g) -> std::vector<std::string> {
        using T = std::decay_t<decltype(g)>;
        if constexpr (std::is_same_v<T, Point>)
          return {"x", "y"};
        else if constexpr (std::is_same_v<T, Line>)
          return {"ax", "ay", "bx", "by"};
        else if constexpr (std::is_same_v<T, Circle>)
          return {"cx", "cy", "r"};
        else if constexpr (std::is_same_v<T, Arc>)
          return {"cx", "cy", "r"};
        else if constexpr (std::is_same_v<T, BSpline>) {
          // Every pole, so an outline drawn as a spline (a knight's head, a
          // cam lobe) can follow a variable the way a polyline does.
          std::vector<std::string> out;
          for (size_t k = 0; k < g.poles.size(); k++) {
            out.push_back("p" + std::to_string(k) + "x");
            out.push_back("p" + std::to_string(k) + "y");
          }
          return out;
        } else if constexpr (std::is_same_v<T, Text>)
          return {"x", "y", "height", "angle_deg", "spacing"};
        else // Ellipse: no expr-driven scalar fields in v1
          return {};
      },
      e);
}

std::optional<double> GetEntityField(const Entity& e,
                                     const std::string& field) {
  return std::visit(
      [&](const auto& g) -> std::optional<double> {
        using T = std::decay_t<decltype(g)>;
        if constexpr (std::is_same_v<T, Point>) {
          if (field == "x")
            return g.p.x;
          if (field == "y")
            return g.p.y;
        } else if constexpr (std::is_same_v<T, Line>) {
          if (field == "ax")
            return g.a.x;
          if (field == "ay")
            return g.a.y;
          if (field == "bx")
            return g.b.x;
          if (field == "by")
            return g.b.y;
        } else if constexpr (std::is_same_v<T, Circle> ||
                             std::is_same_v<T, Arc>) {
          if (field == "cx")
            return g.center.x;
          if (field == "cy")
            return g.center.y;
          if (field == "r")
            return g.radius;
        } else if constexpr (std::is_same_v<T, BSpline>) {
          if (const auto pf = PoleField(field);
              pf && pf->first < g.poles.size())
            return pf->second ? g.poles[pf->first].x : g.poles[pf->first].y;
        } else if constexpr (std::is_same_v<T, Text>) {
          if (field == "x")
            return g.anchor.x;
          if (field == "y")
            return g.anchor.y;
          if (field == "height")
            return g.height;
          if (field == "angle_deg") // the expression speaks degrees
            return angles::ToDegrees(g.angle);
          if (field == "spacing")
            return g.spacing;
        }
        return std::nullopt;
      },
      e);
}

bool SetEntityField(Entity& e, const std::string& field, double value) {
  const double f = value;
  return std::visit(
      [&](auto& g) -> bool {
        using T = std::decay_t<decltype(g)>;
        if constexpr (std::is_same_v<T, Point>) {
          if (field == "x") {
            g.p.x = f;
            return true;
          }
          if (field == "y") {
            g.p.y = f;
            return true;
          }
        } else if constexpr (std::is_same_v<T, Line>) {
          if (field == "ax") {
            g.a.x = f;
            return true;
          }
          if (field == "ay") {
            g.a.y = f;
            return true;
          }
          if (field == "bx") {
            g.b.x = f;
            return true;
          }
          if (field == "by") {
            g.b.y = f;
            return true;
          }
        } else if constexpr (std::is_same_v<T, Circle> ||
                             std::is_same_v<T, Arc>) {
          if (field == "cx") {
            g.center.x = f;
            return true;
          }
          if (field == "cy") {
            g.center.y = f;
            return true;
          }
          if (field == "r") {
            g.radius = value;
            return true;
          }
        } else if constexpr (std::is_same_v<T, BSpline>) {
          if (const auto pf = PoleField(field);
              pf && pf->first < g.poles.size()) {
            (pf->second ? g.poles[pf->first].x : g.poles[pf->first].y) = f;
            return true;
          }
        } else if constexpr (std::is_same_v<T, Text>) {
          if (field == "x") {
            g.anchor.x = f;
            return true;
          }
          if (field == "y") {
            g.anchor.y = f;
            return true;
          }
          if (field == "height") {
            g.height = value;
            return true;
          }
          if (field == "angle_deg") {
            g.angle = angles::ToRadians(value);
            return true;
          }
          if (field == "spacing") {
            g.spacing = value;
            return true;
          }
        }
        return false;
      },
      e);
}

} // namespace sketch

SketchFeature::SketchFeature(const std::string& name, const SketchPlane& p)
    : plane(p) {
  m_name = name;
}

uint32_t SketchFeature::MintEntityId() const {
  // Seeded once per process. Not reproducible run to run, which is fine:
  // nothing asserts a particular entity id (the tests capture what AddEntity
  // returns), and the goldens compare counts and volumes rather than bytes.
  // One engine PER THREAD: a project's parts rebuild side by side when it
  // opens (docs/ui/long-operations.md#opening-a-project), and a sketch that
  // grows a projected or patterned entity mints one during that rebuild -- a
  // shared engine was a data race.
  thread_local std::mt19937 rng(std::random_device{}());
  // 0 is "none", and ids from kDatumBase up are the origin and axes: a real
  // entity minted there would be read as a datum.
  std::uniform_int_distribution<uint32_t> pick(1, sketch::kDatumBase - 1);
  for (;;) {
    const uint32_t id = pick(rng);
    const bool taken = std::any_of(
        entities.begin(), entities.end(),
        [id](const sketch::Entity& e) { return sketch::EntityId(e) == id; });
    if (!taken)
      return id;
  }
}

uint32_t SketchFeature::AddEntity(sketch::Entity e) {
  const uint32_t id = MintEntityId();
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

std::vector<std::vector<uint32_t>>
SketchFeature::SiblingRegions(const std::vector<uint32_t>& ids) const {
  const std::set<uint32_t> idSet(ids.begin(), ids.end());
  for (const sketch::DerivedOp& op : derived) {
    // A linked copy is a loose paste, not a replication of one shape — its
    // copy being a separate profile IS a reasonable intent. Patterns and
    // mirrors replicate; those expand.
    if (op.kind == sketch::DerivedKind::LinkedCopy)
      continue;
    // Family member id-sets: the sources, then each replication index k
    // (a multi-entity source contributes one Inst per entity per k).
    std::vector<std::set<uint32_t>> members;
    members.emplace_back(op.sourceIds.begin(), op.sourceIds.end());
    std::map<int, std::set<uint32_t>> byK;
    for (const sketch::DerivedOp::Inst& in : op.instances)
      byK[in.k].insert(in.genId);
    for (auto& [k, s] : byK)
      members.push_back(std::move(s));
    const bool isMember =
        std::any_of(members.begin(), members.end(),
                    [&](const std::set<uint32_t>& m) { return m == idSet; });
    if (!isMember)
      continue;
    // Every member that forms a whole profile loop right now, in
    // LoopEntityIds order so each row is a valid `regions` entry.
    std::vector<std::vector<uint32_t>> regions;
    for (const sketch::Loop& loop : sketch::BuildLoops(entities)) {
      std::vector<uint32_t> lids = sketch::LoopEntityIds(loop);
      const std::set<uint32_t> lset(lids.begin(), lids.end());
      if (std::any_of(members.begin(), members.end(),
                      [&](const std::set<uint32_t>& m) { return m == lset; }))
        regions.push_back(std::move(lids));
    }
    if (!regions.empty())
      return regions;
  }
  return {ids};
}

uint32_t SketchFeature::AddImage(sketch::ReferenceImage img) {
  const uint32_t id = nextImageId++;
  img.id = id;
  if (img.asset.empty())
    img.asset = std::to_string(GetId()) + "-" + std::to_string(id) + ".png";
  images.push_back(std::move(img));
  return id;
}

size_t SketchFeature::IndexOfImage(uint32_t id) const {
  if (id == 0)
    return std::numeric_limits<size_t>::max();
  for (size_t i = 0; i < images.size(); i++)
    if (images[i].id == id)
      return i;
  return std::numeric_limits<size_t>::max();
}

bool SketchFeature::DeleteImage(uint32_t id) {
  const size_t idx = IndexOfImage(id);
  if (idx == std::numeric_limits<size_t>::max())
    return false;
  images.erase(images.begin() + idx);
  return true;
}

bool SketchFeature::CalibrateImage(uint32_t id, glm::dvec2 p1, glm::dvec2 p2,
                                   double distance, int alignAxis) {
  const size_t idx = IndexOfImage(id);
  if (idx == std::numeric_limits<size_t>::max())
    return false;
  sketch::ReferenceImage& img = images[idx];
  const glm::dvec2 d(double(p2.x) - p1.x, double(p2.y) - p1.y);
  const double curLen = std::hypot(d.x, d.y);
  if (curLen < 1e-9 || distance <= 0.0)
    return false; // coincident picks or a non-positive target: nothing to do

  // Recover p1's pixel coordinate under the CURRENT placement so we can hold it
  // fixed while the scale (and maybe rotation) changes:
  //   pix = R(-rotation) * (p - origin) / unitsPerPixel.
  const double c = std::cos(img.rotation), s = std::sin(img.rotation);
  const glm::dvec2 rel(double(p1.x) - img.origin.x,
                       double(p1.y) - img.origin.y);
  const double invScale =
      img.unitsPerPixel != 0.0 ? 1.0 / img.unitsPerPixel : 0.0;
  const glm::dvec2 pix1((c * rel.x + s * rel.y) * invScale,
                        (-s * rel.x + c * rel.y) * invScale);

  // New rotation: align the p1->p2 direction onto the chosen axis, else keep
  // it.
  double rot = img.rotation;
  if (alignAxis == 0 || alignAxis == 1) {
    const double target = alignAxis == 0 ? 0.0 : M_PI / 2.0;
    rot += target - std::atan2(d.y, d.x);
  }
  const double scale = img.unitsPerPixel * (distance / curLen);

  // Re-anchor the origin so p1's pixel maps back to p1 under the new transform.
  const double c2 = std::cos(rot), s2 = std::sin(rot);
  const glm::dvec2 offset(scale * (c2 * pix1.x - s2 * pix1.y),
                          scale * (s2 * pix1.x + c2 * pix1.y));
  img.origin = glm::dvec2(double(double(p1.x) - offset.x),
                          double(double(p1.y) - offset.y));
  img.rotation = rot;
  img.unitsPerPixel = scale;
  return true;
}

std::array<uint32_t, 4> SketchFeature::AddRectangle(glm::dvec2 c1,
                                                    glm::dvec2 c2) {
  using K = sketch::ConstraintKind;
  const glm::dvec2 a{c1.x, c1.y}, b{c2.x, c1.y}, c{c2.x, c2.y}, d{c1.x, c2.y};
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

std::array<uint32_t, 5> SketchFeature::AddRectangleCentered(glm::dvec2 center,
                                                            glm::dvec2 corner) {
  using K = sketch::ConstraintKind;
  const auto ids = AddRectangle(2.0 * center - corner, corner);
  // Anchor: a centre point held midway between the diagonal corners —
  // Symmetric about a POINT (the solver's P2PSymmetric point overload), so
  // the rectangle stays centred on it when dimensioned or dragged.
  const uint32_t p = AddEntity(sketch::Point{center});
  sketch::Constraint sym{K::Symmetric, {ids[0], 0}, {ids[2], 0}};
  sym.c = {p, 0};
  constraints.push_back(sym);
  return {ids[0], ids[1], ids[2], ids[3], p};
}

namespace {
// Does constraint kind `k` read its second ref (`b`)? Horizontal/Vertical/
// Radius are whole-entity on `a` only; the rest use both.
bool UsesSecondRef(sketch::ConstraintKind k) {
  using K = sketch::ConstraintKind;
  return k != K::Horizontal && k != K::Vertical && k != K::Radius;
}
// Coincident/Horizontal/Vertical are "trivial" — losing them to a delete isn't
// worth warning the user about (matches SolveSpace's
// PruneRequestsAndConstraints).
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
        c.a.entity == id || (UsesSecondRef(c.kind) && c.b.entity == id) ||
        (c.kind == K::Symmetric && c.c.entity == id); // the symmetry line/point
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
  entityExprs.erase(id); // any coordinate expressions die with the entity
  return droppedNonTrivial;
}

uint32_t SketchFeature::SplitEntity(uint32_t id, glm::dvec2 P) {
  const size_t idx = IndexOfEntity(id);
  if (idx == std::numeric_limits<size_t>::max())
    return 0;
  auto* line = std::get_if<sketch::Line>(&entities[idx]);
  if (!line)
    return 0;
  const glm::dvec2 a = line->a, b = line->b;
  const double eps = 1e-4;
  if (glm::length(P - a) < eps || glm::length(P - b) < eps)
    return 0; // degenerate: split at an endpoint

  using K = sketch::ConstraintKind;
  line->b = P; // piece 1: a -> P (keeps id)
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

bool SketchFeature::TrimBracket(uint32_t id, glm::dvec2 clickUV, glm::dvec2& a,
                                glm::dvec2& ab, double& tLo,
                                double& tHi) const {
  const size_t idx = IndexOfEntity(id);
  if (idx == std::numeric_limits<size_t>::max())
    return false;
  auto* line = std::get_if<sketch::Line>(&entities[idx]);
  if (!line)
    return false;
  a = line->a;
  ab = line->b - line->a;
  const double len2 = glm::dot(ab, ab);
  if (len2 < 1e-12)
    return false;

  // Cut parameters along [a,b]: interior intersections with other lines, and
  // where circles/arcs cross the line (so a curve laid over a line splits it).
  std::vector<double> cuts = {0.0, 1.0};
  for (const auto& e : entities) {
    if (sketch::EntityId(e) == id)
      continue;
    if (auto* o = std::get_if<sketch::Line>(&e)) {
      if (auto t = sketch::SegmentIntersect(a, line->b, o->a, o->b))
        if (*t > 1e-4 && *t < 1.0 - 1e-4)
          cuts.push_back(*t);
    } else if (auto* c = std::get_if<sketch::Circle>(&e)) {
      for (double t : LineCircleParams(a, ab, c->center, c->radius))
        cuts.push_back(t);
    } else if (auto* ar = std::get_if<sketch::Arc>(&e)) {
      for (double t : LineCircleParams(a, ab, ar->center, ar->radius)) {
        const glm::dvec2 p = a + t * ab;
        if (AngleInArc(std::atan2(p.y - ar->center.y, p.x - ar->center.x), *ar))
          cuts.push_back(t);
      }
    }
  }
  std::sort(cuts.begin(), cuts.end());
  cuts.erase(
      std::unique(cuts.begin(), cuts.end(),
                  [](double x, double y) { return std::abs(x - y) < 1e-4; }),
      cuts.end());

  const double tClick = glm::clamp(glm::dot(clickUV - a, ab) / len2, 0.0, 1.0);
  tLo = 0.0;
  tHi = 1.0;
  for (size_t i = 0; i + 1 < cuts.size(); i++) {
    if (tClick >= cuts[i] - 1e-6 && tClick <= cuts[i + 1] + 1e-6) {
      tLo = cuts[i];
      tHi = cuts[i + 1];
      break;
    }
  }
  return true;
}

std::optional<std::pair<glm::dvec2, glm::dvec2>>
SketchFeature::TrimSpanAt(uint32_t id, glm::dvec2 clickUV) const {
  glm::dvec2 a, ab;
  double tLo, tHi;
  if (!TrimBracket(id, clickUV, a, ab, tLo, tHi))
    return std::nullopt;
  return std::make_pair(a + tLo * ab, a + tHi * ab);
}

bool SketchFeature::TrimLineAt(uint32_t id, glm::dvec2 clickUV) {
  glm::dvec2 a, ab;
  double tLo, tHi;
  if (!TrimBracket(id, clickUV, a, ab, tLo, tHi))
    return false;
  auto at = [&](double t) { return a + t * ab; };
  if (tLo <= 1e-4 && tHi >= 1.0 - 1e-4) {
    DeleteEntity(id); // no bracketing cut — remove the whole line
    return true;
  }
  // Split off the clicked interval [tLo, tHi] and delete just that piece.
  uint32_t clicked = id;
  if (tHi < 1.0 - 1e-4)
    SplitEntity(id, at(tHi)); // id becomes [0, tHi]
  if (tLo > 1e-4)
    clicked = SplitEntity(id, at(tLo)); // new piece is [tLo, tHi]
  DeleteEntity(clicked);
  return true;
}

bool SketchFeature::TrimCurveAt(uint32_t id, glm::dvec2 clickUV) {
  const size_t idx = IndexOfEntity(id);
  if (idx == std::numeric_limits<size_t>::max())
    return false;

  // Extract center/radius and the angular window (a full circle, or the arc's
  // own sweep). Angles below are all measured relative to `base` so the window
  // is the half-open interval [base, base + span).
  glm::dvec2 center;
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

  // Interior cut offsets, plus the window ends (a circle wraps; an arc
  // doesn't).
  std::vector<double> cuts;
  if (!isCircle) {
    cuts.push_back(0.0);
    cuts.push_back(span);
  }
  for (const auto& e : entities) {
    if (sketch::EntityId(e) == id)
      continue;
    // Crossings from lines, other circles, and arcs alike — circles laid
    // over a circle split it just like lines do.
    std::vector<double> angles;
    if (auto* o = std::get_if<sketch::Line>(&e)) {
      angles = LineCircleAngles(center, radius, o->a, o->b);
    } else if (auto* oc = std::get_if<sketch::Circle>(&e)) {
      angles = CircleCircleAngles(center, radius, oc->center, oc->radius);
    } else if (auto* oa = std::get_if<sketch::Arc>(&e)) {
      for (double ang :
           CircleCircleAngles(center, radius, oa->center, oa->radius)) {
        const glm::dvec2 p(center.x + (radius * std::cos(ang)),
                           center.y + (radius * std::sin(ang)));
        if (AngleInArc(std::atan2(p.y - oa->center.y, p.x - oa->center.x), *oa))
          angles.push_back(ang);
      }
    } else {
      continue;
    }
    for (double ang : angles) {
      double off = rel(ang);
      if (off > 1e-4 && off < span - 1e-4)
        cuts.push_back(off);
      else if (isCircle && off <= 1e-4)
        cuts.push_back(off); // keep a crossing exactly at base for circles
    }
  }
  std::sort(cuts.begin(), cuts.end());
  cuts.erase(
      std::unique(cuts.begin(), cuts.end(),
                  [](double x, double y) { return std::abs(x - y) < 1e-4; }),
      cuts.end());

  const double click =
      rel(std::atan2(clickUV.y - center.y, clickUV.x - center.x));

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
  std::vector<uint32_t> generated;
  // Axis geometry comes from the datum helpers for the X/Y axes, else from the
  // axis line entity. A linked Mirror op references axisLineId either way.
  glm::dvec2 a, b;
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

  if (!link) {
    // One-shot bake: free reflected copies, no record.
    for (uint32_t sid : sourceIds) {
      const size_t si = IndexOfEntity(sid);
      if (si == std::numeric_limits<size_t>::max() || sid == axisLineId)
        continue;
      generated.push_back(AddEntity(sketch::ReflectEntity(entities[si], a, b)));
    }
    return generated;
  }

  // Linked (v51): a Mirror derived op — replayed each regeneration, so the
  // copies track their sources AND the axis. Replaces the old
  // symmetry-constraint fan-out (which flooded the constraint list and
  // fought the solver on arcs — FreeCAD issue #13551).
  sketch::DerivedOp op;
  op.kind = sketch::DerivedKind::Mirror;
  for (uint32_t sid : sourceIds)
    if (sid != axisLineId && IndexOfEntity(sid) != SIZE_MAX)
      op.sourceIds.push_back(sid);
  if (op.sourceIds.empty())
    return generated;
  op.axisEntity = axisLineId;
  derived.push_back(op);
  RegenerateDerived();
  for (const auto& in : derived.back().instances)
    generated.push_back(in.genId);
  return generated;
}

uint32_t SketchFeature::FilletCorner(uint32_t lineId1, uint32_t lineId2,
                                     double radius, std::string* whyNot) {
  using K = sketch::ConstraintKind;
  const auto fail = [&](const std::string& why) -> uint32_t {
    if (whyNot)
      *whyNot = why;
    return 0;
  };
  if (radius <= 0.0)
    return fail("fillet radius must be positive");
  const size_t i1 = IndexOfEntity(lineId1), i2 = IndexOfEntity(lineId2);
  if (i1 == std::numeric_limits<size_t>::max())
    return fail("no sketch entity with id " + std::to_string(lineId1));
  if (i2 == std::numeric_limits<size_t>::max())
    return fail("no sketch entity with id " + std::to_string(lineId2));
  auto* l1 = std::get_if<sketch::Line>(&entities[i1]);
  auto* l2 = std::get_if<sketch::Line>(&entities[i2]);
  if (!l1 || !l2)
    return fail("entity " + std::to_string(!l1 ? lineId1 : lineId2) +
                " is not a line (a fillet rounds the corner of two lines "
                "sharing an endpoint; rects arrive as four lines)");

  // The shared corner: the endpoint pair (one per line) that coincides. c1/c2
  // are the which-index (0=a, 1=b) of the corner endpoint on each line.
  const double tol = 1e-4;
  const glm::dvec2 e1[2] = {l1->a, l1->b}, e2[2] = {l2->a, l2->b};
  int c1 = -1, c2 = -1;
  for (int i = 0; i < 2 && c1 < 0; i++)
    for (int j = 0; j < 2; j++)
      if (glm::length(e1[i] - e2[j]) < tol) {
        c1 = i;
        c2 = j;
        break;
      }
  if (c1 < 0)
    return fail("the lines share no endpoint (within 1e-4) - no corner to "
                "round");

  const glm::dvec2 C = e1[c1];
  glm::dvec2 d1 = e1[1 - c1] - C, d2 = e2[1 - c2] - C;
  const double len1 = glm::length(d1), len2 = glm::length(d2);
  if (len1 < tol || len2 < tol)
    return fail("a line is zero-length");
  d1 /= len1;
  d2 /= len2;
  const double theta = std::acos(glm::clamp(glm::dot(d1, d2), -1.0, 1.0));
  if (theta < 1e-3 || theta > M_PI - 1e-3)
    return fail("the lines are parallel/collinear - no corner to round");
  const double dTan = radius / std::tan(theta / 2.0);
  if (dTan > len1 - tol || dTan > len2 - tol)
    return fail("radius " + std::to_string(radius) +
                " is too large - the fillet would consume a whole segment");
  const glm::dvec2 bis = glm::normalize(d1 + d2);
  const glm::dvec2 center = C + bis * (radius / std::sin(theta / 2.0));
  const glm::dvec2 T1 = C + d1 * dTan;
  const glm::dvec2 T2 = C + d2 * dTan;

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
  sketch::Arc arc =
      startIsT1 ? sketch::Arc{center, radius, aT1, aT1 + wrap(aT2 - aT1)}
                : sketch::Arc{center, radius, aT2, aT2 + wrap(aT1 - aT2)};
  const uint8_t w1 = static_cast<uint8_t>(c1), w2 = static_cast<uint8_t>(c2);

  // A fillet must NEVER rotate a line — it only trims to the tangent points.
  // But a line's direction can be locked only *transitively through the
  // corner* (e.g. the owner's staircase: L3 is vertical merely because its
  // top point shares X with the shared corner, via a point-pair vertical to
  // that corner). Trimming moves the corner endpoints off their old position,
  // so those constraints would drag the line into a rotation.
  //
  // Fix (construction-point re-anchoring): drop a CONSTRUCTION point at the
  // old corner C, re-point every existing constraint that referenced either
  // corner endpoint onto it (so the corner's alignments now hold the fixed
  // point instead), and add point-on-line for each line through it — keeping
  // each line aimed exactly as before, for ANY orientation, with no H/V
  // special-casing. NOTE: AddEntity may reallocate `entities`, invalidating
  // l1/l2 — both are done being dereferenced (trim above), so don't touch
  // them past here.
  sketch::Point cornerPt{C};
  cornerPt.construction = true;
  const uint32_t pId = AddEntity(cornerPt);

  // Re-point BEFORE adding the arc-connection constraints (which reuse the
  // corner endpoints and must NOT be caught).
  auto repoint = [&](sketch::PointRef& r) {
    if ((r.entity == lineId1 && r.which == w1) ||
        (r.entity == lineId2 && r.which == w2))
      r = {pId, 0};
  };
  for (auto& con : constraints) {
    repoint(con.a);
    repoint(con.b);
    repoint(con.c);
  }
  // The old corner coincident is now P=P — drop it (plus any other degenerate
  // self-coincident the re-point produced).
  constraints.erase(std::remove_if(constraints.begin(), constraints.end(),
                                   [](const sketch::Constraint& con) {
                                     return con.kind == K::Coincident &&
                                            con.a.entity == con.b.entity &&
                                            con.a.which == con.b.which;
                                   }),
                    constraints.end());

  const uint32_t arcId = AddEntity(arc);

  // Everything from here is added by THIS fillet call — the only constraints
  // the prune below is allowed to touch (pre-existing user constraints are
  // never removed).
  const size_t addedFrom = constraints.size();

  // Join the trimmed line ends to the arc endpoints; tangency does the rest.
  const uint32_t startLine = startIsT1 ? lineId1 : lineId2;
  const uint8_t startWhich = startIsT1 ? w1 : w2;
  const uint32_t endLine = startIsT1 ? lineId2 : lineId1;
  const uint8_t endWhich = startIsT1 ? w2 : w1;
  constraints.push_back(
      {K::Coincident, {startLine, startWhich}, {arcId, 1}, 0.0});
  constraints.push_back({K::Coincident, {endLine, endWhich}, {arcId, 2}, 0.0});
  constraints.push_back({K::Tangent, {lineId1, 0}, {arcId, 0}, 0.0});
  constraints.push_back({K::Tangent, {lineId2, 0}, {arcId, 0}, 0.0});

  // Each line stays collinear with the fixed corner point → direction locked.
  constraints.push_back({K::PointOnLine, {pId, 0}, {lineId1, 0}, 0.0});
  constraints.push_back({K::PointOnLine, {pId, 0}, {lineId2, 0}, 0.0});

  // Add-then-prune: in a richly-constrained corner the fillet's own web (arc
  // tangency + endpoint coincidents + the point-on-line direction locks) can
  // over-determine the corner, so a fillet-added constraint can be REDUNDANT
  // (dependent on the rest). PlaneGCS reports redundants as "conflicting",
  // a spurious red flag on a sketch that is geometrically fine.
  //
  // Solve and drop exactly one flagged constraint that WE added this call —
  // never a pre-existing user constraint — keeping the removal only when it
  // leaves the sketch clean AT THE SAME DEGREES OF FREEDOM. A genuinely-
  // redundant constraint can't change the solution (DOF unchanged), so the
  // geometry and every direction lock are preserved; a load-bearing one would
  // free a DOF and is restored. Try the auxiliary point-on-line locks before
  // the geometric tangency/coincidence. Skip if the base solve fails.
  auto flaggedSet = [](const sketch::SolveResult& r) {
    std::set<size_t> f(r.conflictingConstraints.begin(),
                       r.conflictingConstraints.end());
    f.insert(r.redundantConstraints.begin(), r.redundantConstraints.end());
    return f;
  };
  sketch::Solver base;
  base.Load(*this);
  const sketch::SolveResult r0 = base.Solve();
  const std::set<size_t> flagged0 = flaggedSet(r0);
  if (r0.ok && !flagged0.empty()) {
    const int baseDof = r0.dof;
    // Removal preference: auxiliary point-on-line first, then tangent, then
    // coincidence (drop the least structural constraint that resolves it).
    auto rank = [&](size_t i) {
      switch (constraints[i].kind) {
      case K::PointOnLine:
        return 0;
      case K::Tangent:
        return 1;
      default:
        return 2; // Coincident
      }
    };
    std::vector<size_t> cands;
    for (size_t i : flagged0)
      if (i >= addedFrom)
        cands.push_back(i);
    std::sort(cands.begin(), cands.end(),
              [&](size_t a, size_t b) { return rank(a) < rank(b); });
    for (size_t idx : cands) {
      const sketch::Constraint saved = constraints[idx];
      constraints.erase(constraints.begin() + idx);
      sketch::Solver t;
      t.Load(*this);
      const sketch::SolveResult r1 = t.Solve();
      if (r1.ok && flaggedSet(r1).empty() && r1.dof == baseDof)
        break; // redundant constraint dropped; the sketch reads clean
      constraints.insert(constraints.begin() + idx, saved); // keep it
    }
  }
  return arcId;
}

void SketchFeature::RemoveDerivedOp(size_t opIndex) {
  if (opIndex >= derived.size())
    return;
  // Delete the generated entities (DeleteEntity also drops their constraints),
  // then drop the op record.
  for (const auto& in : derived[opIndex].instances)
    DeleteEntity(in.genId);
  derived.erase(derived.begin() + opIndex);
}

size_t SketchFeature::AddExternalGeometry(const ElementRef& ref,
                                          sketch::ExternalKind kind) {
  external.push_back({ref, kind, {}}); // projected on the next Execute
  return external.size() - 1;
}

void SketchFeature::RemoveExternalGeometry(size_t extIndex) {
  if (extIndex >= external.size())
    return;
  for (uint32_t id : external[extIndex].genIds)
    DeleteEntity(id);
  external.erase(external.begin() + extIndex);
}

bool SketchFeature::RegenerateExternal(const RegenContext& ctx) {
  for (auto& ext : external) {
    // A feature can create SEVERAL bodies (an Import, a Derived copy of a
    // multi-body part): the element lives on whichever of them resolves the
    // name, not on the first one. Taking the first left every element of a
    // derived servo's second body "no longer exists".
    const Body* body = nullptr;
    std::optional<int> idx;
    bool anyBody = false;
    for (const Body& b : ctx.bodies) {
      if (b.createdBy != ext.ref.featureId)
        continue;
      anyBody = true;
      idx = ext.kind == sketch::ExternalKind::Face
                ? b.elementMap.Resolve(ext.ref.name)
                : b.elementMap.ResolveEdge(ext.ref.name);
      if (idx) {
        body = &b;
        break;
      }
    }
    if (!anyBody) {
      SetError("external geometry's body no longer exists");
      return false;
    }
    if (body == nullptr) {
      SetError("external geometry '" + ext.ref.name + "' no longer exists");
      return false;
    }
    std::vector<sketch::Entity> proj =
        sketch::ProjectElement(body->shape, ext.kind, *idx, plane);
    // Refresh in place: reuse the existing generated ids (so a user's
    // constraints to them survive), creating/deleting entities only when the
    // projected count changes.
    for (size_t i = 0; i < proj.size(); i++) {
      sketch::SetConstruction(proj[i], true);
      const size_t existing =
          i < ext.genIds.size() ? IndexOfEntity(ext.genIds[i]) : SIZE_MAX;
      if (existing != SIZE_MAX) {
        sketch::SetEntityId(proj[i], ext.genIds[i]);
        entities[existing] = std::move(proj[i]);
      } else {
        const uint32_t id = AddEntity(std::move(proj[i]));
        if (i < ext.genIds.size())
          ext.genIds[i] = id;
        else
          ext.genIds.push_back(id);
      }
    }
    while (ext.genIds.size() > proj.size()) {
      DeleteEntity(ext.genIds.back());
      ext.genIds.pop_back();
    }
  }
  return true;
}

// Resolves a face ElementRef to a work plane: find the body the referenced
// feature produced whose element map still holds the name, then extract the
// planar face's frame. nullopt if the name no longer resolves (deleted /
// renamed element) or the face isn't planar. Shared with the up-to-face
// extrude end condition (M3.3).
std::optional<SketchPlane> ResolveFacePlane(const RegenContext& ctx,
                                            const ElementRef& ref) {
  for (const Body& body : ctx.bodies) {
    if (body.createdBy != ref.featureId) {
      continue;
    }
    const auto idx = body.elementMap.Resolve(ref.name);
    if (!idx) {
      continue;
    }
    glm::dvec3 center, normal, uAxis;
    if (body.shape.FacePlane(*idx, center, normal, uAxis)) {
      return SketchPlane(center, normal, uAxis);
    }
  }
  return std::nullopt;
}

bool SketchFeature::Execute(RegenContext& ctx) {
  if (faceRef.IsSet()) {
    const auto p = ResolveFacePlane(ctx, faceRef);
    if (!p) {
      SetError("attached face '" + faceRef.name + "' no longer exists");
      return false;
    }
    plane = *p;
  } else if (planeRef != 0) {
    // A datum plane, another sketch's plane, or a Frame's XY plane (X as u):
    // the three things a primitive can stand on besides a face or a world
    // plane (docs/modeling/README.md#primitives). Whichever it is, a missing
    // or failed one is this sketch's error -- never the world plane.
    const Feature* ref = ctx.FindById(planeRef);
    if (ref == nullptr) {
      SetError("the plane this sketch is on (feature " +
               std::to_string(planeRef) + ") is gone or suppressed");
      return false;
    }
    if (!ref->GetError().empty()) {
      SetError("the plane this sketch is on, '" + ref->GetName() +
               "', failed: " + ref->GetError());
      return false;
    }
    if (const auto* datum = dynamic_cast<const DatumPlaneFeature*>(ref)) {
      plane = datum->OutputPlane();
    } else if (const auto* sk = dynamic_cast<const SketchFeature*>(ref)) {
      plane = sk->plane;
    } else if (const auto* fr = dynamic_cast<const FrameFeature*>(ref)) {
      plane = SketchPlane(fr->Origin(), fr->ZAxis(), fr->XAxis());
    } else {
      SetError("the plane this sketch is on, '" + ref->GetName() + "', is a " +
               ref->TypeName() + ", not a DatumPlane, Sketch or Frame");
      return false;
    }
  }

  // Cross-sketch linked copies re-sync from their (already executed) source.
  for (auto& op : derived) {
    if (op.kind != sketch::DerivedKind::LinkedCopy || op.sourceSketchId == 0)
      continue;
    const auto* src =
        dynamic_cast<const SketchFeature*>(ctx.FindById(op.sourceSketchId));
    if (src == nullptr) {
      SetError("linked-copy source sketch is gone - break the link to keep "
               "the copies as free geometry");
      return false;
    }
    if (!ReplayLinkedCopyFrom(op, *src)) {
      SetError("a linked-copy source entity no longer exists in '" +
               src->GetName() + "' - break the link or re-copy");
      return false;
    }
  }

  // Live external geometry: re-project each reference onto the (possibly moved)
  // plane so the projected construction entities track their source.
  if (!external.empty() && !RegenerateExternal(ctx))
    return false;

  // A text label that cannot be laid out — an unknown font, a character its
  // font has no glyph for — is this sketch's error, named, rather than a
  // label that quietly builds with letters missing
  // (docs/sketching/README.md#text).
  for (const sketch::Entity& e : entities) {
    const auto* text = std::get_if<sketch::Text>(&e);
    if (text == nullptr)
      continue;
    if (auto geo = sketch::BuildTextGeometry(*text); !geo) {
      SetError("text \"" + text->text + "\": " + geo.error());
      return false;
    }
  }

  const bool exprDriven =
      std::any_of(constraints.begin(), constraints.end(),
                  [](const sketch::Constraint& c) {
                    return sketch::IsDimension(c.kind) && !c.valueExpr.empty();
                  }) ||
      std::any_of(entityExprs.begin(), entityExprs.end(), [](const auto& kv) {
        return std::any_of(kv.second.begin(), kv.second.end(),
                           [](const auto& fe) { return !fe.second.empty(); });
      });
  // With external geometry we always re-solve, so geometry constrained to the
  // (re-projected) references follows when the source moves.
  if (!exprDriven && external.empty()) {
    return true;
  }
  // Dimension values and expression-driven entity coordinates were already
  // refreshed from the parameter table by Document::Regenerate (via
  // VisitExprs); re-solve so the rest of the sketch follows (the driven
  // coordinates themselves are solver-fixed and cannot move).
  sketch::Solver solver;
  solver.Load(*this);
  const sketch::SolveResult result = solver.Solve();
  if (!result.ok) {
    // Keep the last good entity positions instead of failing the history —
    // the conflict is the user's to resolve, downstream features can still
    // consume the previous geometry.
    LOG_WARN("Sketch '{}': expression-driven solve failed, keeping previous "
             "geometry",
             GetName());
    return true;
  }
  solver.WriteBack(entities);
  RegenerateDerived();
  return true;
}

void SketchFeature::VisitExprs(const ExprVisitor& visit) {
  for (sketch::Constraint& c : constraints) {
    if (!sketch::IsDimension(c.kind)) {
      continue;
    }
    if (c.kind == sketch::ConstraintKind::Angle) {
      // Angle values are stored in radians, but expressions (like the
      // dimension UI) speak degrees — convert around the visit.
      double degrees = glm::degrees(c.value);
      visit(c.valueExpr, degrees);
      c.value = glm::radians(degrees);
    } else {
      visit(c.valueExpr, c.value);
    }
  }
  // Expression-driven entity coordinates (Decision 2A): surface each
  // (expression, current value) pair and write the evaluated value back into
  // the entity geometry. Because Document::Regenerate runs this refresh
  // before hashing, the updated coordinates are baked into the feature JSON
  // the regen cache keys on — a variable edit invalidates the sketch.
  for (auto& [id, fields] : entityExprs) {
    const size_t idx = IndexOfEntity(id);
    if (idx == std::numeric_limits<size_t>::max()) {
      continue; // stale id (entity deleted out-of-band); harmless
    }
    for (auto& [field, expr] : fields) {
      const auto current = sketch::GetEntityField(entities[idx], field);
      if (!current) {
        continue; // field doesn't apply to this entity kind
      }
      double value = *current;
      visit(expr, value);
      sketch::SetEntityField(entities[idx], field, value);
    }
  }
}

void SketchFeature::RegenerateDerived() {
  for (auto& op : derived) {
    if (op.kind == sketch::DerivedKind::LinkedCopy && op.sourceSketchId != 0)
      continue; // cross-sketch: replayed in Execute (needs the source)

    // Mirror (v51): resolve the axis endpoints fresh each regen so the
    // copies track it. An unresolvable axis prunes the instances (same as
    // a vanished source).
    glm::dvec2 axisA{0.0}, axisB{0.0};
    if (op.kind == sketch::DerivedKind::Mirror) {
      bool axisOk = false;
      if (sketch::IsDatum(op.axisEntity)) {
        const auto ep = sketch::DatumAxisEndpoints(op.axisEntity);
        axisA = ep.first;
        axisB = ep.second;
        axisOk = true;
      } else {
        const size_t ai = IndexOfEntity(op.axisEntity);
        if (ai != std::numeric_limits<size_t>::max()) {
          if (auto* axis = std::get_if<sketch::Line>(&entities[ai])) {
            axisA = axis->a;
            axisB = axis->b;
            axisOk = true;
          }
        }
      }
      if (!axisOk) {
        for (const auto& in : op.instances)
          DeleteEntity(in.genId);
        op.instances.clear();
        continue;
      }
    }

    // Circular (v51): an optional centre REFERENCE tracks an entity point;
    // fall back to the baked coordinate when unset or unresolvable.
    glm::dvec2 circCenter = op.vec;
    if (op.kind == sketch::DerivedKind::CircularPattern &&
        op.centerEntity != 0) {
      const size_t ci = IndexOfEntity(op.centerEntity);
      if (ci != std::numeric_limits<size_t>::max())
        circCenter = sketch::EntityPointUV(entities[ci], op.centerWhich);
    }

    auto transform = [&](const sketch::Entity& src, int k) -> sketch::Entity {
      if (op.kind == sketch::DerivedKind::LinkedCopy)
        return sketch::TranslateEntity(src, op.vec);
      if (op.kind == sketch::DerivedKind::Mirror)
        return sketch::ReflectEntity(src, axisA, axisB);
      if (op.kind == sketch::DerivedKind::LinearPattern) {
        // 2D grid (v50): the flat instance index k unfolds row-major.
        // count2 == 1 degenerates to the old 1D behavior (k * vec).
        const int col = k % op.count;
        const int row = k / op.count;
        return sketch::TranslateEntity(src,
                                       static_cast<double>(col) * op.vec +
                                           static_cast<double>(row) * op.vec2);
      }
      return sketch::RotateEntity(src, circCenter,
                                  static_cast<double>(k) * op.scalar);
    };
    const int total = op.kind == sketch::DerivedKind::LinearPattern
                          ? op.count * std::max(1, op.count2)
                      : op.kind == sketch::DerivedKind::Mirror ? 2
                                                               : op.count;
    std::vector<sketch::DerivedOp::Inst> keep;
    for (uint32_t sid : op.sourceIds) {
      const size_t si = IndexOfEntity(sid);
      if (si == std::numeric_limits<size_t>::max())
        continue; // source removed → its instances get pruned below
      const sketch::Entity srcCopy = entities[si]; // copy before any AddEntity
      for (int k = 1; k < total; k++) {
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

bool SketchFeature::ReplayLinkedCopyFrom(sketch::DerivedOp& op,
                                         const SketchFeature& source) {
  std::vector<sketch::DerivedOp::Inst> keep;
  for (uint32_t sid : op.sourceIds) {
    const size_t si = source.IndexOfEntity(sid);
    if (si == std::numeric_limits<size_t>::max())
      return false; // the source entity vanished: the caller errors
    sketch::Entity g = sketch::TranslateEntity(source.entities[si], op.vec);
    uint32_t genId = 0;
    for (const auto& in : op.instances)
      if (in.sourceId == sid && in.k == 1) {
        genId = in.genId;
        break;
      }
    const size_t gi = (genId != 0) ? IndexOfEntity(genId)
                                   : std::numeric_limits<size_t>::max();
    if (gi != std::numeric_limits<size_t>::max()) {
      sketch::SetEntityId(g, genId);
      entities[gi] = g;
    } else {
      genId = AddEntity(g);
    }
    keep.push_back({sid, 1, genId});
  }
  for (const auto& in : op.instances) {
    bool kept = false;
    for (const auto& kp : keep)
      if (kp.genId == in.genId)
        kept = true;
    if (!kept)
      DeleteEntity(in.genId);
  }
  op.instances = std::move(keep);
  return true;
}

void SketchFeature::BreakLink(size_t opIndex) {
  if (opIndex >= derived.size() ||
      derived[opIndex].kind != sketch::DerivedKind::LinkedCopy)
    return;
  derived.erase(derived.begin() + static_cast<long>(opIndex));
}

std::vector<uint32_t>
SketchFeature::AddLinearPattern(const std::vector<uint32_t>& sourceIds,
                                glm::dvec2 step, int count, bool link,
                                glm::dvec2 step2, int count2) {
  std::vector<uint32_t> gen;
  count2 = std::max(1, count2);
  if (count < 2 || sourceIds.empty())
    return gen;
  if (!link) {
    for (uint32_t sid : sourceIds) {
      const size_t si = IndexOfEntity(sid);
      if (si == std::numeric_limits<size_t>::max())
        continue;
      const sketch::Entity srcCopy = entities[si];
      // Row-major grid, skipping the seed cell (0,0).
      for (int row = 0; row < count2; row++)
        for (int col = 0; col < count; col++) {
          if (row == 0 && col == 0)
            continue;
          gen.push_back(AddEntity(sketch::TranslateEntity(
              srcCopy, static_cast<double>(col) * step +
                           static_cast<double>(row) * step2)));
        }
    }
    return gen;
  }
  sketch::DerivedOp op;
  op.kind = sketch::DerivedKind::LinearPattern;
  op.sourceIds = sourceIds;
  op.vec = step;
  op.count = count;
  op.vec2 = step2;
  op.count2 = count2;
  derived.push_back(op);
  RegenerateDerived();
  for (const auto& in : derived.back().instances)
    gen.push_back(in.genId);
  return gen;
}

std::vector<uint32_t> SketchFeature::AddCircularPattern(
    const std::vector<uint32_t>& sourceIds, glm::dvec2 center, double stepAngle,
    int count, bool link, uint32_t centerEntity, uint8_t centerWhich) {
  std::vector<uint32_t> gen;
  if (count < 2 || sourceIds.empty())
    return gen;
  // A referenced centre resolves to its current point for the bake too.
  if (centerEntity != 0) {
    const size_t ci = IndexOfEntity(centerEntity);
    if (ci != std::numeric_limits<size_t>::max())
      center = sketch::EntityPointUV(entities[ci], centerWhich);
  }
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
  op.centerEntity = centerEntity;
  op.centerWhich = centerWhich;
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

bool HasUserGeometry(const SketchFeature& sketch) {
  if (!sketch.derived.empty() || !sketch.images.empty())
    return true;
  std::set<uint32_t> generated;
  for (const sketch::ExternalGeom& ex : sketch.external)
    generated.insert(ex.genIds.begin(), ex.genIds.end());
  for (const sketch::Entity& e : sketch.entities)
    if (generated.find(sketch::EntityId(e)) == generated.end())
      return true;
  return false;
}
