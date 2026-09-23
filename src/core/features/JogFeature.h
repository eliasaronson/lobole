#pragma once

#include "core/Feature.h"

#include <cstdint>
#include <string>

// A jog (docs/sheetmetal/README.md#features): a step in a panel — two opposing
// bends across it along a sketch line, the far side carried up by `height` and
// otherwise untouched.
//
// The line only LOCATES the step. The bends run the full width of the panel
// there, because a step that stopped short would tear the panel either side
// of it; the line's own length is not used.
class JogFeature : public Feature {
public:
  JogFeature(const std::string& name, uint64_t sketchId, double height);

  std::string TypeName() const override { return "Jog"; }

  std::unique_ptr<Feature> Clone() const override {
    return std::make_unique<JogFeature>(*this);
  }

  bool Execute(RegenContext& ctx) override;

  void VisitFeatureRefs(const RefVisitor& visit) const override {
    visit(sketchId);
  }

  void VisitExprs(const ExprVisitor& visit) override {
    visit(heightExpr, height);
    visit(angleDegExpr, angleDeg);
  }

  [[nodiscard]] std::string WhyIncomplete() const override {
    return sketchId == 0 ? "pick a sketch holding one line drawn on a sheet "
                           "panel"
                         : std::string{};
  }

  // A sketch holding exactly one line, drawn ON a panel's face.
  uint64_t sketchId = 0;
  // Face to face: how far the far side's picked face ends up above where it
  // was, mm. There is a floor — the two bends alone take (2R + t)(1 - cos a)
  // — and a height under it is refused rather than squeezed.
  double height = 5.0;
  std::string heightExpr;
  // How steeply the step climbs, (0, 90]. 90 is a square step.
  double angleDeg = 90.0;
  std::string angleDegExpr;
  // Which side of the line moves. The sketch face's normal is "up": the far
  // side is carried that way.
  bool flip = false;
};
