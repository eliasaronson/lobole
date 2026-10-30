// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

// Thermal studies (docs/simulation/README.md#thermal), solved in-house. Each
// case checks the outcome against a closed form: a slab held at two
// temperatures, heated through its volume and through one face; a fin
// against the 1-D fin with a convecting tip; a pipe and a heated rod on a
// Netgen mesh; and the thermal strain a temperature puts into a static
// solve (a clamped bar, a cube held on every face, a free bar bent by a
// gradient). Hand-made meshes where the geometry allows, so they run on every
// CI image. The CalculiX cross-check on the same mesh lives in
// test_fem_calculix.cpp ([thermal][calculix]).

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "cad/CADMaterials.h"
#include "core/Document.h"
#include "core/Feature.h"
#include "core/Measure.h"
#include "core/features/BooleanFeature.h"
#include "core/fem/FemSolver.h"
#include "core/fem/FemStudy.h"
#include "core/fem/FemThermal.h"
#include "core/io/Serialization.h"
#include "topo_testmesh.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <vector>

using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

#ifdef HAVE_FEM_SOLVER

namespace {

constexpr double kPi = 3.14159265358979323846;

// The 100 x 10 x 10 bar the plan measured ccx on: k = 50 W/(m K) at 5 mm.
constexpr double kL = 100.0, kW = 10.0, kK = 50.0;

fem::FemMesh Bar() { return GenerateBoxMesh(20, 2, 2, kL, kW, kW); }

std::vector<fem::Material> Conductor(double k = kK) {
  fem::Material m;
  m.youngMPa = 200000.0;
  m.poisson = 0.3;
  m.conductivityWmK = k;
  m.expansionPerK = 12e-6;
  return {m};
}

fem::HeatSolution Solve(const fem::FemMesh& mesh, const fem::HeatLoads& loads,
                        const std::vector<fem::Material>& mats = Conductor()) {
  fem::HeatSolution out;
  std::string err;
  const bool ok = fem::SolveHeat(mesh, mats, loads, {}, out, &err);
  INFO(err);
  REQUIRE(ok);
  return out;
}

double FlowOf(const fem::HeatSolution& s, uint64_t id) {
  for (const fem::HeatFlow& f : s.flows)
    if (f.id == id)
      return f.watts;
  FAIL("no flow for load " << id);
  return 0.0;
}

} // namespace

TEST_CASE("a slab held at two temperatures conducts k A dT / L, linearly",
          "[fem][thermal]") {
  const fem::FemMesh mesh = Bar();
  fem::HeatLoads loads;
  loads.held.push_back({1, TrisAtPlane(mesh, 0, 0.0, 1e-9), 100.0});
  loads.held.push_back({2, TrisAtPlane(mesh, 0, kL, 1e-9), 20.0});
  const fem::HeatSolution s = Solve(mesh, loads);
  for (size_t n = 0; n < mesh.nodes.size(); ++n)
    CHECK_THAT(s.temperature[n],
               WithinAbs(100.0 - 80.0 * mesh.nodes[n].x / kL, 1e-9));
  // k A dT / L in mW, out of the cold end and into the hot one.
  const double q = kK * kW * kW * 80.0 / kL / 1000.0; // W
  CHECK_THAT(FlowOf(s, 2), WithinRel(q, 1e-9));
  CHECK_THAT(FlowOf(s, 1), WithinRel(-q, 1e-9));
  CHECK_THAT(s.inW, WithinRel(q, 1e-9));
  CHECK(std::abs(s.imbalanceW) <= 1e-9 * q);
  // -k grad T, W/m^2: k dT/L along +x, everywhere.
  for (const glm::dvec3& f : s.heatFlux)
    CHECK_THAT(f.x, WithinRel(kK * 80.0 / kL * 1000.0, 1e-9));
}

TEST_CASE("a slab heated through its volume peaks at q L^2 / 8k",
          "[fem][thermal]") {
  const fem::FemMesh mesh = Bar();
  fem::HeatLoads loads;
  loads.held.push_back({1, TrisAtPlane(mesh, 0, 0.0, 1e-9), 20.0});
  loads.held.push_back({2, TrisAtPlane(mesh, 0, kL, 1e-9), 20.0});
  const double q = 0.01; // mW/mm^3
  const double powerW = q * kL * kW * kW / 1000.0;
  loads.sources.push_back({3, {}, powerW * 1000.0});
  const fem::HeatSolution s = Solve(mesh, loads);
  for (size_t n = 0; n < mesh.nodes.size(); ++n) {
    const double x = mesh.nodes[n].x;
    CHECK_THAT(s.temperature[n],
               WithinAbs(20.0 + q * x * (kL - x) / (2.0 * kK), 1e-9));
  }
  // Each end carries half; the source's power goes in exactly.
  CHECK_THAT(FlowOf(s, 1), WithinRel(powerW / 2, 1e-9));
  CHECK_THAT(FlowOf(s, 2), WithinRel(powerW / 2, 1e-9));
  CHECK_THAT(FlowOf(s, 3), WithinRel(-powerW, 1e-12));
  CHECK(std::abs(s.imbalanceW) <= 1e-9 * powerW);
}

TEST_CASE("a slab heated through one face rises q L / k", "[fem][thermal]") {
  const fem::FemMesh mesh = Bar();
  fem::HeatLoads loads;
  loads.held.push_back({1, TrisAtPlane(mesh, 0, kL, 1e-9), 20.0});
  fem::HeatLoads::Flux f;
  f.id = 2;
  f.tris = TrisAtPlane(mesh, 0, 0.0, 1e-9);
  f.total = false;
  f.fluxMWmm2 = 0.5;
  loads.fluxes.push_back(f);
  const fem::HeatSolution s = Solve(mesh, loads);
  for (size_t n = 0; n < mesh.nodes.size(); ++n)
    CHECK_THAT(s.temperature[n],
               WithinAbs(20.0 + 0.5 * (kL - mesh.nodes[n].x) / kK, 1e-9));
  const double w = 0.5 * kW * kW / 1000.0;
  CHECK_THAT(FlowOf(s, 2), WithinRel(-w, 1e-9));
  CHECK_THAT(FlowOf(s, 1), WithinRel(w, 1e-9));
}

TEST_CASE("a fin cools as the 1-D fin with a convecting tip does",
          "[fem][thermal]") {
  const fem::FemMesh mesh = Bar();
  fem::HeatLoads loads;
  loads.held.push_back({1, TrisAtPlane(mesh, 0, 0.0, 1e-9), 100.0});
  fem::HeatLoads::Film film;
  film.id = 2;
  film.hMWmm2K = 25.0 * fem::kWm2ToMWmm2;
  film.ambientC = 20.0;
  for (int t = 0; t < static_cast<int>(mesh.boundary.size()); ++t)
    if (mesh.boundary[t].faceIndex != 0)
      film.tris.push_back(t);
  loads.films.push_back(film);
  const fem::HeatSolution s = Solve(mesh, loads);
  // Incropera eq. 3.75 and 3.77: the fin with a convecting tip.
  const double h = film.hMWmm2K, P = 4 * kW, A = kW * kW;
  const double m = std::sqrt(h * P / (kK * A)), r = h / (m * kK);
  const auto theory = [&](double x) {
    return 20.0 + 80.0 *
                      (std::cosh(m * (kL - x)) + r * std::sinh(m * (kL - x))) /
                      (std::cosh(m * kL) + r * std::sinh(m * kL));
  };
  const double baseW = std::sqrt(h * P * kK * A) * 80.0 *
                       (std::sinh(m * kL) + r * std::cosh(m * kL)) /
                       (std::cosh(m * kL) + r * std::sinh(m * kL)) / 1000.0;
  double mid = 0, tip = 0;
  int nm = 0, nt = 0;
  for (size_t n = 0; n < mesh.nodes.size(); ++n) {
    if (std::abs(mesh.nodes[n].x - kL / 2) < 1e-9) {
      mid += s.temperature[n];
      ++nm;
    }
    if (std::abs(mesh.nodes[n].x - kL) < 1e-9) {
      tip += s.temperature[n];
      ++nt;
    }
  }
  mid /= nm;
  tip /= nt;
  std::printf("thermal: fin mid %.6f (1-D %.6f), tip %.6f (1-D %.6f), base "
              "%.6f W (1-D %.6f W)\n",
              mid, theory(kL / 2), tip, theory(kL), -FlowOf(s, 1), baseW);
  CHECK_THAT(mid, WithinRel(theory(kL / 2), 0.005));
  CHECK_THAT(tip, WithinRel(theory(kL), 0.005));
  CHECK_THAT(-FlowOf(s, 1), WithinRel(baseW, 0.02));
  CHECK_THAT(FlowOf(s, 2), WithinRel(-FlowOf(s, 1), 1e-6));
}

TEST_CASE("a heat solve refuses what it cannot answer", "[fem][thermal]") {
  const fem::FemMesh mesh = Bar();
  fem::HeatSolution out;
  std::string err;
  fem::HeatLoads flux;
  fem::HeatLoads::Flux f;
  f.id = 1;
  f.tris = TrisAtPlane(mesh, 0, 0.0, 1e-9);
  f.totalMW = 1000.0;
  flux.fluxes.push_back(f);
  CHECK_FALSE(fem::SolveHeat(mesh, Conductor(), flux, {}, out, &err));
  CHECK_THAT(err, ContainsSubstring("nowhere to go"));
  fem::HeatLoads held;
  held.held.push_back({1, TrisAtPlane(mesh, 0, 0.0, 1e-9), 20.0});
  CHECK_FALSE(fem::SolveHeat(mesh, Conductor(0.0), held, {}, out, &err));
  CHECK_THAT(err, ContainsSubstring("conductivity"));
  // Two faces that meet, held at two temperatures.
  held.held.push_back({2, TrisAtPlane(mesh, 1, 0.0, 1e-9), 30.0});
  CHECK_FALSE(fem::SolveHeat(mesh, Conductor(), held, {}, out, &err));
  CHECK_THAT(err, ContainsSubstring("two temperatures"));
}

TEST_CASE("an iterative heat solve answers what the direct one does",
          "[fem][thermal]") {
  const fem::FemMesh mesh = Bar();
  fem::HeatLoads loads;
  loads.held.push_back({1, TrisAtPlane(mesh, 0, 0.0, 1e-9), 20.0});
  loads.sources.push_back({2, {}, 5000.0});
  fem::HeatLoads::Film film{3, TrisAtPlane(mesh, 1, kW, 1e-9), 0.01, 20.0};
  loads.films.push_back(film);
  const fem::HeatSolution direct = Solve(mesh, loads);
  fem::SolveOptions it;
  it.iterativeAboveDofs = 0;
  fem::HeatSolution pcg;
  std::string err;
  REQUIRE(fem::SolveHeat(mesh, Conductor(), loads, it, pcg, &err));
  CHECK(pcg.solver == fem::kIterativeSolverName);
  for (size_t n = 0; n < mesh.nodes.size(); ++n)
    CHECK_THAT(pcg.temperature[n], WithinRel(direct.temperature[n], 1e-6));
}

// ---- The thermal load on a static solve -----------------------------------

namespace {

// Holds direction k at every node of the plane x_axis = value.
void Hold(const fem::FemMesh& mesh, int axis, double value, int k,
          fem::Supports& sup) {
  for (int n : NodesAtPlane(mesh, axis, value, 1e-9)) {
    fem::HeldNode h;
    h.node = n;
    h.held[k] = true;
    sup.held.push_back(h);
  }
}

std::shared_ptr<fem::NodalTemperature> Uniform(const fem::FemMesh& mesh,
                                               double t, double t0) {
  auto f = std::make_shared<fem::NodalTemperature>();
  f->temperature.assign(mesh.nodes.size(), t);
  f->stressFreeC = t0;
  return f;
}

fem::FemResults Static(const fem::FemMesh& mesh, const fem::Supports& sup,
                       std::shared_ptr<fem::NodalTemperature> field) {
  fem::SolveOptions o;
  o.thermal = std::move(field);
  std::string err;
  auto r =
      fem::SolveCases(mesh, Conductor(), sup, {{}}, &err, glm::dvec3(0.0), o);
  INFO(err);
  REQUIRE(r);
  return r->front();
}

} // namespace

TEST_CASE("a bar clamped along its axis and heated carries -E alpha dT",
          "[fem][thermal]") {
  const fem::FemMesh mesh = Bar();
  fem::Supports sup;
  Hold(mesh, 0, 0.0, 0, sup);
  Hold(mesh, 0, kL, 0, sup);
  Hold(mesh, 1, 0.0, 1, sup); // symmetry planes across
  Hold(mesh, 2, 0.0, 2, sup);
  sup.reactions = true;
  const fem::FemResults r = Static(mesh, sup, Uniform(mesh, 120.0, 20.0));
  const double sigma = -200000.0 * 12e-6 * 100.0; // -240 MPa
  for (const fem::Stress& s : r.stress) {
    CHECK_THAT(s[0], WithinRel(sigma, 1e-9));
    CHECK(std::abs(s[1]) < 1e-9 * std::abs(sigma));
    CHECK(std::abs(s[2]) < 1e-9 * std::abs(sigma));
  }
  // What the clamped end carries: sigma A.
  double fx = 0.0;
  for (int n : NodesAtPlane(mesh, 0, kL, 1e-9))
    fx += r.nodeReaction[n].x;
  CHECK_THAT(fx, WithinRel(sigma * kW * kW, 1e-9));
}

TEST_CASE("a cube held on every face and heated carries -E alpha dT / "
          "(1 - 2 nu)",
          "[fem][thermal]") {
  const fem::FemMesh mesh = GenerateBoxMesh(3, 3, 3, 30, 30, 30);
  fem::Supports sup;
  for (const fem::FemMesh::BoundaryTri& t : mesh.boundary)
    for (int n : t.nodes)
      sup.fixedNodes.push_back(n);
  const fem::FemResults r = Static(mesh, sup, Uniform(mesh, 70.0, 20.0));
  const double sigma = -200000.0 * 12e-6 * 50.0 / (1.0 - 2.0 * 0.3);
  for (const fem::Stress& s : r.stress)
    for (int k = 0; k < 3; ++k)
      CHECK_THAT(s[k], WithinRel(sigma, 1e-9));
}

TEST_CASE("a free bar under a gradient through its depth bends without "
          "stress, at alpha dT / h",
          "[fem][thermal]") {
  const fem::FemMesh mesh = Bar();
  // Held statically determinately: a corner in all three, the far corner
  // along its length in two, a third corner in one.
  fem::Supports sup;
  for (size_t n = 0; n < mesh.nodes.size(); ++n) {
    const glm::dvec3 p = mesh.nodes[n];
    fem::HeldNode h;
    h.node = static_cast<int>(n);
    if (glm::length(p) < 1e-9)
      h.held = {true, true, true};
    else if (glm::length(p - glm::dvec3(kL, 0, 0)) < 1e-9)
      h.held = {false, true, true};
    else if (glm::length(p - glm::dvec3(0, kW, 0)) < 1e-9)
      h.held = {false, false, true};
    else
      continue;
    sup.held.push_back(h);
  }
  auto field = std::make_shared<fem::NodalTemperature>();
  field->stressFreeC = 20.0;
  const double dT = 50.0;
  for (const glm::dvec3& p : mesh.nodes)
    field->temperature.push_back(20.0 + dT * p.z / kW);
  const fem::FemResults r = Static(mesh, sup, field);
  for (const double vm : r.vonMises)
    CHECK(vm < 1e-6);
  // The curvature alpha dT / h: across the depth, the ends' axial motions
  // differ by kappa L h.
  const auto ux = [&](double x, double z) {
    for (size_t n = 0; n < mesh.nodes.size(); ++n)
      if (std::abs(mesh.nodes[n].x - x) < 1e-9 &&
          std::abs(mesh.nodes[n].y) < 1e-9 &&
          std::abs(mesh.nodes[n].z - z) < 1e-9)
        return r.displacement[n].x;
    FAIL("no node");
    return 0.0;
  };
  const double bend = (ux(kL, kW) - ux(kL, 0)) - (ux(0, kW) - ux(0, 0));
  CHECK_THAT(bend, WithinRel(12e-6 * dT / kW * kL * kW, 1e-9));
}

// ---- A bimetal strip
// (docs/simulation/README.md#a-thermal-load-on-an-assembly)

namespace {

// Timoshenko's curvature of a heated bimetal strip (S. Timoshenko, "Analysis
// of bi-metal thermostats", J. Opt. Soc. Am. 11, 233-255, 1925; the Kanthal
// handbook's eq. [1]): layers t1 and t2 thick, moduli E1 and E2, expansion
// coefficients a1 and a2, heated dT from flat. 1/mm, positive curling toward
// the layer that expands less.
double TimoshenkoCurvature(double t1, double t2, double e1, double e2,
                           double a1, double a2, double dT) {
  const double m = t1 / t2, n = e1 / e2, h = t1 + t2;
  return 6.0 * (a2 - a1) * dT * (1 + m) * (1 + m) /
         (h *
          (3.0 * (1 + m) * (1 + m) + (1 + m * n) * (m * m + 1.0 / (m * n))));
}

fem::Material Layer(double eMPa, double nu, double alpha) {
  fem::Material m;
  m.youngMPa = eMPa;
  m.poisson = nu;
  m.expansionPerK = alpha;
  return m;
}

// A 40 x 5 strip of two 0.5 mm layers, `lower` under `upper`, clamped
// across its end at x = 0 and heated from 20 to 100 C: how its free end
// (face 1, x = 40) moved as a rigid body.
fem::FaceMotion HeatedStrip(const fem::Material& lower,
                            const fem::Material& upper, size_t* nodes,
                            double* spanCurvature = nullptr) {
  fem::FemMesh mesh = GenerateBoxMesh(80, 5, 2, 40.0, 5.0, 1.0);
  mesh.tetDomain.resize(mesh.tets.size());
  for (size_t t = 0; t < mesh.tets.size(); ++t) {
    double z = 0.0;
    for (int k = 0; k < 4; ++k)
      z += 0.25 * mesh.nodes[mesh.tets[t][k]].z;
    mesh.tetDomain[t] = z < 0.5 ? 0 : 1;
  }
  fem::Supports sup;
  for (int n : NodesAtPlane(mesh, 0, 0.0, 1e-9))
    sup.fixedNodes.push_back(n);
  fem::SolveOptions o;
  o.thermal = Uniform(mesh, 100.0, 20.0);
  std::string err;
  const auto r = fem::SolveCases(mesh, {lower, upper}, sup, {{}}, &err,
                                 glm::dvec3(0.0), o);
  INFO(err);
  REQUIRE(r);
  // The section's rotation at x, from the axial motions of its top and
  // bottom fibres on the strip's mid-width.
  const auto turn = [&](double x) {
    double top = 0.0, bottom = 0.0;
    for (size_t n = 0; n < mesh.nodes.size(); ++n) {
      const glm::dvec3& p = mesh.nodes[n];
      if (std::abs(p.x - x) > 1e-9 || std::abs(p.y - 2.5) > 1e-9)
        continue;
      if (std::abs(p.z) < 1e-9)
        bottom = r->front().displacement[n].x;
      if (std::abs(p.z - 1.0) < 1e-9)
        top = r->front().displacement[n].x;
    }
    return bottom - top;
  };
  if (spanCurvature)
    *spanCurvature = (turn(10.0) - turn(30.0)) / 20.0;
  const auto fm = fem::FitFaceMotion(mesh, r->front().displacement, 1);
  REQUIRE(fm);
  REQUIRE(fm->rotationDetermined);
  if (nodes)
    *nodes = mesh.nodes.size();
  return *fm;
}

} // namespace

TEST_CASE("a steel and brass strip heated from flat curls as Timoshenko's "
          "bimetal, and a steel pair does not",
          "[fem][thermal]") {
  // The library's steel (E 200 GPa, 12e-6/K: EN 1993-1-1) under C26000
  // brass (E 110.3 GPa, nu 0.333, 19.2e-6/K: Wieland Concast's datasheet).
  const fem::Material steel = Layer(200000.0, 0.3, 12e-6);
  const fem::Material brass = Layer(110300.0, 16000.0 / 12000.0 - 1.0, 19.2e-6);
  const double L = 40.0;
  const double kappa =
      TimoshenkoCurvature(0.5, 0.5, 200000.0, 110300.0, 12e-6, 19.2e-6, 80.0);
  CHECK_THAT(kappa, WithinRel(8.448e-4, 1e-3)); // the plan's number
  size_t nodes = 0;
  double span = 0.0;
  const fem::FaceMotion fm = HeatedStrip(steel, brass, &nodes, &span);
  // Brass on top expands more, so the strip curls down: the free end turns
  // about +y (x toward -z) and drops.
  const double rotation = fm.rotation.y;
  const double drop = -fm.translation.z;
  std::printf("bimetal: steel/brass 40 x 5 x 2 x 0.5, 20 -> 100 C, %zu "
              "nodes: curvature between x = 10 and 30 %.5e /mm (Timoshenko "
              "%.5e, %+.3f %%); tip rotation %.6f rad (%.6f, %+.3f %%), "
              "drop %.6f mm (%.6f, %+.3f %%)\n",
              nodes, span, kappa, 100.0 * (span / kappa - 1), rotation,
              kappa * L, 100.0 * (rotation / (kappa * L) - 1), drop,
              kappa * L * L / 2, 100.0 * (drop / (kappa * L * L / 2) - 1));
  // Away from the clamp the strip curves as the closed form says. The clamp
  // holds both layers' ends across the width and thickness, and the end
  // turns a little more there (docs/simulation/README.md
  // #a-thermal-load-on-an-assembly): measured +1.9 % on the tip's rotation
  // and +3.6 % on its drop at L/b = 8.
  CHECK_THAT(span, WithinRel(kappa, 0.01));
  CHECK_THAT(rotation, WithinRel(kappa * L, 0.02));
  CHECK(drop > kappa * L * L / 2);
  CHECK_THAT(drop, WithinRel(kappa * L * L / 2, 0.04));
  // The control: both layers steel. It grows, and does not curl (what
  // turns is the structured mesh's asymmetry, a thousandth of the pair's).
  const fem::FaceMotion same = HeatedStrip(steel, steel, nullptr);
  std::printf("bimetal: steel/steel control, tip rotation %.3g rad, end "
              "moved %.6f mm along (alpha dT L %.6f)\n",
              glm::length(same.rotation), same.translation.x, 12e-6 * 80.0 * L);
  CHECK(glm::length(same.rotation) < 1e-3 * kappa * L);
  CHECK_THAT(same.translation.x, WithinRel(12e-6 * 80.0 * L, 0.01));
}

// ---- Studies ------------------------------------------------------------

TEST_CASE("a thermal study refuses what it cannot take, naming what it can",
          "[fem][thermal]") {
  fem::Study s;
  s.type = fem::AnalysisType::Thermal;
  fem::Load force;
  force.id = 1;
  force.type = fem::LoadType::Force;
  s.loads.push_back(force);
  CHECK_THAT(fem::ThermalRefusal(s),
             ContainsSubstring("temperature, heat_flux"));
  s.loads.clear();
  fem::Load t;
  t.id = 2;
  t.type = fem::LoadType::Temperature;
  CHECK_THAT(fem::ThermalLoadRefusal(t), ContainsSubstring("temperature_c"));
  t.temperatureC = -300.0;
  CHECK_THAT(fem::ThermalLoadRefusal(t), ContainsSubstring("absolute zero"));
  fem::Load c;
  c.id = 3;
  c.type = fem::LoadType::Convection;
  c.ambientC = 20.0;
  CHECK_THAT(fem::ThermalLoadRefusal(c), ContainsSubstring("Incropera"));
  c.filmWm2K = 0.0;
  CHECK_THAT(fem::ThermalLoadRefusal(c), ContainsSubstring("positive"));
  fem::Load f;
  f.id = 4;
  f.type = fem::LoadType::HeatFlux;
  CHECK_THAT(fem::ThermalLoadRefusal(f), ContainsSubstring("exactly one"));
  f.powerW = 1.0;
  f.fluxWm2 = 1.0;
  CHECK_THAT(fem::ThermalLoadRefusal(f), ContainsSubstring("exactly one"));
  // A quantity another type takes is refused, not carried into the file
  // (whose reader would refuse it on reopening, or an undo's snapshot).
  fem::Load src;
  src.id = 5;
  src.type = fem::LoadType::HeatGeneration;
  src.powerW = 1.0;
  src.body = "Part";
  CHECK(fem::ThermalLoadRefusal(src).empty());
  src.filmWm2K = 10.0;
  CHECK_THAT(fem::ThermalLoadRefusal(src), ContainsSubstring("film_w_m2k"));
  // A thermal load on a static study; load cases and gravity on a thermal
  // one.
  t.temperatureC = 20.0;
  fem::Study st;
  st.loads.push_back(t);
  CHECK_THAT(fem::ThermalRefusal(st),
             ContainsSubstring("study of type thermal"));
  s.loads = {t};
  s.loadCases.push_back({"a", 1.0, ""});
  CHECK_THAT(fem::ThermalRefusal(s), ContainsSubstring("load cases"));
  s.loadCases.clear();
  s.gravity = glm::dvec3(0, 0, -9.81);
  CHECK_THAT(fem::ThermalRefusal(s), ContainsSubstring("gravity"));
  // A thermal load where it cannot go, and without its stress-free
  // temperature.
  fem::Study modal;
  modal.type = fem::AnalysisType::Modal;
  modal.thermalLoad.kind = fem::ThermalLoad::Kind::Uniform;
  modal.thermalLoad.hasStressFree = true;
  CHECK_THAT(fem::ThermalRefusal(modal), ContainsSubstring("modal"));
  fem::Study buckle = modal;
  buckle.type = fem::AnalysisType::Buckling;
  CHECK_THAT(fem::ThermalRefusal(buckle),
             ContainsSubstring("thermal buckling"));
  fem::Study uniform;
  uniform.thermalLoad.kind = fem::ThermalLoad::Kind::Uniform;
  CHECK_THAT(fem::ThermalRefusal(uniform), ContainsSubstring("stress_free_c"));
}

TEST_CASE("the material library carries cited thermal numbers, and generic "
          "aluminium is gone",
          "[fem][thermal][materials]") {
  using CADMaterials::MaterialLibrary;
  using CADMaterials::MaterialType;
  const auto& steel = MaterialLibrary::GetMaterial(MaterialType::STEEL);
  CHECK_THAT(steel.conductivityWmK, WithinRel(53.334, 1e-9));
  CHECK(steel.expansionPerK == 12e-6);
  const auto& al = MaterialLibrary::GetMaterial(MaterialType::ALUMINUM_6061_T6);
  CHECK_THAT(al.conductivityWmK, WithinRel(191.4, 1e-9));
  CHECK(al.expansionPerK == 23e-6);
  const auto& pla = MaterialLibrary::GetMaterial(MaterialType::PLA);
  CHECK(pla.bulkConductivity);
  CHECK(pla.serviceMaxC == 55.0);
  CHECK(pla.expansionPerK == 0.0); // no resin maker publishes a measured one
  const auto& petg = MaterialLibrary::GetMaterial(MaterialType::PETG);
  CHECK(petg.serviceMaxC == 68.0);
  // Metals carry no service limit, and no material without a source has a
  // conductivity.
  CHECK(std::isnan(steel.serviceMaxC));
  CHECK(MaterialLibrary::GetMaterial(MaterialType::COPPER).conductivityWmK ==
        0.0);
  CHECK_THAT(MaterialLibrary::ConductivitySlugList(),
             ContainsSubstring("aluminum_6061_t6"));
  CHECK_THAT(MaterialLibrary::ExpansionSlugList(), !ContainsSubstring("pla"));
}

TEST_CASE("stainless, brass and the bimetal carry their cited numbers, and a "
          "row without a Poisson's ratio is refused a structural study",
          "[fem][thermal][materials]") {
  using CADMaterials::MaterialLibrary;
  using CADMaterials::MaterialType;
  // 1.4301: SCI P413 (2017), §2.3.1, Table 2.2 (cold-rolled strip), 2.7.
  const auto& ss = MaterialLibrary::GetMaterial(MaterialType::STAINLESS_STEEL);
  CHECK(ss.youngGPa == 200.0);
  CHECK(ss.poisson == 0.3);
  CHECK(ss.yieldMPa == 230.0);
  CHECK(ss.density == 7900.0);
  CHECK(ss.conductivityWmK == 15.0);
  CHECK(ss.expansionPerK == 16e-6);
  // C26000: Wieland Concast's datasheet. No yield: it gives rod's only.
  const auto& brass = MaterialLibrary::GetMaterial(MaterialType::BRASS);
  CHECK(brass.youngGPa == 110.3);
  CHECK_THAT(brass.poisson, WithinRel(1.0 / 3.0, 1e-12));
  CHECK(brass.yieldMPa == 0.0);
  CHECK(brass.density == 8530.0);
  CHECK(brass.conductivityWmK == 121.2);
  CHECK(brass.expansionPerK == 19.2e-6);
  CHECK_THAT(MaterialLibrary::ExpansionSlugList(), ContainsSubstring("brass"));
  // KANTHAL 94S: the handbook's p. 40 and type table.
  const auto& bi =
      MaterialLibrary::GetMaterial(MaterialType::BIMETAL_KANTHAL_94S);
  CHECK(MaterialLibrary::Slug(MaterialType::BIMETAL_KANTHAL_94S) ==
        "bimetal_kanthal_94s");
  CHECK(bi.youngGPa == 190.0);
  CHECK(bi.poisson == 0.0);
  CHECK(bi.conductivityWmK == 12.0);
  CHECK(bi.expansionPerK == 0.0);
  CHECK(bi.density == 8100.0);
  CHECK(bi.specificCurvaturePerK == 17.8e-6);
  CHECK(bi.specificCurvatureTolerance == 0.04);
  CHECK(bi.specificDeflectionPerK == 9.5e-6);
  CHECK(bi.linearFromC == 0.0);
  CHECK(bi.linearToC == 200.0);
  CHECK(MaterialLibrary::GradeOf("bimetal_kanthal_94s") == "KANTHAL 94S");
  CHECK_THAT(CADMaterials::ElasticDataRefusal(bi),
             ContainsSubstring("Poisson's ratio"));
  CHECK(CADMaterials::ElasticDataRefusal(ss).empty());
  CHECK(CADMaterials::ElasticDataRefusal(MaterialLibrary::GetMaterial(
            MaterialType::DEFAULT)) == "has no stiffness data");
}

#ifdef HAVE_NETGEN

namespace {

ElementRef FaceWhere(const Body& body, const std::string& kind,
                     const std::function<bool(const Measure::FaceInfo&)>& is) {
  for (const auto& [index, name] : body.elementMap.names) {
    const auto info = Measure::DescribeFaceAt(body, index);
    if (info && info->kind == kind && is(*info))
      return {body.createdBy, name};
  }
  FAIL("no such face");
  return {};
}

fem::Load Thermal(Document& doc, fem::LoadType type, const ElementRef& face) {
  fem::Load l;
  l.id = doc.AllocateId();
  l.type = type;
  l.face = face;
  return l;
}

fem::StudyRuntime Run(Document& doc, fem::Study& s,
                      fem::ConvergeMode mode = fem::ConvergeMode::Never) {
  fem::ConvergeOptions converge;
  converge.mode = mode;
  fem::StudyRuntime rt;
  std::string err;
  const bool ok = fem::RunStudy(doc, s, rt, converge, &err);
  INFO(err);
  REQUIRE(ok);
  return rt;
}

// A steel tube, ri 10 ro 20, 40 long.
void Tube(Document& doc) {
  auto outer = std::make_unique<CylinderFeature>(
      "Pipe", glm::dvec3(0, 0, 0), glm::dvec3(0, 0, 1), 20.0, 40.0);
  outer->SetMaterial("steel");
  doc.AddFeature(std::move(outer));
  const uint64_t solid = doc.GetFeature(0)->GetId();
  doc.AddFeature(std::make_unique<CylinderFeature>(
      "Bore", glm::dvec3(0, 0, -1.0), glm::dvec3(0, 0, 1), 10.0, 42.0));
  const uint64_t tool = doc.GetFeature(doc.FeatureCount() - 1)->GetId();
  doc.AddFeature(std::make_unique<BooleanFeature>("Hollow", solid, tool,
                                                  BooleanOp::Subtract));
}

} // namespace

TEST_CASE("a pipe held at its bore and cooled outside carries the radial "
          "heat",
          "[fem][thermal]") {
  Document doc;
  Tube(doc);
  REQUIRE(doc.GetBodies().size() == 1);
  const Body& body = doc.GetBodies()[0];
  const auto radius = [](double r) {
    return [r](const Measure::FaceInfo& f) {
      return std::abs(f.radius - r) < 1e-6;
    };
  };
  fem::Study& s = doc.AddStudy("Pipe");
  s.type = fem::AnalysisType::Thermal;
  s.meshSize = 2.5;
  fem::Load bore = Thermal(doc, fem::LoadType::Temperature,
                           FaceWhere(body, "cylinder", radius(10.0)));
  bore.temperatureC = 100.0;
  fem::Load out = Thermal(doc, fem::LoadType::Convection,
                          FaceWhere(body, "cylinder", radius(20.0)));
  out.filmWm2K = 25.0;
  out.ambientC = 20.0;
  s.loads = {bore, out};
  const fem::StudyRuntime rt = Run(doc, s);
  REQUIRE(rt.bodies.size() == 1);
  const auto& t = rt.bodies[0].thermal;
  REQUIRE(t.solved);
  const double k = 54.0 - 3.33e-2 * 20.0, h = 25e-3;
  const double perMm =
      2 * kPi * 80.0 / (std::log(2.0) / k + 1.0 / (h * 20.0)); // mW/mm
  const double w = perMm * 40.0 / 1000.0;
  double boreW = 0, outW = 0;
  for (const fem::HeatFlow& f : t.flows)
    (f.id == bore.id ? boreW : outW) = f.watts;
  std::printf("thermal: pipe %.6f W out (theory %.6f), bore %.6f, %zu nodes\n",
              outW, w, boreW, rt.bodies[0].mesh.stats.nodeCount);
  CHECK_THAT(outW, WithinRel(w, 0.01));
  CHECK_THAT(-boreW, WithinRel(w, 0.01));
}

TEST_CASE("a heated rod peaks at q R^2 / 4k + q R / 2h over its ambient, and "
          "the loop settles it",
          "[fem][thermal]") {
  Document doc;
  auto rod = std::make_unique<CylinderFeature>("Rod", glm::dvec3(0, 0, 0),
                                               glm::dvec3(0, 0, 1), 10.0, 40.0);
  rod->SetMaterial("pla");
  doc.AddFeature(std::move(rod));
  const Body& body = doc.GetBodies()[0];
  fem::Study& s = doc.AddStudy("Rod");
  s.type = fem::AnalysisType::Thermal;
  s.meshSize = 2.5;
  fem::Load out =
      Thermal(doc, fem::LoadType::Convection,
              FaceWhere(body, "cylinder", [](const auto&) { return true; }));
  out.filmWm2K = 25.0;
  out.ambientC = 20.0;
  fem::Load src;
  src.id = doc.AllocateId();
  src.type = fem::LoadType::HeatGeneration;
  src.body = body.name;
  src.powerW = 0.5;
  s.loads = {out, src};
  const fem::StudyRuntime rt = Run(doc, s, fem::ConvergeMode::Always);
  const auto& t = rt.bodies[0].thermal;
  const double k = 0.130, h = 25e-3, R = 10.0;
  const double q = 500.0 / (kPi * R * R * 40.0); // mW/mm^3
  const double peak = 20.0 + q * R * R / (4 * k) + q * R / (2 * h);
  std::printf("thermal: rod peak %.6f C (theory %.6f), converged %d in %zu "
              "steps\n",
              t.maxC, peak, rt.convergence.converged,
              rt.convergence.steps.size());
  CHECK_THAT(t.maxC, WithinRel(peak, 0.01));
  CHECK(rt.convergence.checked);
  CHECK(rt.convergence.converged);
  REQUIRE_FALSE(rt.convergence.steps.empty());
  CHECK(rt.convergence.steps[0].peaks[0].quantity == "temperature_rise");
  // PLA's heat deflection temperature, 55 C: the margin, and the warning
  // that its conductivity is the resin's.
  CHECK(t.hasService);
  CHECK_THAT(t.marginK, WithinAbs(55.0 - t.maxC, 1e-9));
  CHECK(t.bulkConductivity);
  // Saved and read back as it was.
  fem::StudyRuntime kept = rt;
  fem::KeepWhatIsRead(kept);
  REQUIRE_FALSE(kept.bodies[0].results.temperature.empty());
  CHECK(kept.bodies[0].results.temperature.size() ==
        kept.bodies[0].mesh.nodes.size());
  nlohmann::json j;
  REQUIRE(DocumentIO::StudyResultToJson(kept, j));
  const fem::StudyRuntime back = DocumentIO::StudyResultFromJson(j);
  REQUIRE(back.bodies.size() == 1);
  const auto& bt = back.bodies[0].thermal;
  CHECK(bt.solved);
  CHECK(bt.maxC == kept.bodies[0].thermal.maxC);
  CHECK(bt.marginK == kept.bodies[0].thermal.marginK);
  CHECK(bt.serviceBasis == kept.bodies[0].thermal.serviceBasis);
  CHECK(bt.flows.size() == 2);
  CHECK(bt.bulkConductivity);
  CHECK(back.bodies[0].results.temperature ==
        kept.bodies[0].results.temperature);
  CHECK(back.bodies[0].results.heatFlux == kept.bodies[0].results.heatFlux);
}

TEST_CASE("a static study takes a thermal study's temperature on its own mesh",
          "[fem][thermal]") {
  Document doc;
  auto bar =
      std::make_unique<BoxFeature>("Bar", glm::dvec3(0), 100.0, 10.0, 10.0);
  bar->SetMaterial("steel");
  doc.AddFeature(std::move(bar));
  const Body& body = doc.GetBodies()[0];
  const auto atX = [](double x) {
    return [x](const Measure::FaceInfo& f) {
      return std::abs(f.centroid.x - x) < 1e-6;
    };
  };
  fem::Study& heat = doc.AddStudy("Heat");
  heat.type = fem::AnalysisType::Thermal;
  heat.meshSize = 5.0;
  fem::Load hot = Thermal(doc, fem::LoadType::Temperature,
                          FaceWhere(body, "plane", atX(0.0)));
  hot.temperatureC = 100.0;
  fem::Load cold = Thermal(doc, fem::LoadType::Temperature,
                           FaceWhere(body, "plane", atX(100.0)));
  cold.temperatureC = 20.0;
  heat.loads = {hot, cold};
  const uint64_t heatId = heat.id;
  fem::Study& st = doc.AddStudy("Clamped");
  st.meshSize = 5.0;
  fem::Load a;
  a.id = doc.AllocateId();
  a.type = fem::LoadType::Fixed;
  a.face = FaceWhere(body, "plane", atX(0.0));
  fem::Load b = a;
  b.id = doc.AllocateId();
  b.face = FaceWhere(body, "plane", atX(100.0));
  st.loads = {a, b};
  st.thermalLoad.kind = fem::ThermalLoad::Kind::FromStudy;
  st.thermalLoad.fromStudy = heatId;
  // No convection, so no ambient to derive the stress-free temperature from.
  {
    fem::StudyRuntime rt;
    std::string err;
    CHECK_FALSE(fem::RunStudy(doc, st, rt, &err));
    CHECK_THAT(err, ContainsSubstring("stress_free_c"));
  }
  st.thermalLoad.hasStressFree = true;
  st.thermalLoad.stressFreeC = 20.0;
  const fem::StudyRuntime rt = Run(doc, st);
  const auto& b0 = rt.bodies[0];
  REQUIRE(b0.thermal.load);
  CHECK_THAT(b0.thermal.maxC, WithinAbs(100.0, 1e-6));
  CHECK_THAT(b0.thermal.minC, WithinAbs(20.0, 1e-6));
  REQUIRE(b0.results.temperature.size() == b0.mesh.nodes.size());
  // Clamped at both ends (all three directions), heated by 40 K on average:
  // the ends carry E alpha (T_mean - T0) A, give or take what the clamped
  // faces' lateral hold adds.
  double fx = 0.0;
  for (const auto& r : b0.results.reactions)
    if (r.support == b.id)
      fx = r.forceN.x;
  const double expect = 200000.0 * 12e-6 * 40.0 * 100.0;
  std::printf("thermal: clamped bar end force %.3f N (E alpha dT A %.3f)\n",
              -fx, expect);
  CHECK(std::abs(fx) > 0.9 * expect);
  CHECK(std::abs(fx) < 1.6 * expect);
  // The input hash takes the thermal study's definition.
  const uint64_t before = fem::StudyInputKey(doc, st);
  doc.StudyForRun(heatId)->loads[0].temperatureC = 120.0;
  CHECK(fem::StudyInputKey(doc, st) != before);
}

// The cost of a thermal study of 228k nodes (mesh, conduction, flux
// recovery, flows): a 100 x 20 x 20 steel bar held at its ends at 0.93 mm,
// measured 2026-10-29: 99.1 s of CPU pinned to an E-core (`taskset -c
// 20`), the slowest core this machine has. Netgen grades this bar to 228k
// nodes from 0.93 to 0.95 mm and jumps to 486k at 0.9, so 300k is not a
// size it makes. Bounded at twice the E-core's
// (docs/ui/testing.md#tests-that-time-things).
constexpr double kThermalCostBoundSeconds = 200.0;
TEST_CASE("a thermal study of 228k nodes costs what it was measured to",
          "[fem][thermal][slow]") {
  Document doc;
  auto bar =
      std::make_unique<BoxFeature>("Bar", glm::dvec3(0), 100.0, 20.0, 20.0);
  bar->SetMaterial("steel");
  doc.AddFeature(std::move(bar));
  const Body& body = doc.GetBodies()[0];
  const auto atX = [](double x) {
    return [x](const Measure::FaceInfo& f) {
      return std::abs(f.centroid.x - x) < 1e-6;
    };
  };
  fem::Study& s = doc.AddStudy("Bar");
  s.type = fem::AnalysisType::Thermal;
  s.meshSize = 0.93;
  fem::Load hot = Thermal(doc, fem::LoadType::Temperature,
                          FaceWhere(body, "plane", atX(0.0)));
  hot.temperatureC = 100.0;
  fem::Load cold = Thermal(doc, fem::LoadType::Temperature,
                           FaceWhere(body, "plane", atX(100.0)));
  cold.temperatureC = 20.0;
  s.loads = {hot, cold};
  const std::clock_t c0 = std::clock();
  const fem::StudyRuntime rt = Run(doc, s);
  const double cpu = double(std::clock() - c0) / CLOCKS_PER_SEC;
  const size_t nodes = rt.bodies[0].mesh.stats.nodeCount;
  std::printf("thermal cost: %zu nodes, %.1f s CPU, solver %s\n", nodes, cpu,
              rt.solver.c_str());
  CHECK(nodes > 200000);
  CHECK(nodes < 260000);
  CHECK(cpu < kThermalCostBoundSeconds);
  // k A dT / L, whatever the mesh.
  double out = 0.0;
  for (const fem::HeatFlow& f : rt.bodies[0].thermal.flows)
    if (f.id == cold.id)
      out = f.watts;
  CHECK_THAT(out, WithinRel(53.334 * 400.0 * 80.0 / 100.0 / 1000.0, 1e-6));
}

#endif // HAVE_NETGEN

#endif // HAVE_FEM_SOLVER
