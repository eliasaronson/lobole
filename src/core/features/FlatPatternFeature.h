#pragma once

#include "core/Feature.h"

#include <cstdint>
#include <string>

// The blank: the sheet body laid flat
// (docs/sheetmetal/README.md#how-the-unfolder-works).
//
// A terminal feature, authored SUPPRESSED, that replaces the body's shape
// with its flat pattern in place. It is not a second body — a folded part and
// its blank occupy the same space, so two bodies would sit inside each other
// and every measurement would double. Switching between them is a
// CONFIGURATION: `Flat` unsuppresses this, the authored model stays folded,
// and both are one document with one undo stack.
//
// It flattens from the bends the earlier features RECORDED, so it must sit
// after every feature that reshapes the sheet. A cut taken across a bend is
// refused rather than silently swallowed when the bend is rebuilt flat —
// Unfold/Refold is the answer to that refusal.
class FlatPatternFeature : public Feature {
public:
  FlatPatternFeature(const std::string& name, uint64_t bodyId);

  std::string TypeName() const override { return "FlatPattern"; }

  std::unique_ptr<Feature> Clone() const override {
    return std::make_unique<FlatPatternFeature>(*this);
  }

  bool Execute(RegenContext& ctx) override;

  void VisitFeatureRefs(const RefVisitor& visit) const override {
    visit(bodyId);
    visit(fixedFace.featureId);
  }

  [[nodiscard]] std::string WhyIncomplete() const override {
    return bodyId == 0 ? "pick the sheet body to flatten" : std::string{};
  }

  // The body to flatten, by the stable id of the feature that created it.
  uint64_t bodyId = 0;
  // The panel that stays put while everything else swings flat. Unset picks
  // the piece the first bend folds FROM, which is the base flange — the face
  // a shop would lay on the brake's bed anyway.
  ElementRef fixedFace;
};
