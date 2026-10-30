// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/features/CradleFeature.h"

#include "common/Logger.h"
#include "core/Document.h"
#include "core/HeavyBuild.h"
#include "core/MeshOperand.h"
#include "core/Parallel.h"
#include "core/PrintAllowance.h"
#include "core/io/MeshImportCost.h"
#include "core/mesh/MeshBoolean.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

CradleFeature::CradleFeature(const std::string& name, uint64_t targetId,
                             uint64_t toolId)
    : targetId(targetId), toolId(toolId) {
  m_name = name;
}

namespace {

std::string Mm(double v) {
  char buf[64];
  std::snprintf(buf, sizeof buf, "%.3g", v);
  return buf;
}

// The cost law (docs/modeling/README.md#what-a-cradle-costs), measured with
// `[mesh-cost-cradle-field]` and `[mesh-cost-cradle-feature]`
// (tests/test_mesh_cradle.cpp) on the figure at 10k, 300k and 2M triangles,
// on the development machine: the field, about 2 µs of CPU a node shared by
// the workers (0.68 s for 7.5M nodes on 22 threads); manifold's level set on
// one thread, 0.62 µs a node at 7.5M and 0.88 at 30M (26 s), taken at the
// larger; and 2.4 µs a triangle of the scan (its tree), the block and the
// pocket (the subtraction: 1.5 s for a 0.3 mm pocket's 785k).
constexpr double kNodeS = 2.0e-6;
constexpr double kLevelSetNodeS = 0.9e-6;
constexpr double kTriangleS = 2.4e-6;

double Workers() { return double(std::max<size_t>(1, parallel::Workers())); }

double BoxArea(const glm::dvec3& lo, const glm::dvec3& hi) {
  const glm::dvec3 d = hi - lo;
  return 2.0 * (d.x * d.y + d.y * d.z + d.z * d.x);
}

} // namespace

namespace cradle {

double BudgetS() { return interchange::kPatientImportSeconds; }

// What the level set may hold: what stage 0 measured it at for the cells
// the time budget allows serially (about 4e7 cells, 1.7 GB), rounded up.
double MemoryBudgetBytes() { return 2.0 * 1024.0 * 1024.0 * 1024.0; }

double EstimateBytes(const mesh::PocketGrid& grid) {
  return mesh::PocketBytes(grid);
}

double EstimateS(const mesh::PocketGrid& grid, double clearanceMm,
                 size_t toolTriangles, double toolAreaMm2,
                 size_t targetTriangles) {
  (void)clearanceMm;
  if (grid.Empty())
    return 0.0;
  const double h = grid.cellMm;
  const double cells = double(grid.Cells());
  // The pocket's triangles: about two a cell of the surface it contours.
  const double pocketTriangles = 2.0 * toolAreaMm2 / (h * h);
  return kNodeS * cells / Workers() + kLevelSetNodeS * cells +
         kTriangleS * (double(toolTriangles) + double(targetTriangles) +
                       pocketTriangles);
}

} // namespace cradle

CradleFeature::Plan
CradleFeature::MakePlan(const std::vector<Body>& bodies,
                        const printfit::Process* print) const {
  Plan plan;
  if (targetId == 0 || toolId == 0) {
    plan.refusal = WhyIncomplete();
    return plan;
  }
  if (targetId == toolId) {
    plan.refusal = "the block and the scan are the same body";
    return plan;
  }
  const auto byOwner = [&](uint64_t id) -> const Body* {
    for (const Body& b : bodies)
      if (b.createdBy == id)
        return &b;
    return nullptr;
  };
  plan.target = byOwner(targetId);
  plan.tool = byOwner(toolId);
  if (plan.target == nullptr || plan.tool == nullptr) {
    plan.refusal = std::string("the ") +
                   (plan.target == nullptr ? "block" : "scan") +
                   " body was not found (consumed earlier, or its feature "
                   "made none)";
    return plan;
  }
  if (plan.target->reference) {
    plan.refusal = "'" + plan.target->name +
                   "' is a derived reference body, not this part's matter — "
                   "set reference: false on its Derived feature to cut it";
    return plan;
  }
  const double dirLen = glm::length(direction);
  if (!(dirLen > 1e-12) || !std::isfinite(dirLen)) {
    plan.refusal = "direction must be a non-zero vector: the way the object "
                   "moves into the cradle (world -Z, [0, 0, -1], is down)";
    return plan;
  }
  glm::dvec3 tlo, thi, blo, bhi;
  if (!BodyBounds(*plan.tool, tlo, thi) ||
      !BodyBounds(*plan.target, blo, bhi)) {
    plan.refusal = "a body has no geometry";
    return plan;
  }
  // The clearance: stated, or the print process's gap a side -- what keeps
  // two surfaces from closing on each other once printed
  // (docs/tolerances/README.md#printed-parts).
  if (hasClearance) {
    if (!(clearanceMm > 0.0) || !std::isfinite(clearanceMm)) {
      plan.refusal = "clearance_mm must be more than 0 (it is " +
                     Mm(clearanceMm) +
                     "): the gap a side between the scan "
                     "and the pocket wall";
      return plan;
    }
    plan.clearanceMm = clearanceMm;
    plan.clearanceFrom = "clearance_mm";
  } else {
    if (print == nullptr) {
      plan.refusal =
          "a Cradle needs its clearance: the part has no print process to "
          "take its gap from — set one with set_print_process, or give "
          "clearance_mm";
      return plan;
    }
    const glm::dvec3 size = thi - tlo;
    const double basic = std::max({size.x, size.y, size.z});
    std::string why;
    const auto gap =
        printfit::ForBasic(*print, printfit::Printed::Gap, basic, &why);
    if (!gap || !(gap->allowanceMm > 0.0)) {
      plan.refusal = "the print process gives no gap to take the clearance "
                     "from (" +
                     (why.empty() ? std::string("its gap is 0") : why) +
                     "): give clearance_mm";
      return plan;
    }
    plan.clearanceMm = gap->allowanceMm / 2.0;
    plan.clearanceFrom = "the gap a side of " + printfit::Label(*print);
  }
  const double c = plan.clearanceMm;
  if (hasResolution) {
    if (!(resolutionMm > 0.0) || !std::isfinite(resolutionMm)) {
      plan.refusal =
          "resolution_mm must be more than 0 (it is " + Mm(resolutionMm) + ")";
      return plan;
    }
    if (resolutionMm > c) {
      plan.refusal = "resolution_mm " + Mm(resolutionMm) +
                     " is coarser than the clearance " + Mm(c) +
                     ": the pocket wall could stand inside the scan. Give a "
                     "resolution of at most the clearance";
      return plan;
    }
  }
  mesh::PocketOptions o;
  o.clearanceMm = c;
  o.direction = direction / dirLen;
  o.sweep = sweep;
  o.targetLo = blo;
  o.targetHi = bhi;
  const size_t toolTris =
      plan.tool->IsMesh() ? plan.tool->mesh->TriangleCount() : 0;
  const size_t targetTris =
      plan.target->IsMesh() ? plan.target->mesh->TriangleCount() : 0;
  const double area =
      plan.tool->IsMesh() ? plan.tool->mesh->SurfaceArea() : BoxArea(tlo, thi);
  const auto cost = [&](double h, mesh::PocketGrid& grid, double& s,
                        double& bytes) {
    o.cellMm = h;
    grid = mesh::PlanPocket(tlo, thi, o);
    s = cradle::EstimateS(grid, c, toolTris, area, targetTris);
    bytes = cradle::EstimateBytes(grid);
    return s <= cradle::BudgetS() && bytes <= cradle::MemoryBudgetBytes();
  };
  const double asked = hasResolution ? resolutionMm : c / 2.0;
  plan.cellMm = asked;
  if (cost(asked, plan.grid, plan.estimateS, plan.memoryBytes)) {
    if (plan.grid.Empty())
      plan.refusal = "the scan, grown by the clearance, does not reach the "
                     "block";
    return plan;
  }
  // Past the budget: the coarsest cell it takes to fit, in steps of 5%.
  double fits = 0.0;
  for (double h = asked * 1.05; h < asked * 1e3; h *= 1.05) {
    mesh::PocketGrid g;
    double s = 0.0, b = 0.0;
    if (cost(h, g, s, b)) {
      fits = h;
      break;
    }
  }
  const std::string over =
      "past the cost budget (" + Mm(cradle::BudgetS()) + " s, " +
      Mm(cradle::MemoryBudgetBytes() / (1024.0 * 1024.0 * 1024.0)) +
      " GB): at a " + Mm(asked) + " mm cell it is " +
      std::to_string(plan.grid.Cells()) + " cells, about " +
      Mm(plan.estimateS) + " s and " +
      Mm(plan.memoryBytes / (1024.0 * 1024.0 * 1024.0)) + " GB";
  // The tool's triangles alone over the budget: no cell helps.
  const double perTriangle = kTriangleS;
  const double triangleS = perTriangle * double(toolTris + targetTris);
  if (fits == 0.0 || triangleS > cradle::BudgetS()) {
    const size_t fitTris =
        size_t(std::max(0.0, cradle::BudgetS() / 2.0 / perTriangle));
    plan.refusal = "a Cradle " + over + ". Decimate the scan externally " +
                   "(MeshLab's Quadric Edge Collapse Decimation, Blender's " +
                   "Decimate modifier) to under " + std::to_string(fitTris) +
                   " triangles, or cut a smaller block";
    return plan;
  }
  if (hasResolution || fits > c) {
    plan.refusal =
        "a Cradle " + over + ". " +
        (fits <= c
             ? "resolution_mm " + Mm(fits) + " fits it"
             : "the cell that fits, " + Mm(fits) +
                   " mm, is coarser than the clearance " + Mm(c) + " mm") +
        "; or cut a smaller block (the grid covers only the block the scan "
        "reaches), or decimate the scan externally (MeshLab, Blender)";
    return plan;
  }
  plan.cellMm = fits;
  plan.coarsened = true;
  cost(fits, plan.grid, plan.estimateS, plan.memoryBytes);
  return plan;
}

std::string CradleFeature::WhyRefusedOn(const std::vector<Body>& input,
                                        const Document& doc) const {
  const auto& print = doc.PrintProcess();
  const Plan plan = MakePlan(input, print ? &*print : nullptr);
  return plan.refusal.empty() ? "" : "Cradle: " + plan.refusal;
}

double CradleFeature::BuildCostS(const std::vector<Body>& input,
                                 const printfit::Process* print) const {
  const Plan plan = MakePlan(input, print);
  return plan.refusal.empty() ? plan.estimateS : 0.0;
}

bool CradleFeature::Execute(RegenContext& ctx) {
  m_report = Report{};
  const Plan plan = MakePlan(ctx.bodies, ctx.print);
  if (!plan.refusal.empty()) {
    SetError(plan.refusal);
    return false;
  }
  // The scan's tessellation, when it is a B-Rep, is never the coarse part:
  // its own size's deflection, or half a cell when that is finer.
  const double toolDeflection =
      std::min(meshop::DerivedDeflectionMm(*plan.tool), plan.cellMm / 2.0);
  double usedTool = 0.0, usedTarget = 0.0;
  const auto tool = meshop::Triangles(*plan.tool, toolDeflection, usedTool);
  const auto target = meshop::Triangles(*plan.target, 0.0, usedTarget);
  if (tool->triangles.empty() || target->triangles.empty()) {
    SetError("a body has no triangles");
    return false;
  }
  if (const std::string why = meshop::OpenOperandRefusal(
          *target, plan.target->name, "The block a Cradle cuts");
      !why.empty()) {
    SetError(why);
    return false;
  }
  mesh::PocketOptions o;
  o.clearanceMm = plan.clearanceMm;
  o.direction = glm::normalize(direction);
  o.sweep = sweep;
  o.cellMm = plan.cellMm;
  BodyBounds(*plan.target, o.targetLo, o.targetHi);
  o.cancelled = [] { return heavy::Cancelled(); };
  o.progress = [](double f) { heavy::ReportFraction(0.9 * f); };
  mesh::TriMesh pocket;
  mesh::PocketStats stats;
  std::string error;
  if (!mesh::Pocket(*tool, o, pocket, stats, error)) {
    SetError(stats.cancelled ? "cancelled" : error);
    return false;
  }
  if (heavy::Cancelled()) {
    SetError("cancelled");
    return false;
  }
  mesh::TriMesh result;
  if (!mesh::Boolean(*target, pocket, mesh::BooleanOp::Subtract, result,
                     error)) {
    SetError("the subtraction failed: " + error);
    return false;
  }
  heavy::ReportFraction(1.0);
  if (result.triangles.empty()) {
    SetError("the pocket takes the whole block: nothing is left of '" +
             plan.target->name + "'");
    return false;
  }
  LOG_INFO("Cradle '{}': {} cells at {} mm, field {:.2f} s, level set {:.2f} "
           "s, {} triangles",
           m_name, stats.grid.Cells(), plan.cellMm, stats.fieldS,
           stats.levelSetS, result.TriangleCount());
  // The block becomes a mesh body: the same body (its producer, name and
  // material), its one element this feature's.
  Body& body = *std::find_if(ctx.bodies.begin(), ctx.bodies.end(),
                             [&](const Body& b) { return &b == plan.target; });
  const bool wasBrep = !body.IsMesh();
  m_report.built = true;
  m_report.clearanceMm = plan.clearanceMm;
  m_report.clearanceFrom = plan.clearanceFrom;
  m_report.resolutionMm = plan.cellMm;
  m_report.coarsened = plan.coarsened;
  m_report.deviationMm = mesh::PocketDeviationMm(plan.cellMm) + usedTool;
  m_report.cells = stats.grid.Cells();
  m_report.estimateS = plan.estimateS;
  m_report.toolDeflectionMm = usedTool;
  m_report.targetDeflectionMm = usedTarget;
  m_report.triangles = result.TriangleCount();
  body.mesh = std::make_shared<const mesh::TriMesh>(std::move(result));
  body.shape = CADShape();
  body.elementMap = ElementMap{};
  body.elementMap.names[0] = std::to_string(GetId()) + "/mesh";
  if (wasBrep)
    body.meshSince = "Cradle '" + m_name + "'";
  // What named its faces names nothing now.
  body.faceAppearances.clear();
  body.threadRegions.clear();
  body.ringGrips.clear();
  body.bends.clear();
  body.threads.clear();
  body.holes.clear();
  body.bands.clear();
  body.fitBands.clear();
  return true;
}

nlohmann::json CradleFeature::ReportJson() const {
  if (!m_report.built)
    return nullptr;
  nlohmann::json j = {
      {"clearance_mm", m_report.clearanceMm},
      {"clearance_from", m_report.clearanceFrom},
      {"resolution_mm", m_report.resolutionMm},
      {"deviation_mm", m_report.deviationMm},
      {"cells", m_report.cells},
      {"estimate_s", m_report.estimateS},
      {"triangles", m_report.triangles},
  };
  if (m_report.coarsened)
    j["resolution_note"] =
        "coarsened from half the clearance to fit the cost budget";
  if (m_report.toolDeflectionMm > 0.0)
    j["tool_deflection_mm"] = m_report.toolDeflectionMm;
  if (m_report.targetDeflectionMm > 0.0)
    j["target_deflection_mm"] = m_report.targetDeflectionMm;
  return j;
}
