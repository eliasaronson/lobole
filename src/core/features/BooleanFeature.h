// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "core/Feature.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>

enum class BooleanOp { Union, Subtract, Intersect };

// Combines two existing bodies into one (modeling M4). Operands are referenced
// by the stable id of the feature that *produced* each body (Body::createdBy) —
// a whole-body reference, so this needs no topological naming. Both inputs are
// consumed; the result is a new body owned by this feature (referenceable by a
// later boolean). With a MESH operand it runs on triangles, through manifold
// (docs/modeling/README.md#mesh-booleans): both must be closed, a B-Rep
// operand is tessellated at a deflection derived from its size, and the
// result is a mesh body. Regeneration fails if either operand body is missing,
// the two operands are the same, or the result is invalid (e.g. an intersection
// of disjoint bodies).
class BooleanFeature : public Feature {
public:
  BooleanFeature(const std::string& name, uint64_t targetId, uint64_t toolId,
                 BooleanOp op);

  [[nodiscard]] std::string TypeName() const override { return "Boolean"; }

  std::unique_ptr<Feature> Clone() const override {
    return std::make_unique<BooleanFeature>(*this);
  }

  void CopyDerivedState(const Feature& executed) override {
    RestoreExecuted<BooleanFeature>(executed);
  }

  bool Execute(RegenContext& ctx) override;
  uint64_t MaterialSource() const override { return targetId; }

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

  // A mesh boolean's tessellation of its B-Rep operands, for the reply
  // that made it (null for a B-Rep boolean).
  [[nodiscard]] nlohmann::json ReportJson() const;

  uint64_t targetId = 0; // producing-feature id of operand A
  uint64_t toolId = 0;   // producing-feature id of operand B
  BooleanOp op = BooleanOp::Union;

private:
  bool ExecuteMesh(RegenContext& ctx, std::vector<Body>::iterator target,
                   std::vector<Body>::iterator tool);
  struct MeshReport {
    bool mesh = false;
    double targetDeflectionMm = 0.0; // 0: a mesh body
    double toolDeflectionMm = 0.0;
    size_t triangles = 0;
  };
  MeshReport m_meshReport;
};
