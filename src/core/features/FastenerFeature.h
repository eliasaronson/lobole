#pragma once

#include "core/Feature.h"

#include <string>

// A standard fastener, built from a vendored dimension table rather than from
// typed numbers (docs/parts/plan.md, phase 2).
//
// Every dimension except the length comes out of `core/parts/FastenerTable` —
// the row for this size in this standard IS the specification, so a test that
// checks the generated body against those numbers is checking it against
// ISO 4762 rather than against itself. Nothing here is a literal, which is why
// there are no head-diameter or socket-size fields to get wrong.
//
// The length is the exception and is a parameter, because it is the one
// dimension the STACK decides rather than the standard: a screw is made in
// many lengths and which one you want depends on what it goes through. It
// still may not be arbitrary — `parts::SnapLength` refuses a length the size
// is not made in, and phase 3 derives it from the hole rather than asking.
//
// This feature is meant to be the whole of a generated part document: one
// fastener, one part, placed as an occurrence, so the BOM counts it as a
// bought item and CAM never sees it as something to machine. Putting one in a
// part alongside modelled geometry is possible and is wrong for those reasons
// — the placement layer refuses it (plan.md, "The model").
class FastenerFeature : public Feature {
public:
  // `standard` is the vendored file stem ("iso4762def"), `size` a key in it
  // ("M6"). Both are refused at Execute if the table does not carry them, with
  // the table's own message naming what is stocked.
  FastenerFeature(const std::string& name, std::string standard,
                  std::string size, double length);

  std::string TypeName() const override { return "Fastener"; }

  std::unique_ptr<Feature> Clone() const override {
    return std::make_unique<FastenerFeature>(*this);
  }

  bool Execute(RegenContext& ctx) override;

  // The length is expression-drivable like every other length in this
  // codebase, so a variable or a configuration can drive it. The size is NOT:
  // it is a key into a table, not a quantity, and an expression that evaluated
  // to "M6.5" would have nothing to look up.
  void VisitExprs(const ExprVisitor& visit) override {
    visit(lengthExpr, length);
  }

  // The designation a supplier would recognise, and the name the generated
  // part document carries into the BOM: "ISO 4762 M6x20".
  [[nodiscard]] std::string Designation() const;

  std::string standard; // vendored file stem, e.g. "iso4762def"
  std::string size;     // table key, e.g. "M6"
  double length = 0.0;  // mm, under the head
  std::string lengthExpr;
  // Which recess is cut in the head. Only the drives the vendored standards
  // define are accepted; an unknown one is refused by name rather than left
  // plain, because a screw with no recess is a screw nobody can turn and it
  // looks fine in the viewport.
  std::string drive = "hex";
};
