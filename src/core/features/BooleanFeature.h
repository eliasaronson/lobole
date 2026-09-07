#pragma once

#include "core/Feature.h"

#include <cstdint>
#include <string>

enum class BooleanOp { Union, Subtract, Intersect };

// Combines two existing bodies into one (modeling M4). Operands are referenced
// by the stable id of the feature that *produced* each body (Body::createdBy) —
// a whole-body reference, so this needs no topological naming. Both inputs are
// consumed; the result is a new body owned by this feature (referenceable by a
// later boolean). Regeneration fails if either operand body is missing, the
// two operands are the same, or the result is invalid (e.g. an intersection of
// disjoint bodies).
class BooleanFeature : public Feature {
public:
  BooleanFeature(const std::string& name, uint64_t targetId, uint64_t toolId,
                 BooleanOp op);

  [[nodiscard]] std::string TypeName() const override { return "Boolean"; }

  std::unique_ptr<Feature> Clone() const override {
    return std::make_unique<BooleanFeature>(*this);
  }

  bool Execute(RegenContext& ctx) override;

  void VisitFeatureRefs(const RefVisitor& visit) const override {
    visit(targetId); // both operands are named by their producing feature
    visit(toolId);
  }

  [[nodiscard]] std::string WhyIncomplete() const override {
    if (targetId == 0)
      return "pick the target body";
    if (toolId == 0)
      return "pick the tool body";
    return "";
  }

  uint64_t targetId = 0; // producing-feature id of operand A
  uint64_t toolId = 0;   // producing-feature id of operand B
  BooleanOp op = BooleanOp::Union;
};
