// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "core/ElementMap.h"
#include "core/Feature.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// Hollows one body into a thin shell (modeling M8): the faces named by
// `openFaces` (stable ElementRefs into the body's element map) are removed,
// and the remaining boundary is offset inward by `thickness` — the classic
// "make it a wall" operation (a box with its top removed becomes a bin). All
// refs must address the same body (the one produced by ref.featureId). With
// no open faces the whole body is hollowed into a closed shell.
//
// The body is modified in place (createdBy stays the producer's id, like
// fillet/extrude-Add). Shelling reorganizes topology, so the result gets
// fresh primitive-style names ("<id>/faceN") — a later fillet references the
// shelled faces, not the pre-shell ones. Regeneration fails per-feature when
// a ref no longer resolves or OCC rejects the offset (wall thicker than the
// geometry allows).
class ShellFeature : public Feature {
public:
  ShellFeature(const std::string& name, uint64_t bodyId,
               std::vector<ElementRef> openFaces, double thickness);

  std::string TypeName() const override { return "Shell"; }

  std::unique_ptr<Feature> Clone() const override {
    return std::make_unique<ShellFeature>(*this);
  }

  void CopyDerivedState(const Feature& executed) override {
    RestoreExecuted<ShellFeature>(executed);
  }

  bool Execute(RegenContext& ctx) override;
  [[nodiscard]] std::string WhyRefusedOn(const std::vector<Body>& input,
                                         const Document& doc) const override;

  void VisitFeatureRefs(const RefVisitor& visit) const override {
    visit(bodyId);
    VisitRefs(openFaces, visit);
  }

  void VisitExprs(const ExprVisitor& visit) override {
    visit(thicknessExpr, thickness);
  }

  uint64_t bodyId = 0; // producing-feature id of the body to hollow
  std::vector<ElementRef> openFaces;
  double thickness = 1.0;
  std::string thicknessExpr;
};

// The thinnest wall of `body`: from a point well inside each face, along its
// inward normal, to where the line leaves the solid, the least over its
// faces (directedit::WallsUnder). What a shell's default is derived from
// (ui::CreateShell, tools/action_matrix.py): a bounding box is not the wall
// of a body that is already thin-walled. nullopt when no face measures.
std::optional<double> ThinnestWall(const Body& body);
