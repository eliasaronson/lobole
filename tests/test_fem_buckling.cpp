// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

// Linear buckling (docs/simulation/README.md#buckling), solved in-house.
// Each case checks the outcome against a closed form: Euler's column, a
// wide plate's stiffness, a thin cylinder's classical load and its SP-8007
// knockdown, a stocky strut where yield and buckling meet, and a pull that
// buckles nothing. The CalculiX cross-check on the same mesh lives in
// test_fem_calculix.cpp ([buckling][calculix]).

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "core/Document.h"
#include "core/Feature.h"
#include "core/Measure.h"
#include "core/features/BooleanFeature.h"
#include "core/fem/FemSolver.h"
#include "core/fem/FemStudy.h"
#include "core/io/Serialization.h"

#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#if !defined(_WIN32)
#include <sys/resource.h>
#endif

using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::WithinRel;

#ifdef HAVE_NETGEN

namespace {

constexpr double kPi = 3.14159265358979323846;
// The 217k-DOF column's buckling run (mesh, base state, K_G, eigensolve),
// measured 2026-10-29: 177 s of CPU on a P-core, 602 s pinned to an E-core
// (`taskset -c 20`), the slowest core this machine has. Bounded at twice the
// E-core's (docs/ui/testing.md#tests-that-time-things).
constexpr double kBucklingCostBoundSeconds = 1200.0;

ElementRef FaceWhere(const Body& body, const std::string& kind,
                     const std::function<bool(const glm::dvec3&)>& at) {
  for (const auto& [index, name] : body.elementMap.names) {
    const auto info = Measure::DescribeFaceAt(body, index);
    if (info && info->kind == kind && at(info->centroid))
      return {body.createdBy, name};
  }
  FAIL("no such face");
  return {};
}

std::function<bool(const glm::dvec3&)> AtZ(double z) {
  return [z](const glm::dvec3& c) { return std::abs(c.z - z) < 1e-6; };
}

fem::Load MakeLoad(Document& doc, fem::LoadType type, const ElementRef& face,
                   double magnitude = 0.0,
                   glm::dvec3 direction = glm::dvec3(0, 0, -1)) {
  fem::Load l;
  l.id = doc.AllocateId();
  l.type = type;
  l.face = face;
  l.magnitude = magnitude;
  l.direction = direction;
  return l;
}

// Solves `s` with the base state's convergence check off, unless asked.
fem::StudyRuntime Run(Document& doc, fem::Study& s,
                      fem::ConvergeMode mode = fem::ConvergeMode::Never) {
  fem::ConvergeOptions converge;
  converge.mode = mode;
  fem::StudyRuntime rt;
  std::string err;
  const bool ok = fem::RunStudy(doc, s, rt, converge, &err);
  INFO(err);
  REQUIRE(ok);
  REQUIRE(rt.bodies.size() == 1);
  return rt;
}

// A steel column of a w x d section, `length` tall, clamped at its base and
// pushed down on its top with `force` N.
fem::Study& Column(Document& doc, double w, double d, double length,
                   double force, double meshSize) {
  auto box =
      std::make_unique<BoxFeature>("Column", glm::dvec3(0), w, d, length);
  box->SetMaterial("steel");
  doc.AddFeature(std::move(box));
  const Body& body = doc.GetBodies()[0];
  fem::Study& s = doc.AddStudy("Column");
  s.type = fem::AnalysisType::Buckling;
  s.numModes = fem::kBucklingFactorsDefault;
  s.meshSize = meshSize;
  s.loads.push_back(
      MakeLoad(doc, fem::LoadType::Fixed, FaceWhere(body, "plane", AtZ(0.0))));
  s.loads.push_back(MakeLoad(doc, fem::LoadType::Force,
                             FaceWhere(body, "plane", AtZ(length)), force));
  return s;
}

} // namespace

TEST_CASE("a fixed-free column buckles at Euler's load, a 10 x 20 one in its "
          "weak and strong axes four apart",
          "[fem][buckling]") {
  constexpr double E = 210000.0, L = 200.0, P = 1000.0; // library steel
  {
    Document doc;
    fem::Study& s = Column(doc, 10.0, 10.0, L, P, 2.5);
    const fem::StudyRuntime rt = Run(doc, s);
    const fem::StudyBodyResult& b = rt.bodies[0];
    REQUIRE(b.buckling.solved);
    REQUIRE(b.buckling.buckles);
    const double I = 10.0 * 1000.0 / 12.0;
    const double euler = kPi * kPi * E * I / (4.0 * L * L) / P;
    std::printf("buckling: 10x10x200 column %.6g against Euler %.6g (%.3f %%), "
                "%zu nodes\n",
                b.buckling.factor, euler,
                100.0 * (b.buckling.factor / euler - 1.0),
                b.mesh.stats.nodeCount);
    CHECK_THAT(b.buckling.factor, WithinRel(euler, 0.005));
    // A square section has two equal first modes.
    REQUIRE(b.results.modes.size() >= 2);
    CHECK_THAT(b.results.modes[1].factor,
               WithinRel(b.results.modes[0].factor, 1e-3));
    // A member, flat-faced: no knockdown; nowhere near yield.
    CHECK(b.buckling.knockdown == 1.0);
    CHECK_FALSE(b.buckling.atLoadEdge);
    CHECK(b.buckling.yieldFactor > 0.0);
    CHECK(b.buckling.governingFactor < b.buckling.factor);
  }
  {
    Document doc;
    fem::Study& s = Column(doc, 10.0, 20.0, L, P, 2.5);
    const fem::StudyRuntime rt = Run(doc, s);
    const auto& modes = rt.bodies[0].results.modes;
    REQUIRE(modes.size() >= 2);
    const double ratio = modes[1].factor / modes[0].factor;
    std::printf("buckling: 10x20x200 column factors %.6g, %.6g (ratio %.4f)\n",
                modes[0].factor, modes[1].factor, ratio);
    CHECK_THAT(ratio, WithinRel(4.0, 0.01));
  }
}

TEST_CASE("a buckling result is saved and read back as it was",
          "[fem][buckling]") {
  Document doc;
  fem::Study& s = Column(doc, 10.0, 10.0, 100.0, 1000.0, 4.0);
  fem::StudyRuntime rt = Run(doc, s);
  fem::KeepWhatIsRead(rt);
  nlohmann::json j;
  REQUIRE(DocumentIO::StudyResultToJson(rt, j));
  CHECK(j.at("kind") == "buckling");
  const fem::StudyRuntime back = DocumentIO::StudyResultFromJson(j);
  REQUIRE(back.bodies.size() == 1);
  const auto& a = rt.bodies[0];
  const auto& b = back.bodies[0];
  CHECK(b.buckling.solved);
  CHECK(b.buckling.buckles == a.buckling.buckles);
  CHECK(b.buckling.factor == a.buckling.factor);
  CHECK(b.buckling.governingFactor == a.buckling.governingFactor);
  CHECK(b.buckling.knockdown == a.buckling.knockdown);
  CHECK(b.buckling.shapeClass == a.buckling.shapeClass);
  CHECK(b.buckling.yieldFactor == a.buckling.yieldFactor);
  REQUIRE(b.results.modes.size() == a.results.modes.size());
  for (size_t m = 0; m < a.results.modes.size(); ++m) {
    CHECK(b.results.modes[m].factor == a.results.modes[m].factor);
    CHECK(b.results.modes[m].displacement == a.results.modes[m].displacement);
  }
  // A summary missing from a buckling result is refused, not defaulted.
  j["bodies"][0].erase("buckling");
  CHECK_THROWS_WITH(DocumentIO::StudyResultFromJson(j),
                    ContainsSubstring("no buckling summary"));
}

TEST_CASE("a thin cylinder buckles at the classical load, and is knocked "
          "down by SP-8007 at its R/t",
          "[fem][buckling][slow]") {
  // A steel tube of mean radius 25 and wall 1 (R/t 25), 40 long, open and
  // clamped at its base and closed by a 5 mm cap it is pushed down on: the
  // cap holds the loaded edge round, as the classical case's support does.
  constexpr double R = 25.0, t = 1.0, L = 40.0, P = 1000.0;
  constexpr double E = 210000.0, nu = 0.3; // library steel
  Document doc;
  auto outer = std::make_unique<CylinderFeature>(
      "Tube", glm::dvec3(0, 0, 0), glm::dvec3(0, 0, 1), R + t / 2, L + 5.0);
  outer->SetMaterial("steel");
  doc.AddFeature(std::move(outer));
  const uint64_t solid = doc.GetFeature(0)->GetId();
  doc.AddFeature(std::make_unique<CylinderFeature>(
      "Bore", glm::dvec3(0, 0, -1.0), glm::dvec3(0, 0, 1), R - t / 2, L + 1.0));
  const uint64_t tool = doc.GetFeature(doc.FeatureCount() - 1)->GetId();
  doc.AddFeature(std::make_unique<BooleanFeature>("Hollow", solid, tool,
                                                  BooleanOp::Subtract));
  REQUIRE(doc.GetBodies().size() == 1);
  const Body& body = doc.GetBodies()[0];
  fem::Study& s = doc.AddStudy("Tube");
  s.type = fem::AnalysisType::Buckling;
  s.numModes = fem::kBucklingFactorsDefault;
  s.meshSize = 2.0;
  s.loads.push_back(
      MakeLoad(doc, fem::LoadType::Fixed, FaceWhere(body, "plane", AtZ(0.0))));
  s.loads.push_back(MakeLoad(doc, fem::LoadType::Force,
                             FaceWhere(body, "plane", AtZ(L + 5.0)), P));
  const fem::StudyRuntime rt = Run(doc, s);
  const auto& k = rt.bodies[0].buckling;
  REQUIRE(k.buckles);
  const double classical =
      E * t / (R * std::sqrt(3.0 * (1 - nu * nu))) * (2.0 * kPi * R * t) / P;
  std::printf("buckling: thin cylinder R/t %.0f: factor %.6g against the "
              "classical %.6g (%.2f %%); class %s, radius %.4g, wall %.4g, "
              "knockdown %.4f, %d layers, %zu nodes\n",
              R / t, k.factor, classical, 100.0 * (k.factor / classical - 1.0),
              k.shapeClass.c_str(), k.radiusMm, k.wallMm, k.knockdown,
              k.wallLayers, rt.bodies[0].mesh.stats.nodeCount);
  CHECK_THAT(k.factor, WithinRel(classical, 0.05));
  CHECK(k.shapeClass == "cylinder");
  CHECK_THAT(k.wallMm, WithinRel(t, 0.05));
  CHECK(std::abs(k.radiusMm - R) <= t / 2 + 1e-6);
  const double phi = std::sqrt(k.radiusMm / k.wallMm) / 16.0;
  CHECK_THAT(k.knockdown,
             WithinRel(1.0 - 0.901 * (1.0 - std::exp(-phi)), 1e-12));
  CHECK(k.knockdown < 0.8);
  CHECK_THAT(
      1.0 / k.governingFactor,
      WithinRel(1.0 / (k.knockdown * k.factor) +
                    1.0 / std::min(k.yieldFactor > 0 ? k.yieldFactor : 1e300,
                                   k.interlayerFactor > 0 ? k.interlayerFactor
                                                          : 1e300),
                1e-9));
}

TEST_CASE("a wide cantilever plate buckles at its plate stiffness",
          "[fem][buckling]") {
  // A steel plate 60 wide, 1 thick and 20 long, clamped on one long edge
  // and pushed along its length on the other: a wide column whose bending
  // stiffness is the plate's, D = E t^3 / 12 (1 - nu^2), per width.
  constexpr double b = 60.0, t = 1.0, L = 20.0, P = 100.0;
  Document doc;
  auto box = std::make_unique<BoxFeature>("Plate", glm::dvec3(0), b, t, L);
  box->SetMaterial("steel");
  doc.AddFeature(std::move(box));
  const Body& body = doc.GetBodies()[0];
  fem::Study& s = doc.AddStudy("Plate");
  s.type = fem::AnalysisType::Buckling;
  s.numModes = fem::kBucklingFactorsDefault;
  s.meshSize = 2.0;
  s.loads.push_back(
      MakeLoad(doc, fem::LoadType::Fixed, FaceWhere(body, "plane", AtZ(0.0))));
  s.loads.push_back(
      MakeLoad(doc, fem::LoadType::Force, FaceWhere(body, "plane", AtZ(L)), P));
  const fem::StudyRuntime rt = Run(doc, s);
  const auto& k = rt.bodies[0].buckling;
  REQUIRE(k.buckles);
  const double D = 210000.0 * t * t * t / (12.0 * (1.0 - 0.09));
  const double plate = kPi * kPi * D * b / (4.0 * L * L) / P;
  std::printf("buckling: wide plate %.6g against the plate's %.6g (%.2f %%), "
              "%d element(s) through, %zu nodes\n",
              k.factor, plate, 100.0 * (k.factor / plate - 1.0), k.wallLayers,
              rt.bodies[0].mesh.stats.nodeCount);
  CHECK_THAT(k.factor, WithinRel(plate, 0.02));
  // The wall it bends is the plate's, with one element through it at this
  // size: the warning's case.
  CHECK_THAT(k.wallMm, WithinRel(t, 1e-6));
  CHECK(k.wallLayers == 1);
  CHECK(k.shapeClass == "flat");
}

TEST_CASE("a pull buckles nothing, and says so", "[fem][buckling]") {
  Document doc;
  fem::Study& s = Column(doc, 10.0, 10.0, 100.0, -1000.0, 4.0);
  const fem::StudyRuntime rt = Run(doc, s);
  const fem::StudyBodyResult& b = rt.bodies[0];
  REQUIRE(b.buckling.solved);
  CHECK_FALSE(b.buckling.buckles);
  CHECK_THAT(b.buckling.none,
             ContainsSubstring("nothing in this load buckles"));
  // What governs is then yield alone.
  CHECK(b.buckling.governingFactor == b.buckling.yieldFactor);
}

TEST_CASE("a stocky strut fails below both its buckling and its yield "
          "factor, by Rankine-Gordon",
          "[fem][buckling]") {
  // Euler's load of a 10 x 10 steel post 131.4 mm tall, fixed-free, is its
  // squash load at 250 MPa (pi^2 E I / (4 L^2) = 25 kN at E 210 GPa): the
  // two meet.
  Document doc;
  fem::Study& s = Column(doc, 10.0, 10.0, 131.4, 10000.0, 2.5);
  const fem::StudyRuntime rt = Run(doc, s);
  const auto& k = rt.bodies[0].buckling;
  REQUIRE(k.buckles);
  REQUIRE(k.yieldFactor > 0.0);
  std::printf("buckling: stocky strut buckling %.4g, yield %.4g, governing "
              "%.4g\n",
              k.factor, k.yieldFactor, k.governingFactor);
  CHECK(k.factor < 2.0 * k.yieldFactor);
  CHECK(k.yieldFactor < 2.0 * k.factor);
  CHECK(k.governingFactor < k.factor);
  CHECK(k.governingFactor < k.yieldFactor);
  CHECK_THAT(
      1.0 / k.governingFactor,
      WithinRel(1.0 / (k.knockdown * k.factor) + 1.0 / k.yieldFactor, 1e-12));
}

TEST_CASE("a buckling study refuses load cases", "[fem][buckling]") {
  Document doc;
  fem::Study& s = Column(doc, 10.0, 10.0, 100.0, 1000.0, 4.0);
  s.loadCases.push_back({"a", 1.0, ""});
  s.loads[1].loadCase = "a";
  fem::StudyRuntime rt;
  std::string err;
  CHECK_FALSE(fem::RunStudy(doc, s, rt, &err));
  CHECK_THAT(err, ContainsSubstring("takes no load cases"));
}

TEST_CASE("a buckling run that will not fit in memory is refused, naming "
          "both numbers",
          "[fem][buckling]") {
  Document doc;
  fem::Study& s = Column(doc, 10.0, 10.0, 100.0, 1000.0, 4.0);
  fem::StudyPlan plan;
  std::string err;
  REQUIRE(fem::PlanStudy(doc, s, plan, &err));
  fem::StudyJobInput in;
  REQUIRE(fem::MeshPlannedStudy(plan, in, &err));
  fem::SolveOptions options;
  options.availableBytes = 1000;
  fem::StudyRuntime rt;
  CHECK_FALSE(fem::SolvePreparedStudy(in, rt, &err, options));
  CHECK_THAT(err, ContainsSubstring("degrees of freedom would need about"));
  CHECK_THAT(err, ContainsSubstring("GB is free"));
  CHECK(rt.bodies.empty());
}

#endif // HAVE_NETGEN

#ifdef HAVE_NETGEN
TEST_CASE("measure: a 1 mm plate's factor at one and two elements through",
          "[.][buckling-measure]") {
  // LOBOLE_MEASURE="b L size refine; ..." picks the configurations.
  std::vector<std::array<double, 4>> configs;
  if (const char* env = std::getenv("LOBOLE_MEASURE")) {
    std::string all = env;
    size_t at = 0;
    while (at < all.size()) {
      const size_t end = all.find(';', at);
      std::array<double, 4> c{};
      std::sscanf(all.substr(at, end - at).c_str(), "%lf %lf %lf %lf", &c[0],
                  &c[1], &c[2], &c[3]);
      configs.push_back(c);
      at = end == std::string::npos ? all.size() : end + 1;
    }
  } else {
    configs = {{60.0, 20.0, 2.0, 0.0}, {60.0, 20.0, 2.0, 0.5}};
  }
  for (const auto& [b, L, size, refine] : configs) {
    {
      Document doc;
      auto box =
          std::make_unique<BoxFeature>("Plate", glm::dvec3(0), b, 1.0, L);
      box->SetMaterial("steel");
      doc.AddFeature(std::move(box));
      const Body& body = doc.GetBodies()[0];
      fem::Study& s = doc.AddStudy("Plate");
      s.type = fem::AnalysisType::Buckling;
      s.numModes = 3;
      s.meshSize = size;
      s.loads.push_back(MakeLoad(doc, fem::LoadType::Fixed,
                                 FaceWhere(body, "plane", AtZ(0.0))));
      s.loads.push_back(MakeLoad(doc, fem::LoadType::Force,
                                 FaceWhere(body, "plane", AtZ(L)), 100.0));
      if (refine > 0)
        for (double y : {0.0, 1.0}) {
          fem::Refinement r;
          r.face = FaceWhere(body, "plane", [y](const glm::dvec3& c) {
            return std::abs(c.y - y) < 1e-6;
          });
          r.size = refine;
          s.refinements.push_back(r);
        }
      const auto t0 = std::chrono::steady_clock::now();
      const fem::StudyRuntime rt = Run(doc, s);
      const double secs =
          std::chrono::duration<double>(std::chrono::steady_clock::now() - t0)
              .count();
      const auto& k = rt.bodies[0].buckling;
      const double D = 210000.0 / (12.0 * (1 - 0.09));
      const double plate = kPi * kPi * D * b / (4.0 * L * L) / 100.0;
      std::printf("plate b=%g L=%g size=%g refine=%g: factor %.6g, plate "
                  "%.6g (%.2f %%), layers %d, wall %.3g, %zu nodes, %.1f "
                  "s\n",
                  b, L, size, refine, k.factor, plate,
                  100.0 * (k.factor / plate - 1.0), k.wallLayers, k.wallMm,
                  rt.bodies[0].mesh.stats.nodeCount, secs);
      std::fflush(stdout);
    }
  }
}
#endif

#ifdef HAVE_NETGEN
namespace {
// Peak resident memory of this process so far, bytes.
double PeakRssBytes() {
#if !defined(_WIN32)
  rusage ru{};
  getrusage(RUSAGE_SELF, &ru);
  return static_cast<double>(ru.ru_maxrss) * 1024.0;
#else
  return 0.0;
#endif
}
} // namespace

// How the memory estimate's coefficients were measured (hidden: run it by
// name). The steel column at falling sizes, ascending in DOF, so each run's
// peak is the process's new one; CPU seconds beside it.
TEST_CASE("calibrate the buckling memory estimate", "[.][buckling-calibrate]") {
  const double base = PeakRssBytes();
  for (double size : {5.0, 3.5, 2.5, 1.8, 1.5, 1.35}) {
    Document doc;
    fem::Study& s = Column(doc, 10.0, 10.0, 200.0, 1000.0, size);
    const std::clock_t c0 = std::clock();
    const fem::StudyRuntime rt = Run(doc, s);
    const double cpu = double(std::clock() - c0) / CLOCKS_PER_SEC;
    const size_t nodes = rt.bodies[0].mesh.stats.nodeCount;
    const size_t dofs = 3 * nodes;
    std::printf("buckling calibrate: size %.2f, %zu nodes, %zu DOF, peak "
                "%.0f MB over the start, estimate %.0f MB, %.1f s CPU\n",
                size, nodes, dofs, (PeakRssBytes() - base) / 1e6,
                fem::BucklingMemoryEstimate(dofs) / 1e6, cpu);
    std::fflush(stdout);
  }
}

// The cost of a buckling run of about 240k DOF (217k is what Netgen's 1.28
// mm makes of the column): mesh, base state, K_G and the eigensolve.
// Measured once (kBucklingCostBoundSeconds says where), bounded at twice
// that against the slowest core.
TEST_CASE("a buckling run of 240k DOF costs what it was measured to",
          "[fem][buckling][slow]") {
  Document doc;
  fem::Study& s = Column(doc, 10.0, 10.0, 200.0, 1000.0, 1.28);
  const double base = PeakRssBytes();
  const std::clock_t c0 = std::clock();
  const fem::StudyRuntime rt = Run(doc, s);
  const double cpu = double(std::clock() - c0) / CLOCKS_PER_SEC;
  const size_t dofs = 3 * rt.bodies[0].mesh.stats.nodeCount;
  std::printf("buckling cost: peak %.0f MB over the start, estimate %.0f MB\n",
              (PeakRssBytes() - base) / 1e6,
              fem::BucklingMemoryEstimate(dofs) / 1e6);
  std::printf("buckling cost: %zu DOF, %.1f s CPU, factor %.6g\n", dofs, cpu,
              rt.bodies[0].buckling.factor);
  CHECK(dofs > 200000);
  CHECK(dofs < 300000);
  CHECK(cpu < kBucklingCostBoundSeconds);
}
#endif
