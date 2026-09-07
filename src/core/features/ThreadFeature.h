#pragma once

#include "core/Feature.h"

#include <glm/glm.hpp>

// Cosmetic thread (docs/modeling/thread-rib.md Part A): an annotation on a
// cylindrical face — external boss or hole wall — that leaves the geometry
// untouched and draws the drafting convention instead (dashed thread-depth
// circles + crest lines, rendered by the UI from the session `overlay` this
// feature computes each regeneration). The designation drives pitch via the
// ISO tables (core/ThreadStandards); a custom designation keeps the manual
// pitch. `modeled` (a real helical cut) is reserved in the schema but not
// implemented yet — setting it errors the feature without touching bodies.
class ThreadFeature : public Feature {
public:
  ThreadFeature(const std::string& name, ElementRef face,
                std::string designation, double pitch);

  std::string TypeName() const override { return "Thread"; }

  std::unique_ptr<Feature> Clone() const override {
    return std::make_unique<ThreadFeature>(*this);
  }

  bool Execute(RegenContext& ctx) override;
  void VisitFeatureRefs(const RefVisitor& visit) const override {
    VisitRefs(face, visit); // the cylindrical face being threaded
  }

  void VisitExprs(const ExprVisitor& visit) override {
    visit(lengthExpr, length);
  }

  ElementRef face;         // a cylindrical face, by stable toponame
  std::string designation; // "M8", "M8x1", or free text (custom)
  double pitch = 1.25;
  std::string threadClass; // display only ("6H", "6g"); no geometry meaning
  double length = 0.0;     // threaded length from the face's start; 0 = full
  std::string lengthExpr;
  bool modeled = false; // reserved: real helical cut (not implemented)

  // Session-only, rebuilt by Execute (never serialized): what the UI draws.
  struct Overlay {
    bool valid = false;
    glm::vec3 origin{0.0f};          // a point on the cylinder axis
    glm::vec3 dir{0.0f, 0.0f, 1.0f}; // unit axis
    float radius = 0.0f;     // dashed-circle radius (thread depth convention)
    float faceRadius = 0.0f; // the face's own radius (crest lines)
    float v0 = 0.0f, v1 = 0.0f; // threaded span along dir, relative to origin
    bool external = true;       // boss (true) vs hole wall (false)
  };
  Overlay overlay;
};
