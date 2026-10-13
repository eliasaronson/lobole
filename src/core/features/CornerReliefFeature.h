// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "core/Feature.h"

#include <cstdint>
#include <string>

// Corner relief (docs/sheetmetal/README.md#features): where two bends of a
// sheet meet at a corner, the metal of both is trying to move into the same
// place. A relief cut there — round or square, centred where the two bend lines
// cross — gives it somewhere to go instead of tearing or buckling the corner.
//
// It applies to EVERY such corner of the body: a part with one relieved
// corner and three that tear is not a part anybody wants. Corners where three
// bends meet, or two meet at other than a right angle, are refused by name
// rather than given a relief shaped for a square two-bend corner.
enum class CornerReliefStyle { Circular, Rectangular };
[[nodiscard]] std::string CornerReliefStyleName(CornerReliefStyle s);
[[nodiscard]] bool CornerReliefStyleFromString(const std::string& name,
                                               CornerReliefStyle& out);
[[nodiscard]] const std::string& CornerReliefStyleList();

class CornerReliefFeature : public Feature {
public:
  CornerReliefFeature(const std::string& name, uint64_t bodyId);

  std::string TypeName() const override { return "CornerRelief"; }

  std::unique_ptr<Feature> Clone() const override {
    return std::make_unique<CornerReliefFeature>(*this);
  }

  void CopyDerivedState(const Feature& executed) override {
    RestoreExecuted<CornerReliefFeature>(executed);
  }

  bool Execute(RegenContext& ctx) override;

  void VisitFeatureRefs(const RefVisitor& visit) const override {
    visit(bodyId);
  }

  void VisitExprs(const ExprVisitor& visit) override { visit(sizeExpr, size); }

  [[nodiscard]] std::string WhyIncomplete() const override {
    return bodyId == 0 ? "pick the sheet body whose corners to relieve"
                       : std::string{};
  }

  uint64_t bodyId = 0;
  CornerReliefStyle style = CornerReliefStyle::Circular;
  // The relief's radius, or half its side, mm. 0 = one bend radius plus one
  // thickness: exactly what clears both bends where they cross.
  double size = 0.0;
  std::string sizeExpr;
};
