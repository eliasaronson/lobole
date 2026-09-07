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

  // defaults to steel when its own is empty, so Document's
  // material re-stamp must leave it alone.
  bool StampsOwnMaterial() const override { return true; }

  bool Execute(RegenContext& ctx) override;

  // Names no other feature: the whole part is derived from the standard.
  void VisitFeatureRefs(const RefVisitor&) const override {}

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
  // Which recess is cut in the head, and EMPTY means "the one this standard
  // is made with" — which is almost always what you want, because the
  // standard fixes it. You do not order an ISO 4762 with a torx drive, you
  // order an ISO 14579, so a drive that disagrees with the standard is
  // refused naming the standard that has it. An unknown name is refused too,
  // rather than left plain: a screw with no recess is a screw nobody can
  // turn, and it looks perfectly fine in the viewport.
  std::string drive;
};

class Document;

// The one Fastener feature a generated standard part is made of, or null.
//
// "The whole document" rather than "contains one": a part with a screw AND
// modelled geometry is not a bought item, and calling it one would put it on a
// purchase order and on a drawing callout. Shared by the BOM, the fastener
// check and the drawing callout — three places that must agree on what counts
// as a bought part.
[[nodiscard]] const FastenerFeature* SoleFastenerOf(const Document* doc);

// A note on WHAT EACH STANDARD IS, because "Fastener" covers three shapes:
// `core/parts/FastenerProfile` maps the standard to its kind (screw, nut,
// washer), its head, its recess and whether the nominal length is measured
// over the head. A nut and a washer have no length, and asking for one is
// refused rather than ignored — an "M6x20 washer" is a spec somebody typed
// wrong, and silently building a plain washer for it hides that.
//
// A note on MASS, because a number that is 25% off looks like a bug rather
// than a modelling choice. The shank is a plain cylinder at the nominal
// diameter — a COSMETIC thread, which is what this codebase already does and
// what bd_warehouse defaults to, because generating a real helix is expensive.
// A real thread removes material, so the generated mass is an UPPER BOUND: a
// steel M6x20 comes out at 7.6 g against a catalogue's ~6 g. That is the right
// error to have (a joint is never lighter than the model says) and it is worth
// knowing before comparing against a supplier's figure.
//
// A NUT's error has the opposite sign, and that is worth stating rather than
// assuming the note above generalises. Its bore is cut at the nominal
// diameter, where a real thread leaves material down to the minor diameter —
// so the cosmetic simplification REMOVES material from a nut and ADDS it to a
// screw. (Omitting the corner chamfers pushes back the other way, by less.) A
// generated M6 nut therefore weighs 2.4 g and a real one a little more, while
// the M6x20 screw above is a little heavy. Neither is a bug; they are the
// same simplification seen from inside and outside the thread.
