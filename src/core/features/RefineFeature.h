// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "core/Feature.h"

#include <cstdint>
#include <string>

// Merges a body's coplanar faces and the seams left by booleans/patterns into
// single faces/edges (OCC ShapeUpgrade_UnifySameDomain) — FreeCAD's PartDesign
// "Refine". A fused pattern or a boolean union leaves the shared planar faces
// split by a residual seam edge; refine unifies them so the body reads clean
// and later fillets/drafts see whole faces.
//
// The body is modified in place (createdBy stays the producer's id, like
// fillet/shell). Topology is reorganized, so the result gets fresh primitive-
// style names ("<id>/faceN") — a later feature references the refined faces.
// Regeneration fails per-feature when the body is missing or OCC produces an
// empty/invalid solid.
class RefineFeature : public Feature {
public:
  RefineFeature(const std::string& name, uint64_t bodyId);

  [[nodiscard]] std::string TypeName() const override { return "Refine"; }

  std::unique_ptr<Feature> Clone() const override {
    return std::make_unique<RefineFeature>(*this);
  }

  void CopyDerivedState(const Feature& executed) override {
    RestoreExecuted<RefineFeature>(executed);
  }

  bool Execute(RegenContext& ctx) override;

  void VisitFeatureRefs(const RefVisitor& visit) const override {
    visit(bodyId);
  }

  uint64_t bodyId = 0; // producing-feature id of the body to refine
};
