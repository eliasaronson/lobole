// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "core/AxisRef.h"
#include "core/DirectEdit.h"
#include "core/ElementMap.h"
#include "core/Feature.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

// Moves a set of faces that form one feature -- a hole, a boss, a pocket, a
// slot -- rigidly: the body closes behind it and opens where it lands
// (docs/modeling/README.md#move-face). Built from Delete Face and named
// booleans: H is the body B with the faces deleted; the feature's material
// is cut = H - B (a hole's) and add = B - H (a boss's); the result is
// (H - T(cut)) u T(add), T the move. Each moved wall is "<name>;M<id>".
class MoveFaceFeature : public Feature {
public:
  MoveFaceFeature(const std::string& name, uint64_t bodyId,
                  std::vector<ElementRef> faces, const glm::dvec3& translate);

  std::string TypeName() const override { return "MoveFace"; }

  std::unique_ptr<Feature> Clone() const override {
    return std::make_unique<MoveFaceFeature>(*this);
  }

  void CopyDerivedState(const Feature& executed) override {
    RestoreExecuted<MoveFaceFeature>(executed);
  }

  bool Execute(RegenContext& ctx) override;

  void VisitFeatureRefs(const RefVisitor& visit) const override {
    visit(bodyId);
    VisitRefs(faces, visit);
    VisitRefs(axis, visit);
  }

  void VisitExprs(const ExprVisitor& visit) override {
    visit(translateExpr[0], translate.x);
    visit(translateExpr[1], translate.y);
    visit(translateExpr[2], translate.z);
    visit(rotateExpr, rotateDeg);
  }

  [[nodiscard]] std::string WhyIncomplete() const override {
    return faces.empty() ? "pick the faces to move" : "";
  }

  uint64_t bodyId = 0; // producing-feature id of the body edited
  std::vector<ElementRef> faces;
  glm::dvec3 translate{0.0}; // mm, model space
  std::array<std::string, 3> translateExpr;
  double rotateDeg = 0.0; // about `axis`, after nothing: rotate then translate
  std::string rotateExpr;
  AxisRef axis; // used only when rotateDeg != 0
  directedit::Fingerprints fingerprints;
};
