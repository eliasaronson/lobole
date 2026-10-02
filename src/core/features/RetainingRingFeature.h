#pragma once

#include "core/Feature.h"

#include <string>

// A DIN 471 (shaft) or DIN 472 (bore) retaining ring, built from its standard
// and size (docs/parts/README.md#retaining-rings).
//
// The table row is the whole specification, like a bearing's: thickness s,
// free diameter d3 and section b come out of core/parts/RetainingRingTable.
// It is meant to be the whole of a generated part document, placed as an
// occurrence, so the BOM counts it as a bought item.
//
// The body is SIMPLIFIED: a flat annulus s thick at the ring's FREE (sprung)
// diameter with the table's radial section b — no opening and no lugs, whose
// angular extent the table does not give. A DIN 471 ring's inside is d3, its
// outside d3 + 2b; a DIN 472 ring's outside is d3, its inside d3 - 2b. Free,
// a 471 ring's inside is SMALLER than the groove it sits in (d3 < d2) — that
// grip is what holds it — so in its groove the drawn ring overlaps the groove
// floor by (d2 - d3) / 2.
//
// Built with its axis on +Z, side A on z = 0 and side B on z = s.
class RetainingRingFeature : public Feature {
public:
  RetainingRingFeature(const std::string& name, std::string standard,
                       double size);

  std::string TypeName() const override { return "RetainingRing"; }

  std::unique_ptr<Feature> Clone() const override {
    return std::make_unique<RetainingRingFeature>(*this);
  }

  // Spring steel unless told otherwise, like a fastener.
  bool StampsOwnMaterial() const override { return true; }

  bool Execute(RegenContext& ctx) override;

  // Names no other feature: the whole part is derived from the standard.
  void VisitFeatureRefs(const RefVisitor&) const override {}

  std::string standard; // "din471" | "din472"
  double size = 0.0;    // the nominal shaft or bore diameter, mm

  // The faces a placement names, as the suffix of the element name
  // ("<id>/ring_side_a"): side A bears on what the ring holds.
  static constexpr const char* kBore = "bore";
  static constexpr const char* kOutside = "outside";
  static constexpr const char* kSideA = "ring_side_a";
  static constexpr const char* kSideB = "ring_side_b";
};

class Document;

// The one RetainingRing feature a generated standard part is made of, or
// null — the whole document, as SoleBearingOf.
[[nodiscard]] const RetainingRingFeature*
SoleRetainingRingOf(const Document* doc);
