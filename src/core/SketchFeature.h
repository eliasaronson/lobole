#pragma once

#include "SketchPlane.h"
#include "core/Feature.h"

#include <glm/glm.hpp>
#include <array>
#include <variant>
#include <vector>

namespace sketch {

// Sketch entities live in the plane's UV space. Each carries a stable `id`
// (1-based; 0 means unassigned) so constraints survive insert/delete — see
// PointRef below.
struct Point {
  glm::vec2 p;
  uint32_t id = 0;
};
struct Line {
  glm::vec2 a, b;
  uint32_t id = 0;
};
struct Circle {
  glm::vec2 center;
  double radius;
  uint32_t id = 0;
};

// Rectangles are not a primitive: they decompose into four Lines with shared
// corners (see SketchFeature::AddRectangle) so every side is editable and
// trimmable. Legacy files with a rigid "rect" entity are migrated on load.
using Entity = std::variant<Point, Line, Circle>;

// The stable id of an entity, regardless of which variant alternative it holds.
inline uint32_t EntityId(const Entity& e) {
  return std::visit([](const auto& g) { return g.id; }, e);
}
inline void SetEntityId(Entity& e, uint32_t id) {
  std::visit([id](auto& g) { g.id = id; }, e);
}

// A reference to a point on an entity. `entity` is the entity's stable **id**
// (not its index in the list — that shifts on insert/delete). `which` selects
// which point: 0 = a/center, 1 = b. For whole-entity constraints
// (Horizontal on a line, Radius on a circle), `which` is ignored and
// `a.entity` names the entity.
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
};

struct Constraint {
  ConstraintKind kind;
  PointRef a, b;
  double value = 0.0; // Distance / Radius dimension
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

  SketchPlane plane;
  std::vector<sketch::Entity> entities;
  std::vector<sketch::Constraint> constraints;
  uint32_t nextEntityId = 1; // next stable id to hand out (1-based; 0 = none)
};
