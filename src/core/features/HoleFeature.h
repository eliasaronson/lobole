// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "core/Feature.h"

#include <cstdint>
#include <optional>
#include <string>

enum class HoleKind { Simple, Counterbore, Countersink };

// How a hole across the print direction is printed
// (docs/modeling/holes-and-threads.md#printed-holes): round, or with its roof
// raised to a 45-degree point (Teardrop) or to a flat no wider than the
// process bridges (TeardropFlat), so no face over it looks down past 45.
enum class HolePrinted { Round, Teardrop, TeardropFlat };
// "round" | "teardrop" | "teardrop_flat"
[[nodiscard]] const char* HolePrintedName(HolePrinted p);
[[nodiscard]] std::optional<HolePrinted>
HolePrintedFromName(const std::string& name);
[[nodiscard]] const char* HolePrintedNames();

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

  void CopyDerivedState(const Feature& executed) override {
    RestoreExecuted<HoleFeature>(executed);
  }

  bool Execute(RegenContext& ctx) override;

  // A hole drilled for a thread IS threaded: `fit.kind == "tapped"` records
  // the internal thread on every bore it cut, so a screw in it engages
  // rather than clashes. Nothing else wrote one but a `Thread` feature.
  void RecordTapping(std::vector<Body>& bodies) const;
  // Every bore it cut, as a HoleMark on its body: what CAM reads of a
  // hole's point and thread, and of a hole no operation cuts.
  void RecordHoles(std::vector<Body>& bodies) const;

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
    visit(threadDepthExpr, threadDepth);
  }

  // The drill point at a blind floor and how deep a tapped fit is threaded,
  // as cut: the stated value, or the one derived when it is 0
  // (docs/modeling/holes-and-threads.md#hole). Valid after Execute.
  [[nodiscard]] double PointAngleUsed() const;
  [[nodiscard]] double ThreadDepthUsed() const;

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

  // The included angle of the drill's point, degrees, that a BLIND hole's
  // floor is modelled with: a cone under the bore, its face role `point`.
  // 0 = derived: a hole with a fit is drilled to a drill table's size, and
  // those tables are ISO 235 jobber drills, whose point is 118 degrees; a
  // plain diameter says nothing about how it is made and keeps its flat
  // floor. 180 states a flat floor. Read by nothing on a through hole.
  double pointAngleDeg = 0.0;
  // A tapped fit's thread depth from the mouth, mm. 0 = derived: the drilled
  // depth less the three pitches a blind-hole tap's lead (ISO 8830 form C,
  // 2-3 threads) cuts no full thread in; the whole bore when through. Refused
  // without a tapped fit, and deeper than the hole.
  double threadDepth = 0.0;
  std::string threadDepthExpr;

  // A hole printed across its layers
  // (docs/modeling/holes-and-threads.md#printed-holes): its roof raised to a
  // point, or to a flat, toward the part's print direction
  // (RegenContext::printUp). Refused on a part with no print process, on a hole
  // along the print direction, and with a countersink.
  HolePrinted printed = HolePrinted::Round;

  // Counterbore: a `cbDiameter` recess `cbDepth` deep at the surface.
  double cbDiameter = 2.0;
  double cbDepth = 0.5;
  // Countersink: a cone widening to `csDiameter` at the surface over
  // `csDepth` (typical 82/90 deg bolt heads — the caller picks the depth).
  double csDiameter = 2.0;
  double csDepth = 0.5;

private:
  void NameByRole(const CADShape& tool, const std::vector<glm::dvec3>& centers,
                  const std::vector<uint32_t>& entities, const glm::dvec3& axis,
                  double boreDepth, ElementMap& out) const;

  // The roof's direction in the section across the bore (unit, normal to
  // `axis`) when the hole is printed as a teardrop; the zero vector for a
  // round one.
  glm::dvec3 m_roofUp{0.0};

  // The sketch entities whose hole has no floor in material for a point to
  // be on (drilled on into a pocket): their HoleMark says flat.
  std::vector<uint32_t> m_unpointed;
  // The last-drilled tool and its names (ToolShape).
  CADShape m_tool;
  ElementMap m_toolMap;
};
