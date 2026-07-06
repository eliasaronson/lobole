#pragma once

#include "SketchPlane.h"
#include "core/Feature.h"

#include <glm/glm.hpp>
#include <variant>
#include <vector>

namespace sketch {

// Sketch entities live in the plane's UV space.
struct Point {
  glm::vec2 p;
};
struct Line {
  glm::vec2 a, b;
};
struct Circle {
  glm::vec2 center;
  double radius;
};
struct Rectangle {
  glm::vec2 c1, c2;
};

using Entity = std::variant<Point, Line, Circle, Rectangle>;

// A reference to a point on an entity (by index into the sketch's entity
// list). `which` selects which point: 0 = a/center/c1, 1 = b/c2. For
// whole-entity constraints (Horizontal on a line, Radius on a circle),
// `which` is ignored and `a.entity` names the entity.
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

  SketchPlane plane;
  std::vector<sketch::Entity> entities;
  std::vector<sketch::Constraint> constraints;
};
