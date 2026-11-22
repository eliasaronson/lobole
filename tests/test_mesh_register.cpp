// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

// Laying a scan onto a body
// (docs/interchange/README.md#laying-a-scan-onto-a-body): a scan generated
// standing on its body, displaced by seeded random rigid motions, is laid back
// by importalign::BestFit -- the function the RPC and the Align tool call --
// and the placement it answers is checked against the motion that was applied:
// composed with it, it must be no motion at all, to within the noise. Every
// fixture is tilted off the axes.

#include "mesh_organic.h"

#include "core/Angles.h"
#include "core/Document.h"
#include "core/Feature.h"
#include "core/ImportAlign.h"
#include "core/ImportPlacement.h"
#include "core/features/DatumAxisFeature.h"
#include "core/features/DatumPlaneFeature.h"
#include "core/mesh/Align.h"
#include "core/mesh/Facing.h"
#include "core/mesh/Register.h"
#include "core/mesh/TriMesh.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <memory>
#include <random>
#include <stdexcept>

using Catch::Matchers::ContainsSubstring;

namespace {

// A rigid motion drawn from `rng`: a uniformly random attitude (Shoemake's
// quaternion) and a move of up to `reach` mm along each axis.
glm::dmat4 RandomMotion(std::mt19937_64& rng, double reach) {
  std::uniform_real_distribution<double> u(0.0, 1.0), m(-reach, reach);
  const double u1 = u(rng), u2 = u(rng) * angles::kTwoPi,
               u3 = u(rng) * angles::kTwoPi;
  const glm::dquat q(
      std::sqrt(u1) * std::cos(u3), std::sqrt(1 - u1) * std::sin(u2),
      std::sqrt(1 - u1) * std::cos(u2), std::sqrt(u1) * std::sin(u3));
  glm::dmat4 x(glm::mat3_cast(glm::normalize(q)));
  x[3] = glm::dvec4(m(rng), m(rng), m(rng), 1.0);
  return x;
}

struct Residual {
  double mm = 0.0, deg = 0.0;
};

// How far `placement` composed with `applied` is from no motion: the turn
// it leaves, and the farthest any vertex of `scan` ends from where it was.
Residual Left(const importplace::Placement& placement,
              const glm::dmat4& applied, const mesh::TriMesh& scan) {
  const glm::dmat4 e = placement.Matrix() * applied;
  Residual r;
  r.deg = angles::ToDegrees(mesh::TurnAngle(glm::dmat3(e)));
  for (const glm::dvec3& v : scan.positions)
    r.mm = std::max(r.mm, glm::length(glm::dvec3(e * glm::dvec4(v, 1.0)) - v));
  return r;
}

std::shared_ptr<const mesh::TriMesh> Share(mesh::TriMesh m) {
  return std::make_shared<const mesh::TriMesh>(std::move(m));
}

importalign::BestFitInput Input(const mesh::TriMesh& scan,
                                const mesh::TriMesh& body) {
  importalign::BestFitInput in;
  in.scan = {Share(scan)};
  in.onto = {Share(body)};
  in.ontoName = "body";
  return in;
}

importalign::Result Fit(const importalign::BestFitInput& in,
                        const importalign::Request& rq = {
                            importalign::Mode::BestFit}) {
  importalign::Result res;
  std::string why;
  REQUIRE(importalign::BestFit(in, {}, rq, {}, res, why));
  REQUIRE(why.empty());
  REQUIRE(!res.cancelled);
  return res;
}

// Lays the part's scan back from `count` seeded motions, each checked
// against what was applied; prints each and the closest other attitude.
void LaysBack(const fixtures::Solid& part, int count, uint64_t seed,
              double mmWithin, double degWithin) {
  std::mt19937_64 rng(seed);
  const auto body = Share(part.body);
  for (int i = 0; i < count; i++) {
    const glm::dmat4 applied = RandomMotion(rng, 0.5 * part.size);
    importalign::BestFitInput in;
    in.scan = {Share(mesh::Transformed(part.scan.mesh, applied))};
    in.onto = {body};
    in.ontoName = "body";
    const importalign::Result res = Fit(in);
    const Residual left = Left(res.placement, applied, part.scan.mesh);
    const nlohmann::json& rep = res.report;
    std::printf("%s #%d: left %.4f mm %.4f deg; mean %.4f p95 %.4f within "
                "%.3f of %.4f mm; winner %s; runner-up %s\n",
                part.scan.name.c_str(), i, left.mm, left.deg,
                rep.at("mean_mm").get<double>(), rep.at("p95_mm").get<double>(),
                rep.at("within").get<double>(),
                rep.at("within_mm").get<double>(),
                rep.at("winner").get<std::string>().c_str(),
                rep.contains("runner_up") ? rep.at("runner_up").dump().c_str()
                                          : "none");
    INFO(part.scan.name << " motion " << i << ": " << rep.dump());
    CHECK(left.mm < mmWithin);
    CHECK(left.deg < degWithin);
    CHECK(res.placement.scale == 1.0);
  }
}

} // namespace

TEST_CASE("best_fit lays a displaced scan of a bracket back onto its body",
          "[align][register][mesh_body]") {
  LaysBack(fixtures::ScannedSolid(fixtures::SolidShape::Bracket), 6, 11, 0.05,
           0.05);
}

TEST_CASE("best_fit lays a near-cube back by its one feature",
          "[align][register][mesh_body]") {
  LaysBack(fixtures::ScannedSolid(fixtures::SolidShape::NearCube), 6, 12, 0.05,
           0.05);
}

TEST_CASE("best_fit lays a scan of half a bracket back onto the whole",
          "[align][register][mesh_body]") {
  LaysBack(fixtures::ScannedSolid(fixtures::SolidShape::Bracket, 0.8, 0.05,
                                  /*half=*/true),
           6, 13, 0.05, 0.05);
}

TEST_CASE("best_fit does not lay a thin plate on its back",
          "[align][register][mesh_body]") {
  const fixtures::Solid plate =
      fixtures::ScannedSolid(fixtures::SolidShape::Plate, 0.5, 0.03);
  LaysBack(plate, 6, 14, 0.05, 0.05);
  // On its back -- turned half about its long axis, the far side where
  // the near side was -- it lies on the body as closely but for its hole
  // and notch: the closest other attitude the fit found, and several times
  // worse than the right way up.
  importalign::BestFitInput in = Input(plate.scan.mesh, plate.body);
  const importalign::Result res = Fit(in);
  REQUIRE(res.report.contains("runner_up"));
  const nlohmann::json& second = res.report.at("runner_up");
  std::printf("plate: runner-up %s against %.4f mm\n", second.dump().c_str(),
              res.report.at("score_mm").get<double>());
  CHECK(second.at("turn_deg").get<double>() > 170.0);
  CHECK(second.at("score_mm").get<double>() >
        3.0 * res.report.at("score_mm").get<double>());
  CHECK_FALSE(res.report.contains("note"));
  // Standing off by most of its thickness, its samples nearer the far side
  // than their own: matched to the side facing their way, they come back.
  const auto body = Share(plate.body);
  const mesh::FacingSurface surface(std::make_shared<const mesh::Bvh>(body));
  const auto samples = mesh::SampleSurface(plate.scan.mesh, 3000, 3);
  const glm::dvec3 n = plate.turn * glm::dvec3(0, 0, 1);
  for (double minCos : {0.3}) {
    mesh::RegisterOptions o;
    o.capMm = 5.0;
    o.minFacingCos = minCos;
    o.starts = {mesh::Rigid{glm::dmat3(1.0), 1.3 * n}};
    const mesh::RegisterResult r = mesh::Register(samples, surface, o);
    const double off = glm::length(r.Best().pose.translation);
    std::printf("plate 1.3 mm off its normal, facing cos %.1f: ends %.4f mm "
                "off, %.4f deg, %zu passes settled %d score %.4f\n",
                minCos, off,
                angles::ToDegrees(mesh::TurnAngle(r.Best().pose.rotation)),
                r.Best().passes, int(r.Best().settled), r.Best().scoreMm);
    if (minCos > 0.0)
      CHECK(off < 0.01);
  }
}

TEST_CASE("best_fit on a part its own turns lay on itself says so",
          "[align][register][mesh_body]") {
  // A rounded block: half a turn about each of its axes lays it on itself,
  // so four attitudes fit it alike and the scan is laid in one of them.
  const fixtures::Block scan = fixtures::ScannedBlock(0.5, 0.05);
  const fixtures::Block body = fixtures::ScannedBlock(0.25, 0.0);
  std::mt19937_64 rng(51);
  const glm::dmat4 applied = RandomMotion(rng, 15.0);
  importalign::BestFitInput in;
  in.scan = {Share(mesh::Transformed(scan.organic.mesh, applied))};
  in.onto = {Share(body.organic.mesh)};
  in.ontoName = "block";
  const importalign::Result res = Fit(in);
  const nlohmann::json& rep = res.report;
  std::printf("block: %s\n", rep.dump().c_str());
  REQUIRE(rep.contains("runner_up"));
  CHECK(rep.at("runner_up").at("turn_deg").get<double>() > 170.0);
  REQUIRE(rep.contains("note"));
  CHECK_THAT(rep.at("note").get<std::string>(),
             ContainsSubstring("barely tells them apart"));
  // Whichever it chose, it lies on the block within the noise.
  CHECK(rep.at("mean_mm").get<double>() < 0.05);
  const glm::dmat4 e = res.placement.Matrix() * applied;
  bool onASymmetry = false;
  for (int axis = -1; axis < 3; axis++) {
    glm::dmat3 flip(1.0);
    if (axis >= 0)
      flip = glm::mat3_cast(glm::angleAxis(
          angles::kPi,
          body.turn * glm::dvec3(axis == 0, axis == 1, axis == 2)));
    const glm::dmat3 r = glm::transpose(flip) * glm::dmat3(e);
    onASymmetry |= angles::ToDegrees(mesh::TurnAngle(r)) < 0.05;
  }
  CHECK(onASymmetry);
}

TEST_CASE("best_fit never scales: a scan at the wrong size stays that size",
          "[align][register][mesh_body]") {
  const fixtures::Solid part =
      fixtures::ScannedSolid(fixtures::SolidShape::Bracket);
  importalign::BestFitInput in = Input(part.scan.mesh, part.body);
  importalign::Result res;
  std::string why;
  importplace::Placement current;
  current.scale = 1.02;
  current.translateMm = {3, -2, 1};
  REQUIRE(importalign::BestFit(in, current, {importalign::Mode::BestFit}, {},
                               res, why));
  CHECK(res.placement.scale == 1.02);
}

TEST_CASE("best_fit from three picked pairs, and the pairs refused in a line",
          "[align][register][mesh_body]") {
  const fixtures::Solid part =
      fixtures::ScannedSolid(fixtures::SolidShape::NearCube);
  std::mt19937_64 rng(21);
  const glm::dmat4 applied = RandomMotion(rng, 0.5 * part.size);
  importalign::BestFitInput in;
  in.scan = {Share(mesh::Transformed(part.scan.mesh, applied))};
  in.onto = {Share(part.body)};
  in.ontoName = "body";
  // Three corners of the top, picked a millimetre or so off on the scan.
  const glm::dvec3 l[3] = {{12, 12, 15.6}, {-12, 12, 15.6}, {12, -12, 15.6}};
  importalign::Request rq{importalign::Mode::BestFit};
  for (int i = 0; i < 3; i++) {
    const glm::dvec3 on = part.ToWorld(l[i]);
    const glm::dvec3 off = on + glm::dvec3(0.7, -0.5, 0.4);
    rq.pairsMm.push_back({glm::dvec3(applied * glm::dvec4(off, 1.0)), on});
  }
  const importalign::Result res = Fit(in, rq);
  CHECK(res.report.at("winner") == "pairs");
  CHECK(res.report.at("starts") == 1);
  const Residual left = Left(res.placement, applied, part.scan.mesh);
  std::printf("near cube from pairs: left %.4f mm %.4f deg\n", left.mm,
              left.deg);
  CHECK(left.mm < 0.05);

  importalign::Request line = rq;
  line.pairsMm[2][0] = 0.5 * (line.pairsMm[0][0] + line.pairsMm[1][0]);
  importalign::Result none;
  std::string why;
  CHECK_FALSE(importalign::BestFit(in, {}, line, {}, none, why));
  CHECK_THAT(why, ContainsSubstring("on the import"));
  CHECK_THAT(why, ContainsSubstring("in a line"));
  importalign::Request two = rq;
  two.pairsMm.pop_back();
  CHECK_THROWS_WITH(importalign::BestFit(in, {}, two, {}, none, why),
                    ContainsSubstring("three pairs"));
}

TEST_CASE("best_fit limited to a region lays the patch",
          "[align][register][mesh_body]") {
  const fixtures::Solid part =
      fixtures::ScannedSolid(fixtures::SolidShape::Bracket);
  std::mt19937_64 rng(31);
  // A small motion: a patch has no shape to find the part from far off.
  glm::dmat4 applied(glm::mat3_cast(glm::angleAxis(
      angles::ToRadians(4.0), glm::normalize(glm::dvec3(1, 2, 3)))));
  applied[3] = glm::dvec4(1.0, -0.8, 0.6, 1.0);
  importalign::BestFitInput in;
  in.scan = {Share(mesh::Transformed(part.scan.mesh, applied))};
  in.onto = {Share(part.body)};
  in.ontoName = "body";
  // Where the upright meets the base at one side, on the body: the base's
  // top, the upright's face and the side face round it fix every motion
  // (a patch clear of the side would leave the part free to slide along
  // the corner).
  importalign::Request rq{importalign::Mode::BestFit};
  rq.region = importalign::Request::Region{
      false, part.ToWorld({-25.0, 12.0, 5.0}), 10.0};
  const importalign::Result res = Fit(in, rq);
  const Residual left = Left(res.placement, applied, part.scan.mesh);
  std::printf("bracket by its corner: left %.4f mm %.4f deg\n", left.mm,
              left.deg);
  CHECK(left.mm < 0.05);
  CHECK(left.deg < 0.05);
  importalign::Request empty = rq;
  empty.region->pointMm = part.ToWorld({0, 0, 200});
  importalign::Result none;
  std::string why;
  CHECK_FALSE(importalign::BestFit(in, {}, empty, {}, none, why));
  CHECK_THAT(why, ContainsSubstring("no surface within"));
  empty.region->onScan = true;
  CHECK_FALSE(importalign::BestFit(in, {}, empty, {}, none, why));
  CHECK_THAT(why, ContainsSubstring("the import has no surface within"));
}

TEST_CASE("best_fit stops when its cancel is asked", "[align][register]") {
  const fixtures::Solid part =
      fixtures::ScannedSolid(fixtures::SolidShape::NearCube);
  const importalign::BestFitInput in = Input(part.scan.mesh, part.body);
  std::atomic<bool> cancel{false};
  int calls = 0;
  importalign::Progress progress;
  progress.cancel = &cancel;
  progress.fraction = [&](double) {
    if (++calls == 2)
      cancel = true;
  };
  importalign::Result res;
  std::string why;
  REQUIRE(importalign::BestFit(in, {}, {importalign::Mode::BestFit}, progress,
                               res, why));
  CHECK(res.cancelled);
  CHECK(calls == 2); // stopped at the next pass
}

namespace {
double Cpu() {
  timespec ts{};
  clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
  return double(ts.tv_sec) + 1e-9 * double(ts.tv_nsec);
}
} // namespace

TEST_CASE("best_fit of a 500k-triangle scan costs what it did",
          "[align][register][slow]") {
  // The bracket scanned at a spacing that makes about half a million
  // triangles, laid onto its body.
  const fixtures::Solid part =
      fixtures::ScannedSolid(fixtures::SolidShape::Bracket, 0.33, 0.03, false,
                             /*bodySpacing=*/0.4);
  std::printf("cost: %zu scan triangles, %zu body triangles\n",
              part.scan.mesh.triangles.size(), part.body.triangles.size());
  REQUIRE(part.scan.mesh.triangles.size() > 450000);
  std::mt19937_64 rng(41);
  const glm::dmat4 applied = RandomMotion(rng, 0.5 * part.size);
  importalign::BestFitInput in;
  in.scan = {Share(mesh::Transformed(part.scan.mesh, applied))};
  in.onto = {Share(part.body)};
  in.ontoName = "body";
  // The body's tree built first: it is cached with the body and is not the
  // fit's cost.
  (void)mesh::Bvh::For(in.onto.front());
  const double t0 = Cpu();
  const importalign::Result res = Fit(in);
  const double cpu = Cpu() - t0;
  const Residual left = Left(res.placement, applied, part.scan.mesh);
  std::printf("cost: best fit %.2f CPU s; left %.4f mm %.4f deg\n", cpu,
              left.mm, left.deg);
  CHECK(left.mm < 0.05);
  // Measured at 1.2-1.3 CPU s (2026-11, threads on both kinds of core);
  // the budget leaves room for every thread on the slow ones, which cost
  // 1.8 times as much, and ten times that under the sanitizers.
#if defined(__SANITIZE_THREAD__) || defined(__SANITIZE_ADDRESS__)
  constexpr double kBudget = 40.0;
#else
  constexpr double kBudget = 4.0;
#endif
  CHECK(cpu < kBudget);
}

TEST_CASE("A datum fitted before a re-scan refits after best_fit lays the "
          "re-scan where the scan stood",
          "[align][register][datum][mesh_body]") {
  // The scan, kept as it was read, and the same file as the import the
  // datums are fitted on.
  const fixtures::Post post = fixtures::ScannedPost(40000);
  const std::filesystem::path first =
      fixtures::WriteStl(post.organic.mesh, "register-first");
  Document doc;
  for (const char* name : {"first scan", "scan"}) {
    auto imp = std::make_unique<ImportFeature>(name, first.string());
    imp->upAxis = interchange::UpAxis::None;
    imp->representation = mesh::Representation::Mesh;
    imp->keepOrigin = true;
    doc.AddFeature(std::move(imp));
  }
  const uint64_t firstId = doc.GetFeature(0)->GetId(),
                 scanId = doc.GetFeature(1)->GetId();
  auto axis =
      std::make_unique<DatumAxisFeature>("Bore", DatumAxisKind::FromFit);
  axis->fit = {scanId, post.onCylinder, mesh::Kind::Cylinder, {}};
  doc.AddFeature(std::move(axis));
  auto plane =
      std::make_unique<DatumPlaneFeature>("Top", DatumPlaneKind::FromFit);
  plane->fit = {scanId, post.onTop, mesh::Kind::Plane, {}};
  doc.AddFeature(std::move(plane));
  REQUIRE(doc.Regenerate());
  const auto Axis = [&] {
    return dynamic_cast<const DatumAxisFeature*>(doc.GetFeature(2));
  };
  const auto Plane = [&] {
    return dynamic_cast<const DatumPlaneFeature*>(doc.GetFeature(3));
  };
  REQUIRE(Axis()->GetError().empty());
  const glm::dvec3 axisOrigin = Axis()->Origin();
  const glm::dvec3 topOrigin = Plane()->OutputPlane().GetOrigin();

  // Scanned again -- other triangles, other noise -- and read standing
  // wherever the scanner put it: off its seed, the datum cannot fit.
  std::mt19937_64 rng(61);
  const glm::dmat4 applied = RandomMotion(rng, 40.0);
  const fixtures::Post again = fixtures::ScannedPost(52000, 0.04);
  const std::filesystem::path second = fixtures::WriteStl(
      mesh::Transformed(again.organic.mesh, applied), "register-again");
  auto* scan = dynamic_cast<ImportFeature*>(doc.FindFeatureById(scanId));
  scan->path = second.string();
  doc.Regenerate();
  CHECK_FALSE(Axis()->GetError().empty());

  // Laid onto the first scan, as the RPC lays it.
  std::vector<const Body*> onto;
  for (const Body& b : doc.GetBodies())
    if (b.createdBy == firstId)
      onto.push_back(&b);
  const importalign::BestFitInput in =
      importalign::GatherBestFit(importalign::ImportBodies(doc, *scan), onto);
  importalign::Result res;
  std::string why;
  REQUIRE(importalign::BestFit(in, scan->placement,
                               {importalign::Mode::BestFit}, {}, res, why));
  scan->placement = res.placement;
  REQUIRE(doc.Regenerate());
  REQUIRE(Axis()->GetError().empty());
  REQUIRE(Plane()->GetError().empty());
  // The seeds are world points, and never moved.
  CHECK(Axis()->fit.pointMm == post.onCylinder);
  CHECK(Plane()->fit.pointMm == post.onTop);
  const double axisOff = [&] {
    const glm::dvec3 w = Axis()->Origin() - post.base;
    return glm::length(w - glm::dot(w, post.axis) * post.axis);
  }();
  const double axisDeg = angles::ToDegrees(std::acos(
      std::min(1.0, std::abs(glm::dot(Axis()->Direction(), post.axis)))));
  const double topOff =
      glm::dot(Plane()->OutputPlane().GetOrigin() - topOrigin, post.axis);
  std::printf("re-scan laid back: axis %.4f mm %.4f deg off the true axis "
              "(%.4f mm from where it was), top %.4f mm; %s\n",
              axisOff, axisDeg, glm::length(Axis()->Origin() - axisOrigin),
              topOff, res.report.dump().c_str());
  CHECK(axisOff < 0.05);
  CHECK(axisDeg < 0.1);
  CHECK(std::abs(topOff) < 0.05);
  CHECK(res.placement.scale == 1.0);
  std::filesystem::remove(first);
  std::filesystem::remove(second);
}

TEST_CASE("best_fit lays an exact copy of a coarse CAD mesh exactly",
          "[align][register][mesh_body]") {
  // A 40 x 20 x 10 box, each face a grid of 4 x 4 squares: an export's
  // few, large triangles meeting at sharp edges. A sample's normal blended
  // across those edges leaned past the angle a match allows, and one in a
  // hundred samples was matched to the wrong face.
  mesh::TriMesh box;
  const glm::dvec3 half(20.0, 10.0, 5.0);
  std::vector<glm::dvec3> soup;
  for (int axis = 0; axis < 3; axis++)
    for (int side : {-1, 1}) {
      const int u = (axis + 1) % 3, v = (axis + 2) % 3;
      const auto at = [&](int i, int j) {
        glm::dvec3 p(0.0);
        p[axis] = side * half[axis];
        p[u] = -half[u] + 2.0 * half[u] * i / 4.0;
        p[v] = -half[v] + 2.0 * half[v] * j / 4.0;
        return p;
      };
      for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) {
          const glm::dvec3 a = at(i, j), b = at(i + 1, j), c = at(i + 1, j + 1),
                           d = at(i, j + 1);
          for (const glm::dvec3& p : side > 0 ? std::array{a, b, c, a, c, d}
                                              : std::array{a, c, b, a, d, c})
            soup.push_back(p);
        }
    }
  box = mesh::FromSoup(soup, 1e-9);
  REQUIRE(box.Closed());
  std::mt19937_64 rng(71);
  const glm::dmat4 applied = RandomMotion(rng, 40.0);
  const importalign::Result res =
      Fit(Input(mesh::Transformed(box, applied), box));
  std::printf("coarse box: %s\n", res.report.dump().c_str());
  CHECK(res.report.at("mean_mm").get<double>() < 1e-6);
  CHECK(res.report.at("within").get<double>() == 1.0);
}
