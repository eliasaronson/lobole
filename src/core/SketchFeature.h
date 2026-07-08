#pragma once

#include "SketchPlane.h"
#include "core/Feature.h"

#include <glm/glm.hpp>
#include <array>
#include <cmath>
#include <optional>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace sketch {

// Sketch entities live in the plane's UV space. Each carries a stable `id`
// (1-based; 0 means unassigned) so constraints survive insert/delete — see
// PointRef below.
// `construction`: reference geometry (centerlines, ref circles). It drives
// constraints and snapping but is excluded from the extrude profile and drawn
// dashed/dimmed.
struct Point {
  glm::vec2 p;
  uint32_t id = 0;
  bool construction = false;
};
struct Line {
  glm::vec2 a, b;
  uint32_t id = 0;
  bool construction = false;
};
struct Circle {
  glm::vec2 center;
  double radius;
  uint32_t id = 0;
  bool construction = false;
};
// An arc runs **counter-clockwise** from `startAngle` to `endAngle` (radians,
// measured from the +U axis) — the model is CCW-always; OCC's CW form is
// produced only at shape-build time. `endAngle` is normalized so the CCW sweep
// `endAngle - startAngle` lies in (0, 2π).
struct Arc {
  glm::vec2 center;
  double radius;
  double startAngle;
  double endAngle;
  uint32_t id = 0;
  bool construction = false;
};
// A full ellipse: major axis is `rotation` radians from the +U axis. `which`
// 0 addresses the centre.
struct Ellipse {
  glm::vec2 center;
  double rMajor;
  double rMinor;
  double rotation;
  uint32_t id = 0;
  bool construction = false;
};

// Rectangles are not a primitive: they decompose into four Lines with shared
// corners (see SketchFeature::AddRectangle) so every side is editable and
// trimmable. Legacy files with a rigid "rect" entity are migrated on load.
using Entity = std::variant<Point, Line, Circle, Arc, Ellipse>;

// Builds an ellipse from three clicks: centre, a point on the major axis (sets
// rMajor + rotation), and a third point whose perpendicular distance to the
// major axis sets rMinor.
inline Ellipse EllipseFromClicks(glm::vec2 center, glm::vec2 majorEnd,
                                 glm::vec2 minorPt) {
  const glm::vec2 maj = majorEnd - center;
  const double rMajor = glm::length(maj);
  const double rotation = std::atan2(maj.y, maj.x);
  // Perpendicular (minor-axis) direction, and the third point's distance to it.
  const glm::vec2 minorDir(-std::sin(rotation), std::cos(rotation));
  const double rMinor = std::abs(glm::dot(minorPt - center, minorDir));
  return Ellipse{center, rMajor, rMinor, rotation};
}

// UV of a point on the ellipse at parametric angle `t` (before/after rotation).
inline glm::vec2 EllipsePointUV(const Ellipse& e, double t) {
  const double x = e.rMajor * std::cos(t), y = e.rMinor * std::sin(t);
  const double c = std::cos(e.rotation), s = std::sin(e.rotation);
  return e.center + glm::vec2(static_cast<float>(c * x - s * y),
                              static_cast<float>(s * x + c * y));
}

// UV of one of an arc's referenced points: which 0 = center, 1 = start, 2 = end
// (start/end matching CCW startAngle/endAngle). Mirrors Circle's center = 0.
inline glm::vec2 ArcPointUV(const Arc& arc, uint8_t which) {
  const float r = static_cast<float>(arc.radius);
  if (which == 1)
    return arc.center + r * glm::vec2(std::cos(arc.startAngle),
                                      std::sin(arc.startAngle));
  if (which == 2)
    return arc.center +
           r * glm::vec2(std::cos(arc.endAngle), std::sin(arc.endAngle));
  return arc.center;
}

// The stable id of an entity, regardless of which variant alternative it holds.
inline uint32_t EntityId(const Entity& e) {
  return std::visit([](const auto& g) { return g.id; }, e);
}
inline void SetEntityId(Entity& e, uint32_t id) {
  std::visit([id](auto& g) { g.id = id; }, e);
}
inline bool IsConstruction(const Entity& e) {
  return std::visit([](const auto& g) { return g.construction; }, e);
}
inline void SetConstruction(Entity& e, bool c) {
  std::visit([c](auto& g) { g.construction = c; }, e);
}

// Builds a CCW arc centred at `center`, starting at `start`, sweeping CCW to
// the ray toward `endDir`. Radius = |start - center|; the sweep is normalized
// into (0, 2π].
inline Arc ArcCenterStartEnd(glm::vec2 center, glm::vec2 start,
                             glm::vec2 endDir) {
  const double r = glm::length(start - center);
  double a0 = std::atan2(start.y - center.y, start.x - center.x);
  double a1 = std::atan2(endDir.y - center.y, endDir.x - center.x);
  while (a1 <= a0)
    a1 += 2.0 * M_PI;
  return Arc{center, r, a0, a1};
}

// Builds the CCW arc through three points: from `p0` to `p1`, bulging through
// `through`. Returns nullopt if the points are collinear (no circumcircle).
inline std::optional<Arc> ArcFromThreePoints(glm::vec2 p0, glm::vec2 through,
                                             glm::vec2 p1) {
  const double ax = p0.x, ay = p0.y, bx = through.x, by = through.y, cx = p1.x,
               cy = p1.y;
  const double d = 2.0 * (ax * (by - cy) + bx * (cy - ay) + cx * (ay - by));
  if (std::abs(d) < 1e-9)
    return std::nullopt;
  const double a2 = ax * ax + ay * ay, b2 = bx * bx + by * by,
               c2 = cx * cx + cy * cy;
  glm::vec2 center(
      static_cast<float>((a2 * (by - cy) + b2 * (cy - ay) + c2 * (ay - by)) / d),
      static_cast<float>((a2 * (cx - bx) + b2 * (ax - cx) + c2 * (bx - ax)) / d));
  const double r = glm::length(p0 - center);
  auto ang = [&](glm::vec2 p) {
    return std::atan2(p.y - center.y, p.x - center.x);
  };
  auto wrap = [](double x) {
    while (x < 0)
      x += 2.0 * M_PI;
    while (x >= 2.0 * M_PI)
      x -= 2.0 * M_PI;
    return x;
  };
  const double a0 = ang(p0), a1 = ang(p1), am = ang(through);
  const double sweepP0 = wrap(a1 - a0); // CCW p0 → p1
  if (wrap(am - a0) <= sweepP0)         // `through` lies on that CCW span
    return Arc{center, r, a0, a0 + sweepP0};
  return Arc{center, r, a1, a1 + (2.0 * M_PI - sweepP0)}; // the other way
}

// Reflects a point across the infinite line through a and b.
inline glm::vec2 ReflectPoint(glm::vec2 p, glm::vec2 a, glm::vec2 b) {
  glm::vec2 d = b - a;
  float len2 = glm::dot(d, d);
  if (len2 < 1e-12f)
    return p; // degenerate axis: identity
  float t = glm::dot(p - a, d) / len2; // projection param along the axis
  glm::vec2 foot = a + t * d;
  return foot + (foot - p); // p mirrored through its foot on the axis
}

// Reflects an entity across the line a→b, preserving id/construction and the
// arc CCW-always invariant. A reflection reverses orientation, so an arc's
// swept endpoints are reflected and its start/end angles recomputed (the old
// start maps to the new *end*).
inline Entity ReflectEntity(const Entity& e, glm::vec2 a, glm::vec2 b) {
  Entity out = std::visit(
      [&](const auto& g) -> Entity {
        using T = std::decay_t<decltype(g)>;
        if constexpr (std::is_same_v<T, Point>)
          return Point{ReflectPoint(g.p, a, b)};
        else if constexpr (std::is_same_v<T, Line>)
          return Line{ReflectPoint(g.a, a, b), ReflectPoint(g.b, a, b)};
        else if constexpr (std::is_same_v<T, Circle>)
          return Circle{ReflectPoint(g.center, a, b), g.radius};
        else if constexpr (std::is_same_v<T, Arc>) {
          // Reflected start becomes the new end (winding flips).
          glm::vec2 c = ReflectPoint(g.center, a, b);
          glm::vec2 s = ReflectPoint(ArcPointUV(g, 1), a, b);
          glm::vec2 en = ReflectPoint(ArcPointUV(g, 2), a, b);
          double a0 = std::atan2(en.y - c.y, en.x - c.x);
          double a1 = std::atan2(s.y - c.y, s.x - c.x);
          while (a1 <= a0)
            a1 += 2.0 * M_PI; // CCW-always
          return Arc{c, g.radius, a0, a1};
        } else { // Ellipse: reflect the centre and the major-axis direction
          glm::vec2 c = ReflectPoint(g.center, a, b);
          glm::vec2 maj = ReflectPoint(EllipsePointUV(g, 0.0), a, b);
          return Ellipse{c, g.rMajor, g.rMinor,
                         std::atan2(maj.y - c.y, maj.x - c.x)};
        }
      },
      e);
  SetEntityId(out, 0); // caller stamps a fresh id via AddEntity
  SetConstruction(out, IsConstruction(e));
  return out;
}

// Translates an entity by `t`, preserving id/construction (angles unchanged).
inline Entity TranslateEntity(const Entity& e, glm::vec2 t) {
  Entity out = std::visit(
      [&](const auto& g) -> Entity {
        using T = std::decay_t<decltype(g)>;
        if constexpr (std::is_same_v<T, Point>)
          return Point{g.p + t};
        else if constexpr (std::is_same_v<T, Line>)
          return Line{g.a + t, g.b + t};
        else if constexpr (std::is_same_v<T, Circle>)
          return Circle{g.center + t, g.radius};
        else if constexpr (std::is_same_v<T, Arc>)
          return Arc{g.center + t, g.radius, g.startAngle, g.endAngle};
        else // Ellipse
          return Ellipse{g.center + t, g.rMajor, g.rMinor, g.rotation};
      },
      e);
  SetEntityId(out, 0);
  SetConstruction(out, IsConstruction(e));
  return out;
}

// Rotates an entity by `ang` (radians, CCW) about `center`, preserving
// id/construction and the arc CCW-always invariant (adding `ang` to both arc
// angles keeps the sweep).
inline Entity RotateEntity(const Entity& e, glm::vec2 center, double ang) {
  const float c = static_cast<float>(std::cos(ang)),
              s = static_cast<float>(std::sin(ang));
  auto rot = [&](glm::vec2 p) {
    glm::vec2 d = p - center;
    return center + glm::vec2(c * d.x - s * d.y, s * d.x + c * d.y);
  };
  Entity out = std::visit(
      [&](const auto& g) -> Entity {
        using T = std::decay_t<decltype(g)>;
        if constexpr (std::is_same_v<T, Point>)
          return Point{rot(g.p)};
        else if constexpr (std::is_same_v<T, Line>)
          return Line{rot(g.a), rot(g.b)};
        else if constexpr (std::is_same_v<T, Circle>)
          return Circle{rot(g.center), g.radius};
        else if constexpr (std::is_same_v<T, Arc>)
          return Arc{rot(g.center), g.radius, g.startAngle + ang,
                     g.endAngle + ang};
        else // Ellipse
          return Ellipse{rot(g.center), g.rMajor, g.rMinor, g.rotation + ang};
      },
      e);
  SetEntityId(out, 0);
  SetConstruction(out, IsConstruction(e));
  return out;
}

// UV of the point a ref addresses, for any alternative: Point → p; Line →
// a (which 0) / b (which 1); Circle → center; Arc → center/start/end per
// ArcPointUV. Single source of truth for RefUV / EntityPoints / EntityDots.
inline glm::vec2 EntityPointUV(const Entity& e, uint8_t which) {
  return std::visit(
      [which](const auto& g) -> glm::vec2 {
        using T = std::decay_t<decltype(g)>;
        if constexpr (std::is_same_v<T, Point>)
          return g.p;
        else if constexpr (std::is_same_v<T, Line>)
          return which == 1 ? g.b : g.a;
        else if constexpr (std::is_same_v<T, Circle>)
          return g.center;
        else if constexpr (std::is_same_v<T, Arc>)
          return ArcPointUV(g, which);
        else // Ellipse: only the centre (which 0)
          return g.center;
      },
      e);
}

// A reference to a point on an entity. `entity` is the entity's stable **id**
// (not its index in the list — that shifts on insert/delete). `which` selects
// which point: Line 0 = a, 1 = b; Circle 0 = center; Arc 0 = center, 1 = start,
// 2 = end. For whole-entity constraints (Horizontal on a line, Radius on a
// circle/arc), `which` is ignored and `a.entity` names the entity.
struct PointRef {
  uint32_t entity = 0;
  uint8_t which = 0;
};

enum class ConstraintKind {
  Coincident,    // a, b are two points made equal
  Horizontal,    // a.entity is a line: its endpoints share y
  Vertical,      // a.entity is a line: its endpoints share x
  Parallel,      // a.entity, b.entity are two lines
  Perpendicular, // a.entity, b.entity are two lines
  Equal,         // a.entity, b.entity: equal length (lines) or radius (circles)
  Distance,      // a, b are points: |a-b| = value (driving dimension)
  Radius,        // a.entity is a circle: radius = value
  Angle,         // a.entity, b.entity are two lines: angle = value (radians)
  Tangent,       // a.entity is a line, b.entity is a circle
  PointOnLine,   // a is a point, b.entity is a line
  Symmetric,     // a, b are points symmetric about the line c.entity
  Midpoint,      // a is a point at the midpoint of the line b.entity
  Fixed,         // a is a point (or entity) pinned in place
  Concentric,    // a.entity, b.entity are circles sharing a center
  Collinear,     // a.entity, b.entity are two lines on the same infinite line
  DistanceX,     // a, b are points: b.x - a.x = value (driving dimension)
  DistanceY,     // a, b are points: b.y - a.y = value (driving dimension)
  Diameter,      // a.entity is a circle: diameter = value (radius = value/2)
};

// Which kinds are driving dimensions (carry an editable numeric value + a
// canvas annotation with a `labelOffset`).
inline bool IsDimension(ConstraintKind k) {
  return k == ConstraintKind::Distance || k == ConstraintKind::DistanceX ||
         k == ConstraintKind::DistanceY || k == ConstraintKind::Radius ||
         k == ConstraintKind::Diameter || k == ConstraintKind::Angle;
}

struct Constraint {
  ConstraintKind kind;
  PointRef a, b;
  double value = 0.0;         // Distance / Radius / Angle dimension
  PointRef c;                 // third ref (Symmetric: the symmetry line)
  glm::vec2 labelOffset{0.0f}; // dimension annotation placement (UV)
};

enum class DerivedKind { LinearPattern, CircularPattern };

// A *linked* generative op: it derives geometry from `sourceIds` and is replayed
// by SketchFeature::RegenerateDerived when the sources change. `instances` maps
// each (sourceId, k) to the stable id of the generated entity so refs survive a
// count change (the sketch-local analogue of SolveSpace's remap table). Baked
// ops leave no record.
struct DerivedOp {
  DerivedKind kind;
  std::vector<uint32_t> sourceIds;
  glm::vec2 vec{0.0f}; // Linear: per-step translation. Circular: centre (UV).
  double scalar = 0.0; // Circular: step angle (radians). Linear: unused.
  int count = 2;       // total instances including the seed (count-1 generated)
  struct Inst {
    uint32_t sourceId = 0;
    int k = 0; // instance index 1..count-1
    uint32_t genId = 0;
  };
  std::vector<Inst> instances;
};

} // namespace sketch

// A 2D sketch in the parametric history. Owns its plane as a baked value —
// even when created on a model face, only the face's plane (center, normal)
// at creation time is stored, not a reference to the face itself. Persistent
// face references need topological naming (later milestone), so editing
// upstream features does NOT move dependent sketches yet.
//
// Contributes no bodies; downstream features (Extrude) consume its closed
// profiles, and the viewport renders its entities as line geometry.
class SketchFeature : public Feature {
public:
  SketchFeature(const std::string& name, const SketchPlane& plane);

  std::string TypeName() const override { return "Sketch"; }
  bool Execute(RegenContext&) override { return true; }

  // Appends `e` with a freshly stamped stable id and returns that id.
  uint32_t AddEntity(sketch::Entity e);
  // Index of the entity with the given id, or SIZE_MAX if none. Linear scan —
  // sketches hold few entities, and this avoids a desync-prone cached map.
  size_t IndexOfEntity(uint32_t id) const;
  // Removes the entity with `id` and cleans up dependent constraints
  // (transfer to a coincident survivor, transitive coincidence repair, orphan
  // sweep). Returns the number of *non-trivial* constraints dropped (i.e. not
  // Coincident/Horizontal/Vertical), for a user-facing warning; the entity is
  // still removed. Returns 0 and does nothing if `id` is unknown.
  size_t DeleteEntity(uint32_t id);

  // Adds an axis-aligned rectangle as four real lines with shared corner
  // points: 4 lines + 4 corner coincidents + 2 horizontal + 2 vertical
  // constraints. Corners derive from the opposite corners c1, c2. Returns the
  // four line ids (bottom, right, top, left). This is the modern replacement
  // for the rigid `sketch::Rectangle` entity — each side is individually
  // editable and trimmable.
  std::array<uint32_t, 4> AddRectangle(glm::vec2 c1, glm::vec2 c2);

  // Splits the line `id` at point P (assumed on the segment). Piece 1 keeps
  // `id` (its far endpoint becomes P); piece 2 is a new line P -> original-end.
  // Constraints transfer per the FreeCAD lines-only policy: the far-endpoint
  // refs move to piece 2, whole-edge distances re-anchor to span both pieces,
  // H/V/parallel/perpendicular are copied onto piece 2 (so it stays aligned),
  // and a coincident joins the two pieces at the cut. Returns piece 2's id, or
  // 0 if `id` isn't a line or P is at an endpoint (degenerate).
  uint32_t SplitEntity(uint32_t id, glm::vec2 P);

  // Trims the line `id` at `clickUV`: removes the sub-segment the click falls
  // in, bounded by the line's intersections with other lines (via SplitEntity +
  // DeleteEntity). With no bracketing intersection the whole line is deleted.
  // Returns true if anything changed. Lines only — circles are ignored here
  // (trimming a circle produces an arc; deferred to the arcs milestone).
  bool TrimLineAt(uint32_t id, glm::vec2 clickUV);

  // Trims the circle or arc `id` at `clickUV`: removes the angular span the
  // click falls in, bounded by the curve's intersections with other lines (and
  // the arc's own endpoints). A trimmed circle becomes one arc (the complement
  // of the removed span, keeping the id); a trimmed arc becomes up to two arcs
  // (the first keeps the id, the rest are new entities). With no bracketing
  // crossing the whole curve is deleted. Returns true if anything changed.
  bool TrimCurveAt(uint32_t id, glm::vec2 clickUV);

  // The span (two UV endpoints) that TrimLineAt would remove for a click on
  // line `id` at `clickUV` — the sub-segment bounded by neighbouring crossings.
  // For a hover preview so the user sees exactly what trim will delete. Nullopt
  // if `id` isn't a line.
  std::optional<std::pair<glm::vec2, glm::vec2>>
  TrimSpanAt(uint32_t id, glm::vec2 clickUV) const;

  // Mirrors `sourceIds` across the line entity `axisLineId`, appending the
  // reflected copies (fresh ids) and returning their ids. When `link` is true
  // it also adds constraints tying each copy to its source so the mirror tracks
  // edits (Symmetric on line/arc endpoints, Equal+Symmetric on circle/arc
  // centres; a source point already on the axis gets a Coincident instead).
  // `link=false` bakes an unlinked copy. No-op (empty) if the axis isn't a line.
  std::vector<uint32_t> MirrorEntities(const std::vector<uint32_t>& sourceIds,
                                       uint32_t axisLineId, bool link);

  // Rounds the corner where line `lineId1` and `lineId2` meet with a tangent arc
  // of the given radius: trims both lines to the tangent points, inserts the
  // arc, and adds two Tangent + two Coincident constraints (removing the old
  // corner coincident). Returns the new arc's id, or 0 if the lines don't share
  // a corner, are parallel, or the radius is too large for either segment.
  uint32_t FilletCorner(uint32_t lineId1, uint32_t lineId2, double radius);

  // Patterns of `sourceIds`. Linear: `count` instances (incl. the seed) each
  // stepped by `step` in UV. Circular: `count` instances stepped by `stepAngle`
  // (radians) about `center`. `link=false` bakes unlinked copies and returns
  // their ids; `link=true` records a DerivedOp (regenerates on source edits, id
  // stable across count changes) and returns the generated ids.
  std::vector<uint32_t> AddLinearPattern(const std::vector<uint32_t>& sourceIds,
                                         glm::vec2 step, int count, bool link);
  std::vector<uint32_t>
  AddCircularPattern(const std::vector<uint32_t>& sourceIds, glm::vec2 center,
                     double stepAngle, int count, bool link);

  // Offsets the closed loop formed by `chainIds` by `distance` (signed; corner
  // treatment via `joinArc` — arcs at convex corners, else extend/trim),
  // appending the offset boundary as new entities and returning their ids.
  // Empty if the chain isn't a single closed loop or OCC fails. Always bakes
  // (unlinked); offset is not yet a regenerating DerivedOp.
  std::vector<uint32_t> OffsetChain(const std::vector<uint32_t>& chainIds,
                                    double distance, bool joinArc);

  // Replays every linked DerivedOp: recomputes each generated entity from its
  // (current) source, keeping generated ids stable — reusing the entity for a
  // surviving (source, k), creating new ones when count grows, deleting the
  // surplus when it shrinks. Call after the constraint solve on any edit.
  void RegenerateDerived();

  SketchPlane plane;
  std::vector<sketch::Entity> entities;
  std::vector<sketch::Constraint> constraints;
  std::vector<sketch::DerivedOp> derived; // linked generative ops (replayed)
  uint32_t nextEntityId = 1; // next stable id to hand out (1-based; 0 = none)

private:
  // Shared by TrimLineAt/TrimSpanAt: for line `id` at `clickUV`, outputs the
  // line origin `a`, direction `ab`, and the [tLo,tHi] span to remove. Returns
  // false if `id` isn't a line.
  bool TrimBracket(uint32_t id, glm::vec2 clickUV, glm::vec2& a, glm::vec2& ab,
                   float& tLo, float& tHi) const;
};
