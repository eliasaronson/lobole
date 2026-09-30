#pragma once

#include "core/Feature.h"

#include <string>

namespace parts {
struct BearingRow;
}

// A deep-groove ball bearing, built from its ISO 15 designation
// (docs/parts/README.md#bearings).
//
// The designation is the whole specification: d, D, B and the chamfer come
// out of `core/parts/BearingTable`, so a test that checks the body against
// the row checks it against the standard. Like a fastener, it is meant to be
// the whole of a generated part document, placed as an occurrence, so the
// BOM counts it as a bought item.
//
// The body is ONE solid of revolution, drawn as a shielded bearing (2Z): an
// inner ring and an outer ring standing proud of a recessed shield on each
// side, the four outer corners chamfered at r_s min, and the ball space an
// internal void. Why one solid and not rings and balls:
//  * the balls are a boolean each (8 to 12 of them) and invisible behind a
//    shield anyway; the shielded look is recognisable at any zoom;
//  * two rings would be two disconnected solids, which check_geometry
//    reports as a defect and mass properties would sum as one part anyway;
//  * the void keeps the MASS honest: a full annulus is 40% heavy. With it, in
//    steel, a 608 comes out at 12.2 g against a catalogue's 12 g, a 6200 at
//    31.0 g against 32 g and a 6206 at 194 g against 200 g (the proportions
//    are in BearingFeature.cpp).
// It cannot turn: the rings are one body. A mechanism mates the bearing
// rigidly to one seat and turns on the other (docs/parts/README.md#bearings).
//
// Built with its axis on +Z, side A on z = 0 and side B on z = B — the side
// that sits against a shoulder is on the origin plane, the way a fastener's
// bearing face is.
class BearingFeature : public Feature {
public:
  BearingFeature(const std::string& name, std::string designation);

  std::string TypeName() const override { return "Bearing"; }

  std::unique_ptr<Feature> Clone() const override {
    return std::make_unique<BearingFeature>(*this);
  }

  // Steel unless told otherwise, like a fastener.
  bool StampsOwnMaterial() const override { return true; }

  bool Execute(RegenContext& ctx) override;

  // Names no other feature: the whole part is derived from the standard.
  void VisitFeatureRefs(const RefVisitor&) const override {}

  // "6200" — what a supplier calls it, and what the BOM carries.
  std::string designation;

  // The faces a mate, a fit or a seat check names, as the suffix of the
  // element name ("<id>/bore"). The ring faces on each side are apart
  // because they bear on different parts: the inner ring against a shaft
  // shoulder, the outer against a housing's.
  static constexpr const char* kBore = "bore";
  static constexpr const char* kOutside = "outside";
  static constexpr const char* kInnerSideA = "inner_side_a";
  static constexpr const char* kInnerSideB = "inner_side_b";
  static constexpr const char* kOuterSideA = "outer_side_a";
  static constexpr const char* kOuterSideB = "outer_side_b";
  static constexpr const char* kShieldA = "shield_a";
  static constexpr const char* kShieldB = "shield_b";

  // The element name of one of those faces on this feature's body.
  [[nodiscard]] std::string FaceName(const char* role) const;
  // Which of those a name is ("" when it is none of them).
  [[nodiscard]] static std::string RoleOf(const std::string& elementName);
};

class Document;

// The one Bearing feature a generated standard part is made of, or null —
// the whole document, as SoleFastenerOf.
[[nodiscard]] const BearingFeature* SoleBearingOf(const Document* doc);
