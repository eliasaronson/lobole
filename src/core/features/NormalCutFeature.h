#pragma once

#include "core/Feature.h"

#include <cstdint>
#include <string>
#include <vector>

// A cut normal to the sheet (docs/sheetmetal/README.md#features): a closed
// sketch profile, carried along the sketch's normal onto a sheet body and cut
// straight through the sheet WHEREVER it lands — square to a panel, radially
// through a bend, square to a wall that leans.
//
// An extrude cut goes straight along the sketch's normal instead, which is
// right for a block and wrong for sheet: through a bend or a leaning wall it
// comes out slanted to the metal, a hole no laser cuts in the blank and no
// brake then folds into. It is a separate feature rather than an extrude
// option because an extrude's depth and end condition mean nothing here — a
// sheet is cut through its thickness, and only that.
class NormalCutFeature : public Feature {
public:
  NormalCutFeature(const std::string& name, uint64_t sketchId);

  std::string TypeName() const override { return "NormalCut"; }

  std::unique_ptr<Feature> Clone() const override {
    return std::make_unique<NormalCutFeature>(*this);
  }

  bool Execute(RegenContext& ctx) override;

  void VisitFeatureRefs(const RefVisitor& visit) const override {
    visit(sketchId);
    visit(bodyId);
  }

  [[nodiscard]] std::string WhyIncomplete() const override {
    return sketchId == 0 ? "pick a closed sketch profile to cut"
                         : std::string{};
  }

  uint64_t sketchId = 0;
  // The sheet body to cut. 0 = whichever the profile lands on.
  uint64_t bodyId = 0;
  // Region selection (U2), as an extrude: empty = every region.
  std::vector<std::vector<uint32_t>> regions;
};
