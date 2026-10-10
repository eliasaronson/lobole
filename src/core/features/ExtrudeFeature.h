// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "core/ElementMap.h"
#include "core/Feature.h"
#include "core/sketch/SketchProfile.h" // sketch::ThinSide

#include <cstdint>

enum class ExtrudeMode { NewBody, Add, Cut };

// End condition (M3.2/M3.3): Blind sweeps `distance` from the sketch plane;
// Symmetric centers `distance` on it; TwoSided sweeps `distance` forward and
// `distance2` backward; UpToFace sweeps to a referenced planar face
// (endTarget), following it across upstream edits. ThroughAll (v52, Cut
// only) sweeps past every body's extent, ignoring `distance`; `reversed`
// picks the side like a blind cut. All build ONE prism of the computed total
// length translated back along the normal, so element names are identical to
// a blind extrude's.
enum class ExtrudeEnd { Blind, Symmetric, TwoSided, UpToFace, ThroughAll };

// Extrudes a referenced SketchFeature's closed profile into a solid.
//   NewBody: append the prism as its own body
//   Add:     fuse the prism into an existing body (a boss)
//   Cut:     subtract the prism from existing bodies (a pocket/hole)
// References its sketch by stable id, so it survives feature reordering and
// undo. Regeneration fails (stopping the history there) if the sketch is
// missing or its profile is not a closed loop.
class ExtrudeFeature : public Feature {
public:
  ExtrudeFeature(const std::string& name, uint64_t sketchId, double distance,
                 ExtrudeMode mode, bool reversed = false);

  std::string TypeName() const override { return "Extrude"; }

  std::unique_ptr<Feature> Clone() const override {
    return std::make_unique<ExtrudeFeature>(*this);
  }

  bool Execute(RegenContext& ctx) override;
  void VisitFeatureRefs(const RefVisitor& visit) const override {
    visit(sketchId);
    VisitRefs(endTarget, visit); // "up to face"
  }

  const CADShape* ToolShape() const override {
    return (mode == ExtrudeMode::Cut || mode == ExtrudeMode::Add) &&
                   m_tool.IsValid()
               ? &m_tool
               : nullptr;
  }
  const ElementMap* ToolMap() const override { return &m_toolMap; }
  bool ToolAdds() const override { return mode == ExtrudeMode::Add; }

  void VisitExprs(const ExprVisitor& visit) override {
    visit(distanceExpr, distance);
    visit(distance2Expr, distance2);
    visit(thinThicknessExpr, thinThickness);
    visit(twistExpr, twistDeg);
  }

  // A twist is a Blind or Symmetric solid extrude's: the other end modes
  // and a thin wall are refused here, naming what is accepted, so the tool
  // window cannot accept one and the RPC cannot add one; Execute fails the
  // same way for a file that carries one.
  [[nodiscard]] std::string WhyIncomplete() const override;

  uint64_t sketchId = 0;
  double distance = 1.0;
  // Optional expression driving `distance` (empty = plain literal),
  // evaluated against the document's parameters on every regeneration.
  std::string distanceExpr;
  // The backward side of a TwoSided extrude (with its expression twin).
  double distance2 = 1.0;
  std::string distance2Expr;
  ExtrudeEnd endMode = ExtrudeEnd::Blind;
  // UpToFace target: a planar face by stable element name (toponaming).
  // Planar only in v1 — a curved target needs a trimmed boolean, not a
  // distance.
  ElementRef endTarget;
  // Thin/wall extrude (M3.4): every loop becomes a hollow wall instead of a
  // filled profile. Composes with any end condition.
  bool thin = false;
  double thinThickness = 1.0;
  std::string thinThicknessExpr;
  sketch::ThinSide thinSide = sketch::ThinSide::Out;
  bool reversed = false;
  // Twisted extrude (v168, docs/modeling/README.md#twisted-extrudes): the
  // profile turns `twistDeg` about the sketch normal through the sketch
  // origin over the length, right-handed about the direction of travel when
  // positive. 0 = a plain prism. `herringbone` climbs the first half at that
  // rate and comes back over the second (a V; ignored at zero twist).
  double twistDeg = 0.0;
  std::string twistExpr;
  bool herringbone = false;
  ExtrudeMode mode = ExtrudeMode::NewBody;
  // Region selection (U2): each entry is one selected region — the sorted
  // entity ids of its outer loop (sketch::LoopEntityIds). Empty = the whole
  // sketch (every region), the pre-v34 behavior. A region that no longer
  // matches a loop fails regeneration with a reattach-style error.
  std::vector<std::vector<uint32_t>> regions;

  // The last-swept cut or add prism and its face/edge names
  // (Feature::ToolShape).
  CADShape m_tool;
  ElementMap m_toolMap;
};
