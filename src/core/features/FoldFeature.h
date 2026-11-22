// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "core/Feature.h"
#include "core/sheet/Unfold.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// Which side of a fold's line turns, seen from the sketched face walking from
// the line's first point to its second. Unset is what a new fold arrives
// with: Execute resolves it once, to the side that frees less of the sheet,
// and stores it, so a later edit that changes which side is larger does not
// move the fold.
enum class FoldSide { Unset, Left, Right };

[[nodiscard]] const char* FoldPositionName(sheet::FoldPosition p);
[[nodiscard]] bool FoldPositionFromString(const std::string& name,
                                          sheet::FoldPosition& out);
[[nodiscard]] const std::string& FoldPositionList();
[[nodiscard]] const char* FoldSideName(FoldSide s);
[[nodiscard]] bool FoldSideFromString(const std::string& name, FoldSide& out);
[[nodiscard]] const std::string& FoldSideList();

// A fold (docs/sheetmetal/README.md#fold): a bend made in sheet that is
// already there. A line sketched on a panel, and the material on one side of
// it turned up about that line; where the panel's outline or a cut-out
// interrupts the line, each stretch frees its own piece, so a tab cut on
// three sides inside a plate folds on its own and the plate round it stays
// flat.
class FoldFeature : public Feature {
public:
  FoldFeature(const std::string& name, uint64_t sketchId);

  std::string TypeName() const override { return "Fold"; }

  std::unique_ptr<Feature> Clone() const override {
    return std::make_unique<FoldFeature>(*this);
  }

  // The side, resolved the first time the fold sees its sheet.
  bool RecordsWhatItIsKeyedOn() const override { return true; }

  void CopyDerivedState(const Feature& executed) override {
    RestoreExecuted<FoldFeature>(executed);
  }

  bool Execute(RegenContext& ctx) override;

  void VisitFeatureRefs(const RefVisitor& visit) const override {
    visit(sketchId);
  }

  void VisitExprs(const ExprVisitor& visit) override {
    visit({angleDeg, ExprQuantity::Angle, "angle_deg"});
    visit({radius, ExprQuantity::Length, "radius_mm"});
  }

  [[nodiscard]] std::string WhyIncomplete() const override {
    return sketchId == 0 ? "pick a sketch holding one line drawn on a sheet "
                           "panel"
                         : std::string{};
  }

  // A side_at_mm point that lies on the line, or off the sheet, is refused
  // here, with the document unchanged.
  [[nodiscard]] std::string WhyRefusedOn(const std::vector<Body>& input,
                                         const Document& doc) const override;

  // A sketch holding exactly one line, drawn ON a panel's face.
  uint64_t sketchId = 0;
  // The bend's sweep, (0, 180].
  ExprValue angleDeg{90.0};
  // Inside radius, mm; 0 = the rules' bend radius.
  ExprValue radius{0.0};
  sheet::FoldPosition position = sheet::FoldPosition::Centerline;
  FoldSide side = FoldSide::Unset;
  // A point in the sketch's plane on the side that turns (what a click in
  // the tool sends, `side_at_mm`): resolved to `side` and dropped. Never
  // written to a file.
  std::optional<glm::dvec2> sideAt;
  // Fold away from the sketched face instead of toward it.
  bool reverse = false;

  // ---- derived by Execute, for the tool and the reports ----
  // Each bend's zone on the face before the fold, in order along the line.
  std::vector<sheet::FoldZone> zones;
  double effectiveRadius = 0.0; // mm
  double allowance = 0.0;       // mm, per bend
  std::string radiusSource;     // "rules", "material minimum", "set"
  // The line on the face, and the face's outward normal, as last built.
  glm::dvec3 lineA{0.0}, lineB{0.0}, faceNormal{0.0, 0.0, 1.0};
  // The axis of the first bend, folded -- where the angle handle sits.
  glm::dvec3 axisOrigin{0.0}, axisDir{1.0, 0.0, 0.0};
  glm::dvec3 parentDir{0.0, 1.0, 0.0}, upDir{0.0, 0.0, 1.0};

private:
  // The line in the sketch's plane and on the face, or the reason not.
  struct Line {
    glm::dvec2 a2{0.0}, b2{0.0};
    sheet::Panel panel;
    glm::dvec3 sketchNormal{0.0, 0.0, 1.0};
  };
  [[nodiscard]] std::string FindLine(const std::vector<Body>& bodies,
                                     const Feature* sketchFeature, double t,
                                     Line& out) const;
  // Why `sideAt` cannot name a side: on the line, or off the sheet.
  [[nodiscard]] std::string WhySideAt(const std::vector<Body>& bodies,
                                      const Line& line, const Feature* sketch,
                                      double t) const;
  // Which side `p` (sketch plane coordinates) is on: Left, Right, or Unset
  // when it lies on the line.
  [[nodiscard]] static FoldSide SideOf(const Line& line, const glm::dvec2& p);
};
