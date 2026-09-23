#pragma once

#include "core/Feature.h"

#include <cstdint>
#include <string>
#include <vector>

// The first wall of a sheet part: a closed sketch profile thickened by the
// document's sheet-metal thickness (docs/sheetmetal/README.md#features).
//
// Geometrically it is the cheapest feature in the workstream — a prism one
// gauge deep. What it actually does is DECLARE the part to be sheet metal:
// it is the feature that refuses when no rules are set, and every flange
// after it grows off a wall this one placed.
//
// The thickness is never a parameter here. A base flange that carried its own
// would let a part be 1.5 mm at one end and 3 mm at the other, and nothing
// downstream could refuse it — a blank is cut from one sheet.
class BaseFlangeFeature : public Feature {
public:
  BaseFlangeFeature(const std::string& name, uint64_t sketchId,
                    bool reversed = false);

  std::string TypeName() const override { return "BaseFlange"; }

  std::unique_ptr<Feature> Clone() const override {
    return std::make_unique<BaseFlangeFeature>(*this);
  }

  bool Execute(RegenContext& ctx) override;

  void VisitFeatureRefs(const RefVisitor& visit) const override {
    visit(sketchId);
  }

  // The wall takes the sheet's material unless the feature names its own, so
  // it cannot be re-stamped from m_material after a cached regeneration.
  bool StampsOwnMaterial() const override { return true; }

  [[nodiscard]] std::string WhyIncomplete() const override {
    return sketchId == 0 ? "pick a closed sketch profile for the base flange"
                         : std::string{};
  }

  uint64_t sketchId = 0;
  // Which side of the sketch plane the material grows toward. The profile is
  // the INSIDE face either way, so this is what decides whether a bore drawn
  // on the plane keeps its size.
  bool reversed = false;
  // Region selection (U2): each entry is one selected region's sorted outer
  // loop entity ids. Empty = every region of the sketch.
  std::vector<std::vector<uint32_t>> regions;
};
