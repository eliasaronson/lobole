// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

// Free-free modes and effective mass (docs/simulation/README.md
// #free-free-modes). Checked through the study pipeline (fem::RunStudy on a
// part): a beam with no support finds six rigid modes at zero and its first
// bending mode at the free-free beam's closed form; the rigid modes carry
// the whole mass, the elastic ones none of it; a cantilever's first mode
// carries the share of the mass its closed form gives; and the refusals.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "cad/CADMaterials.h"
#include "core/Document.h"
#include "core/Feature.h"
#include "core/Measure.h"
#include "core/fem/FemSolver.h"
#include "core/fem/FemStudy.h"
#include "study_fixtures.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <numbers>
#include <string>

using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::WithinRel;

namespace {

struct Beam {
  Document doc;
  ElementRef endX0;
};

// A steel beam `L` x `h` x `h` along x.
std::unique_ptr<Beam> MakeBeam(double L, double h) {
  auto b = std::make_unique<Beam>();
  auto box = std::make_unique<BoxFeature>("Beam", glm::dvec3(0), L, h, h);
  box->SetMaterial("steel");
  b->doc.AddFeature(std::move(box));
  const Body& body = b->doc.GetBodies().at(0);
  for (const auto& [index, name] : body.elementMap.names) {
    const auto info = Measure::DescribeFaceAt(body, index);
    if (info && std::abs(info->centroid.x) < 1e-6)
      b->endX0 = ElementRef{body.createdBy, name};
  }
  REQUIRE_FALSE(b->endX0.name.empty());
  return b;
}

CADMaterials::MaterialProperties Steel() {
  return CADMaterials::MaterialLibrary::GetMaterial(
      CADMaterials::MaterialLibrary::MaterialTypeFromString("steel"));
}

} // namespace

#if defined(HAVE_NETGEN) && defined(HAVE_SPECTRA)

TEST_CASE("a free-free beam: six rigid modes at zero, then its closed form",
          "[fem][modal][free]") {
  const double L = 200.0, h = 10.0;
  auto beam = MakeBeam(L, h);
  fem::Study s = NewStudy(beam->doc, "Hanging free");
  s.type = fem::AnalysisType::Modal;
  s.freeFree = true;
  s.numModes = 4;
  s.meshSize = 2.5;
  fem::StudyRuntime rt;
  std::string err;
  const bool ok = fem::RunStudy(beam->doc, s, rt, &err);
  INFO(err);
  REQUIRE(ok);
  REQUIRE(rt.bodies.size() == 1);
  const fem::FemResults& r = rt.bodies[0].results;
  REQUIRE(r.modes.size() == 10); // six rigid, four elastic
  for (int i = 0; i < 6; ++i)
    CHECK(r.modes[i].rigid);
  for (int i = 6; i < 10; ++i)
    CHECK_FALSE(r.modes[i].rigid);

  // The free-free beam's first bending frequency (Blevins, Formulas for
  // Natural Frequency and Mode Shape, table 8-1): (4.7300^2 / 2 pi L^2)
  // sqrt(E I / (rho A)). A square section bends alike both ways, so the
  // first two elastic modes are that one.
  const auto mp = Steel();
  const double E = mp.youngGPa * 1000.0, rho = mp.density * 1e-12; // t/mm^3
  const double I = h * h * h * h / 12.0, A = h * h;
  const double lambda = 4.730040745;
  const double f1 = lambda * lambda / (2.0 * std::numbers::pi * L * L) *
                    std::sqrt(E * I / (rho * A));
  INFO("first elastic " << r.modes[6].frequencyHz << " Hz, second "
                        << r.modes[7].frequencyHz << ", closed form " << f1);
  // Euler-Bernoulli leaves out shear and rotary inertia: at L/h = 20 they
  // take about 1 % off the solid's frequency.
  CHECK_THAT(r.modes[6].frequencyHz, WithinRel(f1, 0.03));
  CHECK_THAT(r.modes[7].frequencyHz, WithinRel(f1, 0.03));
  for (int i = 0; i < 6; ++i) {
    INFO("rigid mode " << i << " at " << r.modes[i].frequencyHz << " Hz");
    CHECK(r.modes[i].frequencyHz < 1e-3 * f1);
  }

  // The rigid modes carry the whole mass along each direction, and the
  // elastic ones, M-orthogonal to a translation, none of it.
  const double mass = rho * A * L * 1000.0; // kg
  CHECK_THAT(r.modalMassKg, WithinRel(mass, 1e-6));
  glm::dvec3 rigid(0.0), elastic(0.0);
  for (const auto& m : r.modes)
    (m.rigid ? rigid : elastic) += m.effectiveMassKg;
  for (int k = 0; k < 3; ++k) {
    CHECK_THAT(rigid[k], WithinRel(mass, 1e-6));
    CHECK(elastic[k] < 1e-6 * mass);
  }
}

TEST_CASE("a cantilever's modes hold its mass, the first its closed share",
          "[fem][modal][free]") {
  const double L = 100.0, h = 10.0;
  auto beam = MakeBeam(L, h);
  fem::Study s = NewStudy(beam->doc, "Cantilever");
  s.type = fem::AnalysisType::Modal;
  s.numModes = 8;
  s.meshSize = 3.0;
  fem::Load fix;
  fix.id = beam->doc.AllocateId();
  fix.type = fem::LoadType::Fixed;
  fix.face = beam->endX0;
  s.loads.push_back(fix);
  fem::StudyRuntime rt;
  std::string err;
  REQUIRE(fem::RunStudy(beam->doc, s, rt, &err));
  const fem::FemResults& r = rt.bodies[0].results;
  REQUIRE(r.modes.size() == 8);
  const double mass = r.modalMassKg;
  REQUIRE(mass > 0.0);
  // A cantilever's first bending mode moves 61.3 % of its mass (the
  // Euler-Bernoulli clamped-free beam's first mode). A square section bends
  // alike along y and z, so its two first modes are one frequency and any
  // pair of shapes across it: together they move that share along each.
  const glm::dvec3 pair =
      r.modes[0].effectiveMassKg + r.modes[1].effectiveMassKg;
  INFO("first bending pair's effective mass " << pair.y / mass << ", "
                                              << pair.z / mass);
  CHECK_THAT(pair.y / mass, WithinRel(0.6131, 0.05));
  CHECK_THAT(pair.z / mass, WithinRel(0.6131, 0.05));
  CHECK(pair.x < 1e-3 * mass);
  // Every mode together holds most of it, never more.
  glm::dvec3 sum(0.0);
  for (const auto& m : r.modes) {
    CHECK_FALSE(m.rigid);
    sum += m.effectiveMassKg;
  }
  for (int k = 1; k < 3; ++k) {
    CHECK(sum[k] < mass * (1.0 + 1e-6));
    CHECK(sum[k] > 0.8 * mass);
  }
}

#endif // HAVE_NETGEN && HAVE_SPECTRA

// Refused, naming what is accepted: free on a study that is not modal, a
// free study with a support (never dropped to make it free), and a modal
// study with no support that is not free says how to make it so.
TEST_CASE("free-free is a modal study's, with nothing holding it",
          "[fem][modal][free]") {
  auto beam = MakeBeam(60.0, 10.0);
  fem::Study s = NewStudy(beam->doc, "S");
  s.freeFree = true;
  CHECK_THAT(fem::FreeFreeRefusal(s), ContainsSubstring("modal study's"));
  s.type = fem::AnalysisType::Modal;
  CHECK(fem::FreeFreeRefusal(s).empty());
  fem::Load fix;
  fix.id = beam->doc.AllocateId();
  fix.type = fem::LoadType::Fixed;
  fix.face = beam->endX0;
  s.loads.push_back(fix);
  CHECK_THAT(fem::FreeFreeRefusal(s), ContainsSubstring("no support"));
  fem::StudyRuntime rt;
  std::string err;
  CHECK_FALSE(fem::RunStudy(beam->doc, s, rt, &err));
  CHECK_THAT(err, ContainsSubstring("free: false"));
  fem::Study held = NewStudy(beam->doc, "Unheld");
  held.type = fem::AnalysisType::Modal;
  fem::Load force;
  force.id = beam->doc.AllocateId();
  force.type = fem::LoadType::Force;
  force.face = beam->endX0;
  force.magnitude = 10.0;
  held.loads.push_back(force);
  CHECK_FALSE(fem::RunStudy(beam->doc, held, rt, &err));
  CHECK_THAT(err, ContainsSubstring("free: true"));
}
