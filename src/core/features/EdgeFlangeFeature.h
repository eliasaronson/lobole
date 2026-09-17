#pragma once

#include "core/Feature.h"
#include "core/SheetMetal.h"

#include <cstdint>
#include <string>
#include <vector>

// Where the flange's wall sits relative to the edge that was picked
// (docs/sheetmetal/plan.md, S3). All three are the same construction with a
// different SETBACK from the picked edge to the bend's tangent line, so the
// choice is about which mold line the original edge marks:
//   MaterialInside  - the OUTSIDE mold line lands on the picked edge, so the
//                     part does not grow past the profile that was drawn.
//   MaterialOutside - the INSIDE mold line lands on it; the wall hangs out
//                     past the original edge by a thickness.
//   BendOutside     - the bend STARTS at the edge, so the whole bend and
//                     wall are added outside and the panel keeps its size.
enum class FlangePosition { MaterialInside, MaterialOutside, BendOutside };

// Which dimension `length` states. A drawing almost never dimensions the flat
// wall — it dimensions to a mold line — so getting this wrong is a flange
// that is right by construction and wrong by a bend radius plus a thickness.
enum class FlangeLengthMode { Flat, Outside, Inside };

[[nodiscard]] std::string FlangePositionName(FlangePosition p);
[[nodiscard]] bool FlangePositionFromString(const std::string& name,
                                            FlangePosition& out);
[[nodiscard]] const std::string& FlangePositionList();

[[nodiscard]] std::string FlangeLengthModeName(FlangeLengthMode m);
[[nodiscard]] bool FlangeLengthModeFromString(const std::string& name,
                                              FlangeLengthMode& out);
[[nodiscard]] const std::string& FlangeLengthModeList();

// A wall bent up from an edge of an existing sheet panel: the feature that
// makes a flat blank into a part.
//
// One bend per picked edge, each recorded on the body as a BendRegion —
// axis, radius, angle, K and where that K came from — because the flat
// pattern is computed from what the features DID, not measured back off the
// cylindrical faces afterwards.
class EdgeFlangeFeature : public Feature {
public:
  EdgeFlangeFeature(const std::string& name, std::vector<ElementRef> edges,
                    double length, double angleDeg = 90.0);

  std::string TypeName() const override { return "EdgeFlange"; }

  std::unique_ptr<Feature> Clone() const override {
    return std::make_unique<EdgeFlangeFeature>(*this);
  }

  bool Execute(RegenContext& ctx) override;

  void VisitFeatureRefs(const RefVisitor& visit) const override {
    for (const ElementRef& e : edges)
      visit(e.featureId);
  }

  void VisitExprs(const ExprVisitor& visit) override {
    visit(lengthExpr, length);
    visit(angleDegExpr, angleDeg);
    visit(radiusExpr, radius);
    visit(offsetStartExpr, offsetStart);
    visit(offsetEndExpr, offsetEnd);
  }

  [[nodiscard]] std::string WhyIncomplete() const override {
    if (edges.empty())
      return "pick one or more straight edges of a sheet panel to flange";
    return {};
  }

  // The edges to bend up, each a straight edge of a flat panel of the same
  // sheet body.
  std::vector<ElementRef> edges;

  double length = 10.0; // mm, read as `lengthMode` says
  std::string lengthExpr;
  double angleDeg = 90.0; // the bend's included sweep, not the wall's pitch
  std::string angleDegExpr;
  // 0 = the document's bend radius (itself the material's minimum unless the
  // rules pin it). Never a literal default: the tightest radius a sheet takes
  // is a property of the sheet.
  double radius = 0.0;
  std::string radiusExpr;

  FlangePosition position = FlangePosition::MaterialInside;
  FlangeLengthMode lengthMode = FlangeLengthMode::Flat;

  // Inset of each end of the flange from the ends of the picked edge, mm.
  // Non-zero is what makes a bend line STOP inside a panel — and so what
  // makes relief necessary, since the metal either gets a cut to move into or
  // tears the panel beside it.
  double offsetStart = 0.0;
  std::string offsetStartExpr;
  double offsetEnd = 0.0;
  std::string offsetEndExpr;

  // Unset (nullopt) inherits the document's relief style, the Appearance
  // pattern. `Tear` and `None` both cut nothing; they differ in what the
  // DFM check is later entitled to say about it — Tear is a decision, None
  // is the claim that there is nothing to relieve.
  std::optional<sheet::ReliefStyle> reliefStyle;
};
