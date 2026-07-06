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
};
