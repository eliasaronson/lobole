#pragma once

#include "core/ElementMap.h"
#include "core/Feature.h"

#include <cstdint>
#include <vector>

enum class EdgeBlendOp { Fillet, Chamfer };

// Rounds (fillet) or bevels (chamfer) edges of one body, referenced by stable
// ElementRefs (modeling M7 / toponaming T5): each ref names an edge in the
// element map of the body produced by ref.featureId, so the blend follows
// upstream edits that renumber edges. All refs must address the same body.
// The body is modified in place (its createdBy stays the producer's id, like
// extrude Add/Cut) and its element map is re-propagated through the blend:
// each blend face is named "<spineEdgeName>;G<id>". Regeneration fails with a
// per-feature error when a ref no longer resolves (its generating entity was
// deleted or a boolean consumed the edge) or OCC rejects the blend (radius
// too large for the geometry); the UI offers re-picking the edges. An
// edge-less blend is a no-op success so a just-created feature can preview
// while the user picks edges.
class FilletFeature : public Feature {
public:
  FilletFeature(const std::string& name, std::vector<ElementRef> edges,
                double radius, EdgeBlendOp op = EdgeBlendOp::Fillet);

  std::string TypeName() const override {
    return op == EdgeBlendOp::Chamfer ? "Chamfer" : "Fillet";
  }

  std::unique_ptr<Feature> Clone() const override {
    return std::make_unique<FilletFeature>(*this);
  }

  bool Execute(RegenContext& ctx) override;

  void VisitFeatureRefs(const RefVisitor& visit) const override {
    VisitRefs(edges, visit);
  }

  [[nodiscard]] std::string WhyIncomplete() const override {
    return edges.empty()
               ? std::string("pick at least one edge to ") +
                     (op == EdgeBlendOp::Chamfer ? "chamfer" : "fillet")
               : std::string();
  }

  void VisitExprs(const ExprVisitor& visit) override {
    visit(radiusExpr, radius);
  }

  std::vector<ElementRef> edges; // all share featureId (one body per feature)
  EdgeBlendOp op = EdgeBlendOp::Fillet;
  double radius = 0.5; // fillet radius / chamfer distance
  // Optional expression driving `radius` (empty = plain literal), evaluated
  // against the document's parameters on every regeneration.
  std::string radiusExpr;
};
