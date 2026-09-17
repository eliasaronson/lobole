#pragma once

#include "core/Feature.h"

#include <cstdint>
#include <string>
#include <vector>

// A hem (docs/sheetmetal/plan.md, S7): the edge of a panel folded right back
// on itself — to stiffen it, and to take the cut edge away from a hand.
//
// It is the edge flange's builder at 180 degrees, with the radius set by the
// GAP the return leaves rather than by the material: a hem is crushed, not
// bent round a die radius, so it is exempt from the minimum bend radius.
enum class HemKind { Closed, Open };
[[nodiscard]] std::string HemKindName(HemKind k);
[[nodiscard]] bool HemKindFromString(const std::string& name, HemKind& out);
[[nodiscard]] const std::string& HemKindList();

class HemFeature : public Feature {
public:
  HemFeature(const std::string& name, std::vector<ElementRef> edges,
             double length);

  std::string TypeName() const override { return "Hem"; }

  std::unique_ptr<Feature> Clone() const override {
    return std::make_unique<HemFeature>(*this);
  }

  bool Execute(RegenContext& ctx) override;

  void VisitFeatureRefs(const RefVisitor& visit) const override {
    for (const ElementRef& e : edges)
      visit(e.featureId);
  }

  void VisitExprs(const ExprVisitor& visit) override {
    visit(lengthExpr, length);
    visit(gapExpr, gap);
  }

  [[nodiscard]] std::string WhyIncomplete() const override {
    return edges.empty() ? "pick one or more straight edges of a sheet panel "
                           "to hem"
                         : std::string{};
  }

  std::vector<ElementRef> edges;
  // How far the return runs back over the panel, past the fold, mm.
  double length = 5.0;
  std::string lengthExpr;
  // Closed: the return lies against the panel, with the smallest gap a solid
  // model can keep between two walls — a tenth of the sheet — since walls
  // that touch fuse into one and the fold stops being a fold. Open: the
  // return stands off by `gap`.
  HemKind kind = HemKind::Closed;
  double gap = 0.0; // open hems; 0 = one thickness
  std::string gapExpr;
  // Inside (default): the fold's outside lands on the picked edge, so the
  // part does not grow past its drawn outline. Outside: the fold starts at
  // the edge and adds to it.
  bool outside = false;
};
