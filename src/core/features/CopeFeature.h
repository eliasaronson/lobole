#pragma once

#include "core/Feature.h"

#include <cstdint>
#include <string>
#include <vector>

// Cuts a frame member to fit the members it MEETS (docs/tube/plan.md, T2).
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
// `gap` opens the cut by that much on the radius, for the weld's root.
class CopeFeature : public Feature {
public:
  CopeFeature(const std::string& name, uint64_t memberId,
              std::vector<uint64_t> against);

  std::string TypeName() const override { return "Cope"; }

  std::unique_ptr<Feature> Clone() const override {
    return std::make_unique<CopeFeature>(*this);
  }

  bool Execute(RegenContext& ctx) override;

  void VisitFeatureRefs(const RefVisitor& visit) const override {
    visit(memberId);
    for (const uint64_t id : against)
      visit(id);
  }

  void VisitExprs(const ExprVisitor& visit) override { visit(gapExpr, gap); }

  [[nodiscard]] std::string WhyIncomplete() const override {
    if (memberId == 0)
      return "pick the member to cope";
    if (against.empty())
      return "pick what it meets";
    return "";
  }

  uint64_t memberId = 0;         // producing-feature id of the member cut
  std::vector<uint64_t> against; // the members it meets; they survive
  double gap = 0.0;              // mm on the radius, for the weld root
  std::string gapExpr;
};
