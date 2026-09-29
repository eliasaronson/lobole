#pragma once

#include "core/Feature.h"

#include <cstdint>
#include <string>

enum class HoleKind { Simple, Counterbore, Countersink };

// Drills holes at a sketch's Point entities (modeling M8 hole wizard): each
// point is a hole center on the sketch plane, and the hole is cut into the
// bodies below along the plane's -normal. Simple = a plain bore; Counterbore
// adds a larger flat-bottomed recess at the surface; Countersink adds a
// conical recess. A far cheaper, more legible bolt-hole than a construction
// circle + cut-extrude per hole. Cut mode (subtracts from every overlapping
// body), like an extrude-Cut. Regeneration fails per-feature when the sketch
// is missing or has no points, or the diameters/depths are non-positive.
class HoleFeature : public Feature {
public:
  HoleFeature(const std::string& name, uint64_t sketchId, HoleKind kind,
              double diameter, double depth);

  std::string TypeName() const override { return "Hole"; }

  std::unique_ptr<Feature> Clone() const override {
    return std::make_unique<HoleFeature>(*this);
  }

  bool Execute(RegenContext& ctx) override;

  // A hole drilled for a thread IS threaded: `fit.kind == "tapped"` records
  // the internal thread on every bore it cut, so a screw in it engages
  // rather than clashes. Nothing else wrote one but a `Thread` feature.
  void RecordTapping(std::vector<Body>& bodies) const;

  void VisitFeatureRefs(const RefVisitor& visit) const override {
    visit(sketchId); // the hole-centre sketch
  }

  // What a Pattern of this hole repeats: the drilled tool, cut at each
  // copy (Feature::ToolShape). A hole leaves no body of its own.
  const CADShape* ToolShape() const override {
    return m_tool.IsValid() ? &m_tool : nullptr;
  }
  const ElementMap* ToolMap() const override { return &m_toolMap; }

  void VisitExprs(const ExprVisitor& visit) override {
    visit(diameterExpr, diameter);
    visit(depthExpr, depth);
  }

  uint64_t sketchId = 0;
  HoleKind kind = HoleKind::Simple;
  double diameter = 1.0;
  std::string diameterExpr;

  // What the hole is FOR (docs/modeling/holes-and-threads.md#hole).
  //
  // Empty means the same hole this feature has always cut: a raw diameter,
  // and nothing downstream knows whether 6.6 is a clearance hole for an M6 or
  // a 6.6 mm hole that happens to be the same size. Set, `diameter` is
  // DERIVED from it at regeneration — the U10 rule, a default carrying a unit
  // comes from what it acts on — and changing the thread re-derives it.
  //
  // The pitch matters only for a tapped hole and only because the drill
  // depends on it (M6 coarse taps to 5.0, M6 fine to 5.25). 0 means the
  // coarse pitch, read out of the vendored standard rather than assumed.
  std::string fitThread; // "M6"; empty = a plain diameter, as before
  std::string fitKind;   // close | normal | loose | tapped
  double fitPitch = 0.0; // mm; 0 = coarse, and only read when tapped
  double depth = 1.0; // bore depth; `through` overrides it
  std::string depthExpr;
  bool through = false; // drill far enough to pierce any body below

  // Counterbore: a `cbDiameter` recess `cbDepth` deep at the surface.
  double cbDiameter = 2.0;
  double cbDepth = 0.5;
  // Countersink: a cone widening to `csDiameter` at the surface over
  // `csDepth` (typical 82/90 deg bolt heads — the caller picks the depth).
  double csDiameter = 2.0;
  double csDepth = 0.5;

private:
  // The last-drilled tool and its names (ToolShape).
  CADShape m_tool;
  ElementMap m_toolMap;
};
