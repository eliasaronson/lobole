#pragma once

#include "core/Feature.h"

#include <cstdint>
#include <string>
#include <vector>

// Cuts a frame member to fit the members it MEETS
// (docs/tube/README.md#cope-and-mitre).
//
// At a node every member is coped to every other and they all survive, which
// is exactly what a Boolean cannot do: a boolean consumes both its operands,
// so coping with one needs a throwaway copy of the neighbour per joint — a
// forty-member frame is a few hundred features of bookkeeping, and every
// throwaway is a tube in the cut list that nobody ordered.
//
// A cope cuts `memberId`'s body with the OUTER solid of each feature in
// `against`, and leaves those alone. The outer solid is the point: the
// outside of the neighbour is what the member has to clear, so a hollow tube
// cuts as though it were solid — subtract the tube itself and the member
// fills its bore.
//
// `mitre` is the OTHER cut a frame needs. Where two members meet END TO END
// there is no round side to fishmouth against, and both tubes are cut on the
// plane that bisects them. Which of the two a joint wants is TOPOLOGY, not
// geometry — once members overrun their nodes (which a mitre requires, since
// a plane through the very end of a tube only shaves a wedge off it) a
// corner and a tee look alike — so the caller says, and the feature cuts.
//
// `gap` opens either cut by that much: a cope on the radius, a mitre by
// moving the plane back along the member.
class CopeFeature : public Feature {
public:
  CopeFeature(const std::string& name, uint64_t memberId,
              std::vector<uint64_t> against, std::vector<uint64_t> mitre = {});

  std::string TypeName() const override { return "Cope"; }

  std::unique_ptr<Feature> Clone() const override {
    return std::make_unique<CopeFeature>(*this);
  }

  bool Execute(RegenContext& ctx) override;

  void VisitFeatureRefs(const RefVisitor& visit) const override {
    visit(memberId);
    for (const uint64_t id : against)
      visit(id);
    for (const uint64_t id : mitre)
      visit(id);
  }

  void VisitExprs(const ExprVisitor& visit) override { visit(gapExpr, gap); }

  [[nodiscard]] std::string WhyIncomplete() const override {
    if (memberId == 0)
      return "pick the member to cope";
    if (against.empty() && mitre.empty())
      return "pick what it meets";
    return "";
  }

  uint64_t memberId = 0;         // producing-feature id of the member cut
  std::vector<uint64_t> against; // lands on their SIDE: a fishmouth
  std::vector<uint64_t> mitre;   // meets them END TO END: a mitre
  double gap = 0.0;              // mm on the radius, for the weld root
  std::string gapExpr;
};
