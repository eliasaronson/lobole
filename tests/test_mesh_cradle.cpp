// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

// The cradle's pocket (src/core/mesh/Field.{h,cpp},
// docs/modeling/README.md#cradle), checked by what it is for: the pocket
// wall stands at the scan plus the clearance, the swept pocket lets the
// object down without trapping it, and the block it is cut from stays a
// closed solid.

#include "mesh_organic.h"

#include "core/Document.h"
#include "core/Feature.h"
#include "core/PrintAllowance.h"
#include "core/features/BooleanFeature.h"
#include "core/features/CradleFeature.h"
#include "core/io/MeshImportCost.h"
#include "core/io/Serialization.h"

#include "core/mesh/Field.h"
#include "core/mesh/MeshBoolean.h"
#include "core/mesh/MeshBvh.h"
#include "core/mesh/Slice.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <memory>

namespace {

mesh::TriMesh Block(const glm::dvec3& lo, const glm::dvec3& hi) {
  mesh::TriMesh block;
  for (int c = 0; c < 8; c++)
    block.positions.push_back(
        {c & 1 ? hi.x : lo.x, c & 2 ? hi.y : lo.y, c & 4 ? hi.z : lo.z});
  block.triangles = {{0, 2, 3}, {0, 3, 1}, {4, 5, 7}, {4, 7, 6},
                     {0, 1, 5}, {0, 5, 4}, {2, 6, 7}, {2, 7, 3},
                     {0, 4, 6}, {0, 6, 2}, {1, 3, 7}, {1, 7, 5}};
  mesh::BuildAdjacency(block);
  return block;
}

mesh::PocketOptions Options(double c, double h, bool sweep,
                            const glm::dvec3& lo, const glm::dvec3& hi) {
  mesh::PocketOptions o;
  o.clearanceMm = c;
  o.cellMm = h;
  o.sweep = sweep;
  o.targetLo = lo;
  o.targetHi = hi;
  return o;
}

} // namespace

TEST_CASE("Cradle pocket: the wall stands at the radius plus the clearance",
          "[mesh][cradle]") {
  const auto s = fixtures::NoisySphere(20000, 20.0, 0.0);
  const double c = 0.4, h = 0.25;
  const glm::dvec3 lo(-30, -30, -30), hi(30, 30, 8);
  for (const bool sweep : {true, false}) {
    INFO("sweep " << sweep);
    mesh::TriMesh pocket;
    mesh::PocketStats stats;
    std::string error;
    REQUIRE(mesh::Pocket(s.mesh, Options(c, h, sweep, lo, hi), pocket, stats,
                         error));
    CHECK(pocket.Closed());
    const double dev = mesh::PocketDeviationMm(h) + s.radiusTolerance;
    size_t below = 0, above = 0;
    for (const glm::dvec3& p : pocket.positions) {
      // The grid's own caps, past the target, are not the wall.
      if (p.z > hi.z)
        continue;
      if (p.z <= 0.0) {
        // Below the equator: a sphere of r + c.
        CHECK(std::abs(glm::length(p) - (s.radius + c)) <= dev);
        below++;
      } else if (sweep) {
        // Above it, swept: a cylinder of r + c straight up.
        CHECK(std::abs(std::hypot(p.x, p.y) - (s.radius + c)) <= dev);
        above++;
      } else {
        CHECK(std::abs(glm::length(p) - (s.radius + c)) <= dev);
        above++;
      }
    }
    CHECK(below > 100);
    CHECK(above > 100);
  }
}

TEST_CASE("Cradle pocket: a block cut by it stays closed, and the object "
          "lowers into it without a clash only when swept",
          "[mesh][cradle]") {
  const auto s = fixtures::NoisySphere(20000, 20.0, 0.0);
  const double c = 0.4, h = 0.25;
  const glm::dvec3 lo(-30, -30, -30), hi(30, 30, 8);
  const mesh::TriMesh block = Block(lo, hi);
  for (const bool sweep : {true, false}) {
    INFO("sweep " << sweep);
    mesh::TriMesh pocket, cradle;
    mesh::PocketStats stats;
    std::string error;
    REQUIRE(mesh::Pocket(s.mesh, Options(c, h, sweep, lo, hi), pocket, stats,
                         error));
    REQUIRE(
        mesh::Boolean(block, pocket, mesh::BooleanOp::Subtract, cradle, error));
    CHECK(cradle.Closed());
    // At z = 5, above the equator: the opening is 2(r + c) wide when swept,
    // and closes in over the sphere when not.
    const auto slice = mesh::Slice(cradle, {0, 0, 5.0}, {0, 0, 1});
    double widest = 0.0;
    for (const auto& loop : slice.polylines)
      for (const glm::dvec3& p : loop.points)
        if (std::hypot(p.x, p.y) < 25.0)
          widest = std::max(widest, std::hypot(p.x, p.y));
    if (sweep)
      CHECK(std::abs(widest - (s.radius + c)) <= mesh::PocketDeviationMm(h));
    else
      CHECK(widest < std::sqrt((s.radius + c) * (s.radius + c) - 25.0) +
                         mesh::PocketDeviationMm(h));
    // Lower the sphere in along -Z from above the block: the purpose.
    const auto cradleTree = std::make_shared<const mesh::TriMesh>(cradle);
    const mesh::Bvh tree(cradleTree);
    const mesh::Bvh ball(std::make_shared<const mesh::TriMesh>(s.mesh));
    bool clashed = false;
    for (double z = 40.0; z >= 0.0; z -= 1.0) {
      glm::dmat4 xf(1.0);
      xf[3] = glm::dvec4(0, 0, z, 1);
      if (tree.Distance(ball, xf).distance < 1e-9)
        clashed = true;
    }
    CHECK(clashed == !sweep);
  }
}

TEST_CASE("Cradle pocket: an open scan's missing bottom still pockets",
          "[mesh][cradle]") {
  // The cut figure: open, one loop round its base, as a phone scan of an
  // object on a table arrives. The winding number still tells its inside.
  const auto f = fixtures::CutFigurine(10000);
  REQUIRE(!f.mesh.Closed());
  glm::dvec3 flo, fhi;
  f.mesh.Bounds(flo, fhi);
  const glm::dvec3 lo = flo - glm::dvec3(6, 6, 2),
                   hi = {fhi.x + 6, fhi.y + 6, 40.0};
  const mesh::TriMesh block = Block(lo, hi);
  mesh::TriMesh pocket, cradle;
  mesh::PocketStats stats;
  std::string error;
  REQUIRE(mesh::Pocket(f.mesh, Options(0.5, 0.5, true, lo, hi), pocket, stats,
                       error));
  REQUIRE(
      mesh::Boolean(block, pocket, mesh::BooleanOp::Subtract, cradle, error));
  CHECK(cradle.Closed());
  // The figure's inside is carved out: a point well inside its body, at
  // mid-height of the block, is no longer in the cradle.
  const mesh::Bvh tree(std::make_shared<const mesh::TriMesh>(cradle));
  const mesh::Bvh fig(std::make_shared<const mesh::TriMesh>(f.mesh));
  const glm::dvec3 mid = 0.5 * (flo + fhi);
  const glm::dvec3 probe(mid.x, mid.y, 10.0);
  if (std::abs(fig.Winding(probe)) > 0.5)
    CHECK(std::abs(tree.Winding(probe)) < 0.5);
  // And the open scan as a boolean operand is refused, naming why.
  mesh::TriMesh direct;
  CHECK_FALSE(
      mesh::Boolean(block, f.mesh, mesh::BooleanOp::Subtract, direct, error));
  CHECK(error.find("open") != std::string::npos);
}

TEST_CASE("Cradle pocket: the cost of each piece, by size",
          "[.][mesh-cost-cradle-field]") {
  // Prints the field, the level set and the subtraction at each cell size
  // and figure size, for the cost law CradleFeature::BuildCostS states.
  const auto now = [] {
    return std::chrono::duration<double>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
  };
  for (size_t n : {10000, 300000}) {
    const auto f = fixtures::Figurine(n);
    glm::dvec3 flo, fhi;
    f.mesh.Bounds(flo, fhi);
    const glm::dvec3 lo = flo - glm::dvec3(6, 6, 2),
                     hi = {fhi.x + 6, fhi.y + 6, 0.5 * (flo.z + fhi.z)};
    const mesh::TriMesh block = Block(lo, hi);
    for (double h : {1.0, 0.5, 0.3}) {
      mesh::TriMesh pocket, cradle;
      mesh::PocketStats stats;
      std::string error;
      const double t0 = now();
      const mesh::PocketOptions o = Options(0.3, h, true, lo, hi);
      const bool ok = mesh::Pocket(f.mesh, o, pocket, stats, error);
      const double t1 = now();
      const bool cut =
          ok && mesh::Boolean(block, pocket, mesh::BooleanOp::Subtract, cradle,
                              error);
      const double t2 = now();
      std::printf("COST figure %zu h %.2f | cells %zu band %zu windings %zu | "
                  "field %.2f s level %.2f s total %.2f s | boolean %.2f s | "
                  "pocket %zu tris, cradle %zu, closed %d %s\n",
                  f.mesh.TriangleCount(), h, stats.grid.Cells(),
                  stats.bandNodes, stats.windingCalls, stats.fieldS,
                  stats.levelSetS, t1 - t0, t2 - t1, pocket.TriangleCount(),
                  cradle.TriangleCount(), int(cut && cradle.Closed()),
                  error.c_str());
      std::fflush(stdout);
    }
  }
}

namespace {

constexpr double kPi = 3.14159265358979323846;
using Catch::Matchers::ContainsSubstring;

// A 60 x 60 x 38 block whose top is 8 mm above the centre of a B-Rep
// sphere of radius 20 sitting in it, and a cradle of the sphere.
struct Scene {
  Document doc;
  uint64_t blockId = 0, ballId = 0;
  CradleFeature* cradle = nullptr;
};
std::unique_ptr<Scene> MakeScene(bool hasClearance, double clearanceMm,
                                 double resolutionMm, bool sweep = true) {
  auto s = std::make_unique<Scene>();
  s->doc.AddFeature(std::make_unique<BoxFeature>(
      "Blank", glm::dvec3(-30, -30, -30), 60.0, 60.0, 38.0));
  s->doc.AddFeature(
      std::make_unique<SphereFeature>("Ball", glm::dvec3(0.0), 20.0));
  s->blockId = s->doc.GetFeature(0)->GetId();
  s->ballId = s->doc.GetFeature(1)->GetId();
  auto cr = std::make_unique<CradleFeature>("Holder", s->blockId, s->ballId);
  cr->hasClearance = hasClearance;
  cr->clearanceMm = clearanceMm;
  cr->hasResolution = resolutionMm > 0.0;
  cr->resolutionMm = resolutionMm;
  cr->sweep = sweep;
  s->doc.AddFeature(std::move(cr));
  s->cradle = dynamic_cast<CradleFeature*>(s->doc.GetFeature(2));
  return s;
}

const Body* BodyOf(const Document& doc, uint64_t createdBy) {
  for (const Body& b : doc.GetBodies())
    if (b.createdBy == createdBy)
      return &b;
  return nullptr;
}

} // namespace

TEST_CASE("Cradle: a block cut to a sphere is a closed mesh body of the "
          "volume the swept pocket leaves",
          "[mesh][cradle]") {
  auto s = MakeScene(true, 0.5, 0.5);
  REQUIRE(s->cradle->GetError().empty());
  const Body* block = BodyOf(s->doc, s->blockId);
  REQUIRE(block != nullptr);
  REQUIRE(block->IsMesh());
  CHECK(block->mesh->Closed());
  // The scan stays.
  const Body* ball = BodyOf(s->doc, s->ballId);
  REQUIRE(ball != nullptr);
  CHECK_FALSE(ball->IsMesh());
  // Block less a hemisphere of r + c and the column of r + c above it.
  const double r = 20.5;
  const double removed = 2.0 / 3.0 * kPi * r * r * r + kPi * r * r * 8.0;
  const double expect = 60.0 * 60.0 * 38.0 - removed;
  const nlohmann::json rep = s->cradle->ReportJson();
  REQUIRE(rep.is_object());
  // The pocket's wall within the reported deviation, over its area.
  const double wall = 2.0 * kPi * r * r + 2.0 * kPi * r * 8.0;
  CHECK(std::abs(block->mesh->Volume() - expect) <=
        rep.at("deviation_mm").get<double>() * wall);
  CHECK(rep.at("clearance_from") == "clearance_mm");
  CHECK(rep.at("resolution_mm").get<double>() == 0.5);
  CHECK(rep.at("tool_deflection_mm").get<double>() > 0.0);
  // A later B-Rep feature on it is refused, naming the cradle.
  const std::string why = MeshBodyRefusal(*block, "Fillet");
  CHECK_THAT(why, ContainsSubstring("Cradle 'Holder'"));
  CHECK_THAT(why, ContainsSubstring("B-Rep work"));
}

TEST_CASE("Cradle: its clearance comes from the print process, and without "
          "one or a clearance_mm it is refused",
          "[mesh][cradle]") {
  auto s = MakeScene(false, 0.0, 0.0);
  CHECK_THAT(s->cradle->GetError(), ContainsSubstring("set_print_process"));
  CHECK_THAT(s->cradle->GetError(), ContainsSubstring("clearance_mm"));
  CHECK(BodyOf(s->doc, s->blockId)->IsMesh() == false);
  // With a process, its gap a side: PETG's is 0.6 on the diameter. The
  // grid is coarse here only to keep the test quick.
  s->cradle->hasResolution = true;
  s->cradle->resolutionMm = 0.3;
  s->doc.SetPrintProcess(printfit::Process{"fdm", "petg", std::nullopt});
  s->doc.Regenerate();
  REQUIRE(s->cradle->GetError().empty());
  const nlohmann::json rep = s->cradle->ReportJson();
  CHECK(rep.at("clearance_mm").get<double>() == 0.3);
  CHECK_THAT(rep.at("clearance_from").get<std::string>(),
             ContainsSubstring("petg"));
}

TEST_CASE("Cradle: each bad input is refused, naming what is accepted",
          "[mesh][cradle]") {
  auto s = MakeScene(true, 0.5, 0.5);
  const auto refusal = [&](const std::function<void(CradleFeature&)>& edit) {
    auto copy = std::unique_ptr<CradleFeature>(
        dynamic_cast<CradleFeature*>(s->cradle->Clone().release()));
    edit(*copy);
    return copy->WhyRefusedOn(*s->doc.CachedBodiesAfter(1), s->doc);
  };
  CHECK(refusal([](CradleFeature&) {}).empty());
  CHECK_THAT(refusal([](CradleFeature& c) { c.clearanceMm = 0.0; }),
             ContainsSubstring("more than 0"));
  CHECK_THAT(refusal([](CradleFeature& c) { c.direction = glm::dvec3(0.0); }),
             ContainsSubstring("non-zero"));
  CHECK_THAT(refusal([](CradleFeature& c) { c.resolutionMm = 0.6; }),
             ContainsSubstring("coarser than the clearance"));
  CHECK_THAT(refusal([&](CradleFeature& c) { c.toolId = c.targetId; }),
             ContainsSubstring("same body"));
  // Past the budget: a stated resolution is refused naming the one that
  // fits, and that one is accepted.
  const std::string over =
      refusal([](CradleFeature& c) { c.resolutionMm = 0.005; });
  CHECK_THAT(over, ContainsSubstring("cost budget"));
  const size_t at = over.find("resolution_mm ", over.find("budget"));
  REQUIRE(at != std::string::npos);
  const double fits = std::atof(over.c_str() + at + 14);
  CHECK(fits > 0.005);
  CHECK(refusal([&](CradleFeature& c) { c.resolutionMm = fits; }).empty());
  // An unknown spelling of a field never loads as a default.
  CHECK_THROWS(DocumentIO::FeatureFromJson({{"type", "Cradle"},
                                            {"target_id", 1},
                                            {"tool_id", 2},
                                            {"direction", "down"}}));
  CHECK_THROWS(DocumentIO::FeatureFromJson(
      {{"type", "Cradle"}, {"target_id", 1}, {"tool_id", 2}, {"sweep", "no"}}));
}

TEST_CASE("Cradle: the defaulted cell is coarsened to fit the budget, never "
          "past the clearance",
          "[mesh][cradle]") {
  auto s = MakeScene(true, 0.5, 0.0);
  const CradleFeature::Plan plan =
      s->cradle->MakePlan(*s->doc.CachedBodiesAfter(1), nullptr);
  REQUIRE(plan.refusal.empty());
  CHECK(plan.cellMm >= 0.25);
  CHECK(plan.cellMm <= 0.5);
  CHECK(plan.estimateS <= cradle::BudgetS());
  CHECK(plan.memoryBytes <= cradle::MemoryBudgetBytes());
  // A clearance so fine that half of it cannot fit: the cell that would is
  // coarser than the clearance, and it is refused.
  s->cradle->clearanceMm = 0.004;
  const CradleFeature::Plan fine =
      s->cradle->MakePlan(*s->doc.CachedBodiesAfter(1), nullptr);
  CHECK_THAT(fine.refusal, ContainsSubstring("coarser than the clearance"));
}

TEST_CASE("Cradle: a mesh Boolean puts a B-Rep screw hole into it, and an "
          "open scan is refused as an operand but cradles",
          "[mesh][cradle]") {
  auto s = MakeScene(true, 0.5, 0.5, false);
  REQUIRE(s->cradle->GetError().empty());
  const double before = BodyOf(s->doc, s->blockId)->mesh->Volume();
  s->doc.AddFeature(std::make_unique<CylinderFeature>(
      "Screw hole", glm::dvec3(25, 25, -35), glm::dvec3(0, 0, 1), 2.0, 50.0));
  const uint64_t holeId = s->doc.GetFeature(3)->GetId();
  s->doc.AddFeature(std::make_unique<BooleanFeature>(
      "With hole", s->blockId, holeId, BooleanOp::Subtract));
  const auto* boo = dynamic_cast<const BooleanFeature*>(s->doc.GetFeature(4));
  REQUIRE(boo->GetError().empty());
  const Body* out = BodyOf(s->doc, boo->GetId());
  REQUIRE(out != nullptr);
  REQUIRE(out->IsMesh());
  CHECK(out->mesh->Closed());
  // Less a cylinder of radius 2 through the 38 mm block, to the tessellation.
  const double hole = kPi * 4.0 * 38.0;
  CHECK(std::abs((before - out->mesh->Volume()) - hole) < 0.01 * hole);
  CHECK(boo->ReportJson().at("tool_deflection_mm").get<double>() > 0.0);
  CHECK(out->meshSince == "Cradle 'Holder'");

  // The cut figure, open as a table-top scan arrives, imported as a mesh.
  const auto fig = fixtures::CutFigurine(4000);
  const auto stl = fixtures::WriteStl(fig.mesh, "cradle-open-figure");
  Document doc;
  auto imp = std::make_unique<ImportFeature>("scan", stl.string());
  imp->keepOrigin = true;
  imp->upAxis = interchange::UpAxis::None;
  imp->representation = mesh::Representation::Mesh;
  doc.AddFeature(std::move(imp));
  const uint64_t scanId = doc.GetFeature(0)->GetId();
  REQUIRE(doc.GetBodies().size() == 1);
  REQUIRE_FALSE(doc.GetBodies()[0].mesh->Closed());
  glm::dvec3 lo, hi;
  doc.GetBodies()[0].mesh->Bounds(lo, hi);
  doc.AddFeature(std::make_unique<BoxFeature>(
      "Blank", glm::dvec3(lo.x - 6, lo.y - 6, lo.z - 2), hi.x - lo.x + 12,
      hi.y - lo.y + 12, 40.0 - (lo.z - 2)));
  const uint64_t blankId = doc.GetFeature(1)->GetId();
  doc.AddFeature(std::make_unique<BooleanFeature>("Direct", blankId, scanId,
                                                  BooleanOp::Subtract));
  CHECK_THAT(doc.GetFeature(2)->GetError(), ContainsSubstring("open"));
  CHECK_THAT(doc.GetFeature(2)->GetError(), ContainsSubstring("Cradle"));
  doc.RemoveFeature(2);
  auto cr = std::make_unique<CradleFeature>("Holder", blankId, scanId);
  cr->hasClearance = true;
  cr->clearanceMm = 0.5;
  cr->hasResolution = true;
  cr->resolutionMm = 0.5;
  doc.AddFeature(std::move(cr));
  CHECK(doc.GetFeature(2)->GetError().empty());
  CHECK(BodyOf(doc, blankId)->mesh->Closed());
  std::filesystem::remove(stl);
}

// The cost check (docs/ui/testing.md#tests-that-time-things): the CPU time
// of a Cradle of a 300k-triangle scan at its default resolution, against
// the cost law's own estimate (the field's share counted on every thread it
// ran on). Shown to fail against a 5 µs spin planted in each sample
// mesh::Pocket hands the level set.
TEST_CASE("Cradle: a scan at the default resolution builds inside its "
          "estimate",
          "[mesh][cradle][slow]") {
  const auto fig = fixtures::Figurine(300000);
  const auto stl = fixtures::WriteStl(fig.mesh, "cradle-cost-figure");
  Document doc;
  auto imp = std::make_unique<ImportFeature>("scan", stl.string());
  imp->keepOrigin = true;
  imp->upAxis = interchange::UpAxis::None;
  imp->representation = mesh::Representation::Mesh;
  doc.AddFeature(std::move(imp));
  const uint64_t scanId = doc.GetFeature(0)->GetId();
  glm::dvec3 lo, hi;
  doc.GetBodies()[0].mesh->Bounds(lo, hi);
  // A stand round the figure's feet, a quarter of its height.
  doc.AddFeature(std::make_unique<BoxFeature>(
      "Blank", glm::dvec3(lo.x - 6, lo.y - 6, lo.z - 2), hi.x - lo.x + 12,
      hi.y - lo.y + 12, 0.25 * (hi.z - lo.z)));
  const uint64_t blankId = doc.GetFeature(1)->GetId();
  auto cr = std::make_unique<CradleFeature>("Holder", blankId, scanId);
  cr->hasClearance = true;
  cr->clearanceMm = 0.5; // the default resolution is half of it
  const std::clock_t t0 = std::clock();
  doc.AddFeature(std::move(cr));
  const double cpuS = double(std::clock() - t0) / CLOCKS_PER_SEC;
  const auto* made = dynamic_cast<const CradleFeature*>(doc.GetFeature(2));
  REQUIRE(made->GetError().empty());
  const nlohmann::json rep = made->ReportJson();
  const double estimate = rep.at("estimate_s").get<double>();
  std::printf("cradle of a 300k figure: %.2f s CPU against an estimate of "
              "%.2f s, %s\n",
              cpuS, estimate, rep.dump().c_str());
  CHECK(rep.at("resolution_mm").get<double>() == 0.25);
  // The estimate is wall time, with the field shared by the workers; CPU
  // time counts the field on every thread it ran on, measured at up to 4 µs
  // a node across 22 (hyperthreads included), so it is bounded on its own.
  const double cells = rep.at("cells").get<double>();
  CHECK(cpuS < 6.0e-6 * cells + 2.0 * estimate);
  std::filesystem::remove(stl);
}

TEST_CASE("Cradle: the whole feature at the default resolution, by size",
          "[.][mesh-cost-cradle-feature]") {
  // A stand to the figure's middle, cradled at PLA's gap (0.25 a side, so a
  // 0.125 mm cell asked) and coarsened to the budget: what a scan costs.
  const auto now = [] {
    return std::chrono::duration<double>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
  };
  for (size_t n : {300000, 2000000}) {
    const auto fig = fixtures::Figurine(n);
    const auto stl = fixtures::WriteStl(fig.mesh, "cradle-feature-cost");
    Document doc;
    auto imp = std::make_unique<ImportFeature>("scan", stl.string());
    imp->keepOrigin = true;
    imp->upAxis = interchange::UpAxis::None;
    imp->representation = mesh::Representation::Mesh;
    doc.AddFeature(std::move(imp));
    const uint64_t scanId = doc.GetFeature(0)->GetId();
    glm::dvec3 lo, hi;
    doc.GetBodies()[0].mesh->Bounds(lo, hi);
    doc.AddFeature(std::make_unique<BoxFeature>(
        "Blank", glm::dvec3(lo.x - 6, lo.y - 6, lo.z - 2), hi.x - lo.x + 12,
        hi.y - lo.y + 12, 0.5 * (hi.z - lo.z)));
    const uint64_t blankId = doc.GetFeature(1)->GetId();
    doc.SetPrintProcess(printfit::Process{"fdm", "pla", std::nullopt});
    const double w = now();
    const std::clock_t c = std::clock();
    doc.AddFeature(std::make_unique<CradleFeature>("Holder", blankId, scanId));
    const auto* made = dynamic_cast<const CradleFeature*>(doc.GetFeature(2));
    std::printf("COST cradle %zu | %.2f s wall %.2f s cpu | %s %s\n", n,
                now() - w, double(std::clock() - c) / CLOCKS_PER_SEC,
                made->ReportJson().dump().c_str(), made->GetError().c_str());
    std::fflush(stdout);
    std::filesystem::remove(stl);
  }
}
