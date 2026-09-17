#pragma once

#include "core/Feature.h"

#include <cstdint>
#include <string>
#include <vector>

// Unfold / Refold (docs/sheetmetal/plan.md, S9): the pair that lets a cut be
// authored FLAT across a bend — a slot that has to be a straight slot in the
// blank, a vent pattern that runs over a fold.
//
// Unfold lays some or all of a sheet body's bends flat, in place; whatever is
// drawn and cut after it is cut in the flat; Refold folds those same bends back
// up, carrying every through-thickness cut across each crease onto the bend by
// the same neutral-axis map the bend allowance comes from.
//
// An Unfold with no Refold after it leaves the body flat, and a FlatPattern
// after it lays down whatever is still folded.
class UnfoldFeature : public Feature {
public:
  UnfoldFeature(const std::string& name, uint64_t bodyId);

  std::string TypeName() const override { return "Unfold"; }

  std::unique_ptr<Feature> Clone() const override {
    return std::make_unique<UnfoldFeature>(*this);
  }

  bool Execute(RegenContext& ctx) override;

  void VisitFeatureRefs(const RefVisitor& visit) const override {
    visit(bodyId);
    visit(fixedFace.featureId);
    for (uint64_t id : bendFeatures)
      visit(id);
  }

  [[nodiscard]] std::string WhyIncomplete() const override {
    return bodyId == 0 ? "pick the sheet body to unfold" : std::string{};
  }

  uint64_t bodyId = 0;
  // The panel that stays put. Unset = the base flange's.
  ElementRef fixedFace;
  // Which bends: every bend made by these flange features. Empty = all of
  // them. By FEATURE rather than by bend index, because an index renumbers
  // the moment an earlier flange gains an edge.
  std::vector<uint64_t> bendFeatures;
};
