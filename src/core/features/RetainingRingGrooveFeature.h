#pragma once

#include "core/Feature.h"

#include <string>

// The groove a DIN 471 or DIN 472 retaining ring sits in, cut into a
// cylindrical face of the part: a shaft's (471) or a bore's wall (472)
// (docs/parts/README.md#the-groove).
//
// Placed by `from`, a circular edge at one END of the face — the side of what
// the ring holds, such as the shoulder a bearing sits against — and `offset`,
// the distance from that edge along the face to the groove's near flank (the
// flank the ring bears on, so for a bearing held between a shoulder and the
// ring: the bearing's width). The groove is the table's: diameter d2, width
// m. Refused, never adjusted, when it does not fit: a face whose diameter is
// not a size the table carries, a groove that runs off the face, or less
// than the table's edge distance n left past it — the land that carries the
// ring's thrust toward the face's free end.
class RetainingRingGrooveFeature : public Feature {
public:
  RetainingRingGrooveFeature(const std::string& name, ElementRef face,
                             ElementRef from, double offset);

  std::string TypeName() const override { return "RetainingRingGroove"; }

  std::unique_ptr<Feature> Clone() const override {
    return std::make_unique<RetainingRingGrooveFeature>(*this);
  }

  bool Execute(RegenContext& ctx) override;
  void VisitFeatureRefs(const RefVisitor& visit) const override {
    VisitRefs(face, visit); // the cylindrical face the groove is cut in
    VisitRefs(from, visit); // the edge at the end it is measured from
  }
  void VisitExprs(const ExprVisitor& visit) override {
    visit(offsetExpr, offset);
  }

  ElementRef face;     // a cylindrical face, by stable toponame
  ElementRef from;     // a circular edge at one end of that face
  double offset = 0.0; // mm, from `from` to the groove's near flank
  std::string offsetExpr;
  // "din471" | "din472", or empty for the one the face takes (a shaft takes
  // 471, a bore 472); the other one is refused.
  std::string standard;

  // The groove's faces, as the suffix of the element name
  // ("<id>/groove_floor"): flank A is the near flank (on `from`'s side),
  // the face a ring's side bears on.
  static constexpr const char* kFloor = "groove_floor";
  static constexpr const char* kFlankA = "groove_flank_a";
  static constexpr const char* kFlankB = "groove_flank_b";
};
