// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "core/Feature.h"
#include "core/mesh/Field.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>

// Cuts a holder shaped to a scan out of a block
// (docs/modeling/README.md#cradle): the target body less the tool -- a scan,
// a mesh body or a B-Rep -- grown by a clearance and swept along the
// direction the object is lowered in, so it lifts in and out. The pocket is
// built in a distance field (mesh::Pocket) and only the last step, the
// subtraction, is a mesh boolean (manifold). The target becomes a mesh body;
// the tool stays.
//
// Heavy: it builds off the frame, as a `feature` job
// (docs/ui/long-operations.md#heavy-features), and is refused past a cost
// budget, naming what fits.
class CradleFeature : public Feature {
public:
  CradleFeature(const std::string& name, uint64_t targetId, uint64_t toolId);

  [[nodiscard]] std::string TypeName() const override { return "Cradle"; }

  std::unique_ptr<Feature> Clone() const override {
    return std::make_unique<CradleFeature>(*this);
  }

  void CopyDerivedState(const Feature& executed) override {
    RestoreExecuted<CradleFeature>(executed);
  }

  bool Execute(RegenContext& ctx) override;
  uint64_t MaterialSource() const override { return targetId; }

  void VisitFeatureRefs(const RefVisitor& visit) const override {
    visit(targetId); // both bodies are named by their producing feature
    visit(toolId);
  }

  void VisitExprs(const ExprVisitor& visit) override {
    if (hasClearance)
      visit(clearanceExpr, clearanceMm);
    if (hasResolution)
      visit(resolutionExpr, resolutionMm);
  }

  [[nodiscard]] std::string WhyIncomplete() const override {
    if (targetId == 0)
      return "pick the block to cut the cradle from";
    if (toolId == 0)
      return "pick the scan to shape it to";
    return "";
  }

  [[nodiscard]] std::string WhyRefusedOn(const std::vector<Body>& input,
                                         const Document& doc) const override;

  [[nodiscard]] bool MayBuildOffFrame() const override { return true; }
  [[nodiscard]] double
  BuildCostS(const std::vector<Body>& input,
             const printfit::Process* print) const override;

  // What the last Execute used, for the reply that made it: the clearance
  // and where it came from, the resolution and the deviation it implies,
  // the grid, a B-Rep operand's tessellation.
  [[nodiscard]] nlohmann::json ReportJson() const;

  uint64_t targetId = 0; // producing-feature id of the block
  uint64_t toolId = 0;   // ... of the scan
  // Unset: the part's print process's gap a side
  // (docs/tolerances/README.md#printed-parts); refused without one.
  bool hasClearance = false;
  double clearanceMm = 0.0;
  std::string clearanceExpr;
  // The way the object moves INTO the cradle: world -Z, down, unless set.
  glm::dvec3 direction{0.0, 0.0, -1.0};
  // false keeps the undercuts: a snap-in grip.
  bool sweep = true;
  // Unset: half the clearance, coarsened to fit the budget, never past the
  // clearance.
  bool hasResolution = false;
  double resolutionMm = 0.0;
  std::string resolutionExpr;

  // What a plan of this cradle on some bodies comes to: what Execute,
  // BuildCostS and WhyRefusedOn all read, so they cannot disagree.
  struct Plan {
    std::string refusal; // non-empty: why it cannot be built
    const Body* target = nullptr;
    const Body* tool = nullptr;
    double clearanceMm = 0.0;
    std::string clearanceFrom; // "clearance_mm" or the print process
    double cellMm = 0.0;
    bool coarsened = false; // the default cell, coarsened to the budget
    mesh::PocketGrid grid;
    double estimateS = 0.0;
    double memoryBytes = 0.0;
  };
  [[nodiscard]] Plan MakePlan(const std::vector<Body>& bodies,
                              const printfit::Process* print) const;

private:
  struct Report {
    bool built = false;
    double clearanceMm = 0.0;
    std::string clearanceFrom;
    double resolutionMm = 0.0;
    bool coarsened = false;
    double deviationMm = 0.0;
    size_t cells = 0;
    double estimateS = 0.0;
    double toolDeflectionMm = 0.0;   // 0: a mesh body
    double targetDeflectionMm = 0.0; // 0: a mesh body
    size_t triangles = 0;            // the result's
  };
  Report m_report;
};

namespace cradle {

// The cost budget a Cradle is refused past: the patient wait an import is
// held to (interchange::kPatientImportSeconds), and the memory the level set
// may take (docs/modeling/README.md#what-a-cradle-costs).
double BudgetS();
double MemoryBudgetBytes();

// The estimated seconds and bytes of a pocket on `grid` around a tool of
// `toolTriangles` and surface `toolAreaMm2`, cut from a target of
// `targetTriangles`: the cost law measured on the fixtures.
double EstimateS(const mesh::PocketGrid& grid, double clearanceMm,
                 size_t toolTriangles, double toolAreaMm2,
                 size_t targetTriangles);
double EstimateBytes(const mesh::PocketGrid& grid);

} // namespace cradle
