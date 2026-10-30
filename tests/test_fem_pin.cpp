// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

// The pin support and the support-edge flag
// (docs/simulation/README.md#pin-supports,
// docs/simulation/README.md#a-peak-on-a-supports-edge). Each is checked by
// its outcome: a pinned lug against the textbook's bearing and net-section
// stresses, a lever's two pin reactions against its statics, the clamp's
// spike that a pin does not have, and the refusals that keep a mechanism
// from being solved.

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
#include "core/fem/FemTopo.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <numbers>
#include <string>
#include <vector>

using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

// A plate `length` x `width` x `thick` (x, y, z from the origin) with a
// through hole of diameter `d` at each of `holesX` (on y = width/2).
struct Plate {
  Document doc;
  std::vector<ElementRef> bores; // in holesX order
  ElementRef endX0, endX1;       // the faces at x = 0 and x = length
  ElementRef top;                // z = thick
};

std::unique_ptr<Plate> MakePlate(double length, double width, double thick,
                                 double d, const std::vector<double>& holesX,
                                 const char* material = "pla") {
  auto p = std::make_unique<Plate>();
  Document& doc = p->doc;
  auto box = std::make_unique<BoxFeature>("Plate", glm::dvec3(0), length, width,
                                          thick);
  box->SetMaterial(material);
  doc.AddFeature(std::move(box));
  uint64_t solid = doc.GetFeature(0)->GetId();
  for (double x : holesX) {
    doc.AddFeature(std::make_unique<CylinderFeature>(
        "Hole", glm::dvec3(x, width / 2, -1.0), glm::dvec3(0, 0, 1), d / 2,
        thick + 2.0));
    const uint64_t tool = doc.GetFeature(doc.FeatureCount() - 1)->GetId();
    doc.AddFeature(std::make_unique<BooleanFeature>("Bore", solid, tool,
                                                    BooleanOp::Subtract));
    solid = doc.GetFeature(doc.FeatureCount() - 1)->GetId();
  }
  REQUIRE(doc.GetBodies().size() == 1);
  const Body& body = doc.GetBodies()[0];
  p->bores.resize(holesX.size());
  for (const auto& [index, name] : body.elementMap.names) {
    const auto info = Measure::DescribeFaceAt(body, index);
    REQUIRE(info.has_value());
    const ElementRef ref{body.createdBy, name};
    if (info->kind == "cylinder") {
      for (size_t i = 0; i < holesX.size(); ++i)
        if (std::abs(info->centroid.x - holesX[i]) < 1e-3)
          p->bores[i] = ref;
    } else if (std::abs(info->centroid.x) < 1e-6) {
      p->endX0 = ref;
    } else if (std::abs(info->centroid.x - length) < 1e-6) {
      p->endX1 = ref;
    } else if (std::abs(info->centroid.z - thick) < 1e-6) {
      p->top = ref;
    }
  }
  for (const ElementRef& b : p->bores)
    REQUIRE_FALSE(b.name.empty());
  REQUIRE_FALSE(p->endX0.name.empty());
  REQUIRE_FALSE(p->endX1.name.empty());
  return p;
}

fem::Load Support(Document& doc, fem::LoadType type, const ElementRef& face,
                  bool fixRotation = false) {
  fem::Load l;
  l.id = doc.AllocateId();
  l.type = type;
  l.face = face;
  l.fixRotation = fixRotation;
  return l;
}

fem::Load Force(Document& doc, const ElementRef& face, double n,
                const glm::dvec3& dir) {
  fem::Load l;
  l.id = doc.AllocateId();
  l.type = fem::LoadType::Force;
  l.face = face;
  l.magnitude = n;
  l.direction = dir;
  return l;
}

// The solving cases need a mesh: a build without Netgen refuses every one
// ("built without FEM support"), which test_fem.cpp checks. Only the
// refusals that come before the mesh run there.
#ifdef HAVE_NETGEN
const fem::FemResults::SupportReaction* ReactionOf(const fem::FemResults& r,
                                                   uint64_t id) {
  for (const auto& sr : r.reactions)
    if (sr.support == id)
      return &sr;
  return nullptr;
}

double YieldOf(const char* material) {
  return CADMaterials::MaterialLibrary::GetMaterial(
             CADMaterials::MaterialLibrary::MaterialTypeFromString(material))
      .yieldMPa;
}
#endif // HAVE_NETGEN

} // namespace

#ifdef HAVE_NETGEN

// A lug: a plate with a hole near one end, held by a pin in the hole and
// pulled from the other end. The pin pushes on the half of the bore between
// the hole and the lug's end; the textbook reads the bearing stress as
// F / (d t) over the projected area, its peak under a cosine distribution
// as (4/pi) F / (d t), and the net section across the hole as
// F / ((w - d) t), raised by the hole's concentration at its sides.
TEST_CASE("a pinned lug bears on the loaded half of its bore",
          "[fem][pin][run]") {
  const double L = 60.0, w = 20.0, t = 4.0, d = 8.0, x0 = 12.0, F = 400.0;
  auto p = MakePlate(L, w, t, d, {x0}, "steel");
  Document& doc = p->doc;
  fem::Study& s = doc.AddStudy("Lug");
  s.meshSize = 1.6;
  // One pin alone leaves the lug free to turn on it, so it is held from
  // turning; the pull passes through the axis, so that holds nothing.
  s.loads.push_back(Support(doc, fem::LoadType::Pin, p->bores[0], true));
  s.loads.push_back(Force(doc, p->endX1, F, glm::dvec3(1, 0, 0)));
  const uint64_t pinId = s.loads[0].id;
  fem::StudyRuntime rt;
  std::string err;
  const bool ok = fem::RunStudy(doc, s, rt, &err);
  INFO(err);
  REQUIRE(ok);
  const fem::StudyBodyResult& b = rt.bodies[0];
  const fem::FemResults& r = b.results;

  // The pin carries the whole pull, against it.
  const auto* sr = ReactionOf(r, pinId);
  REQUIRE(sr);
  CHECK(sr->type == fem::LoadType::Pin);
  CHECK_THAT(sr->forceN.x, WithinRel(-F, 1e-4));
  CHECK_THAT(sr->forceN.y, WithinAbs(0.0, 1e-4 * F));
  CHECK_THAT(sr->forceN.z, WithinAbs(0.0, 1e-4 * F));
  // On about half of the bore: a pin in a hole of its own size touches the
  // half it is pressed against, not the far one.
  const double frac = sr->contactAreaMm2 / sr->faceAreaMm2;
  INFO("contact fraction " << frac);
  CHECK(frac > 0.35);
  CHECK(frac < 0.65);

  // Where it bears: the radial stress on the bore, read at its nodes, is
  // compressive on the side towards the lug's end (x < x0) and nothing on
  // the far side. Its peak against the cosine distribution's (4/pi) F/(dt).
  const glm::dvec3 axisO(x0, w / 2, 0.0);
  double peakBearing = 0.0, farSide = 0.0;
  // The net section's stress at the hole's sides (x = x0, y = w/2 +- d/2).
  double sideStress = 0.0;
  for (size_t n = 0; n < b.mesh.nodes.size(); ++n) {
    const glm::dvec3 x = b.mesh.nodes[n];
    glm::dvec3 rv = x - axisO;
    rv.z = 0.0;
    const double rr = glm::length(rv);
    if (std::abs(rr - d / 2) > 1e-3 || x.z < 0.5 || x.z > t - 0.5)
      continue; // on the bore, off its rims
    const glm::dvec3 m = rv / rr;
    const double sigRR = fem::NormalStress(r.stress[n], m);
    if (m.x < -0.7)
      peakBearing = std::max(peakBearing, -sigRR);
    if (m.x > 0.7)
      farSide = std::max(farSide, std::abs(sigRR));
    if (std::abs(m.x) < 0.2)
      sideStress = std::max(sideStress, r.stress[n][0]);
  }
  const double bearing = F / (d * t);
  const double net = F / ((w - d) * t);
  INFO("peak bearing " << peakBearing << " MPa, (4/pi) F/(dt) "
                       << 4.0 / std::numbers::pi * bearing << "; far side "
                       << farSide << "; hole side " << sideStress
                       << " MPa against the net section's " << net);
  CHECK(peakBearing > 0.9 * bearing);
  CHECK(peakBearing < 2.0 * bearing);
  CHECK(farSide < 0.15 * peakBearing);
  // A pin-loaded lug's concentration on the net section is about 2-3
  // (Peterson, d/w = 0.4); the sides carry it, and no more.
  CHECK(sideStress > 1.5 * net);
  CHECK(sideStress < 4.0 * net);
}

// The case the support was built for: a printed lever hung on two pins and
// pushed down at its end. Statics says what each pin carries; the clamp
// says something else, on the rim of a bore, and is flagged for it.
TEST_CASE("a lever on two pins: reactions balance the load and the clamp's "
          "spike is gone",
          "[fem][pin][run][slow][asan_slow]") {
  const double L = 100.0, w = 12.0, t = 6.0, d = 5.0, x1 = 8.0, x2 = 30.0,
               F = 20.0;
  auto p = MakePlate(L, w, t, d, {x1, x2}, "pla");
  Document& doc = p->doc;
  const double xF = L; // the force acts on the end face
  const auto solve = [&](fem::LoadType type, double boreSize,
                         fem::StudyRuntime& rt) {
    fem::Study& s =
        doc.AddStudy(type == fem::LoadType::Pin ? "Pins" : "Clamps");
    s.meshSize = 2.0;
    // The bores' sizes below ARE the comparison: a run that refined round
    // its peak (converge: auto, the default) would refine the coarse one
    // as far as the time allows, and the clamp's spike then reads against
    // the machine's load, not the mesh.
    s.converge = fem::ConvergeMode::Never;
    for (const ElementRef& bore : p->bores)
      s.refinements.push_back({bore, boreSize, "", 0});
    s.loads.push_back(Support(doc, type, p->bores[0]));
    s.loads.push_back(Support(doc, type, p->bores[1]));
    s.loads.push_back(Force(doc, p->endX1, F, glm::dvec3(0, -1, 0)));
    std::string err;
    const bool ok = fem::RunStudy(doc, s, rt, &err);
    INFO(err);
    REQUIRE(ok);
    return std::make_pair(s.loads[0].id, s.loads[1].id);
  };

  fem::StudyRuntime pinned, clamped, pinnedFine, clampedFine;
  const auto [p1, p2] = solve(fem::LoadType::Pin, 1.0, pinned);
  solve(fem::LoadType::Fixed, 1.0, clamped);
  solve(fem::LoadType::Pin, 0.5, pinnedFine);
  solve(fem::LoadType::Fixed, 0.5, clampedFine);
  const fem::FemResults& r = pinned.bodies[0].results;

  // Statics: about pin 1, R2 (x2 - x1) = F (xF - x1); R1 + R2 = F.
  const double R2 = F * (xF - x1) / (x2 - x1);
  const double R1 = R2 - F;
  const auto* a = ReactionOf(r, p1);
  const auto* b = ReactionOf(r, p2);
  REQUIRE(a);
  REQUIRE(b);
  INFO("pin reactions y " << a->forceN.y << ", " << b->forceN.y << "; statics "
                          << -R1 << ", " << R2);
  const glm::dvec3 sum = a->forceN + b->forceN + glm::dvec3(0, -F, 0);
  CHECK(glm::length(sum) < 1e-4 * F);
  // And their moments answer the load's: the end face's uniform push acts
  // at its middle, (L, w/2, t/2), about the part's origin.
  REQUIRE(a->hasMoment);
  REQUIRE(b->hasMoment);
  const glm::dvec3 load =
      glm::cross(glm::dvec3(xF, w / 2, t / 2), glm::dvec3(0, -F, 0));
  INFO("pin moments " << a->momentNmm.z << ", " << b->momentNmm.z
                      << "; the load's " << load.z);
  CHECK(glm::length(a->momentNmm + b->momentNmm + load) < 1e-4 * F * L);
  CHECK_THAT(a->forceN.y, WithinRel(-R1, 0.01));
  CHECK_THAT(b->forceN.y, WithinRel(R2, 0.01));
  // Each pin presses on about half its bore, not all of it.
  CHECK(a->contactAreaMm2 < 0.7 * a->faceAreaMm2);
  CHECK(b->contactAreaMm2 < 0.7 * b->faceAreaMm2);

  // Bending at pin 2 across the bored section, M c / I, governs the lever.
  const double M = F * (xF - x2);
  const double I = t * (w * w * w - d * d * d) / 12.0;
  const double bend = M * (w / 2) / I;
  const double yield = YieldOf("pla");
  const auto peak = [](const fem::StudyRuntime& rt) {
    return rt.bodies[0].results.maxVonMises;
  };
  const double pin0 = peak(pinned), pin1 = peak(pinnedFine);
  const double clamp0 = peak(clamped), clamp1 = peak(clampedFine);
  INFO("peak von Mises, bores meshed at 1.0 / 0.5 mm: pinned "
       << pin0 << " / " << pin1 << " MPa (FoS " << yield / pin1 << "), clamped "
       << clamp0 << " / " << clamp1 << " MPa (FoS " << yield / clamp1
       << "); M c / I " << bend);
  WARN("lever: pinned " << pin0 << " / " << pin1 << " MPa, clamped " << clamp0
                        << " / " << clamp1 << " MPa, Mc/I " << bend
                        << " MPa, yield " << yield);
  // The pin's peak is the lever's bending, and holds still as the bores'
  // mesh is refined.
  CHECK(pin1 < 1.6 * bend);
  CHECK(pin1 > 0.8 * bend);
  CHECK(std::abs(pin1 - pin0) < 0.03 * pin0);
  // The clamp's grows with the mesh: it sits on a bore's rim, where a clamp
  // is a singularity, and is flagged for it.
  CHECK(clamp1 > 1.05 * clamp0);
  CHECK(clamp1 > pin1);
  const auto& edge = clampedFine.bodies[0].results.supportEdgePeak;
  CHECK(edge.flagged);
  CHECK(edge.type == fem::LoadType::Fixed);
  CHECK(edge.awayMPa < clamp1);
}

// The flag on its own: a cantilever clamped at its root peaks at the
// clamp's edge; a bar with a hole, clamped at one end and pulled at the
// other, peaks at the hole, far from the clamp.
TEST_CASE("a peak on a support's edge is flagged, and one elsewhere is not",
          "[fem][pin][run]") {
  {
    auto p = MakePlate(80.0, 10.0, 10.0, 0.0, {}, "steel");
    Document& doc = p->doc;
    fem::Study& s = doc.AddStudy("Cantilever");
    s.meshSize = 2.5;
    s.loads.push_back(Support(doc, fem::LoadType::Fixed, p->endX0));
    s.loads.push_back(Force(doc, p->endX1, 500.0, glm::dvec3(0, 0, -1)));
    fem::StudyRuntime rt;
    std::string err;
    REQUIRE(fem::RunStudy(doc, s, rt, &err));
    const fem::FemResults& r = rt.bodies[0].results;
    REQUIRE(r.supportEdgePeak.flagged);
    CHECK(r.supportEdgePeak.support == s.loads[0].id);
    CHECK(r.supportEdgePeak.awayMPa > 0.0);
    CHECK(r.supportEdgePeak.awayMPa < r.maxVonMises);
    // The fixed support's reaction answers the load.
    const auto* sr = ReactionOf(r, s.loads[0].id);
    REQUIRE(sr);
    CHECK_THAT(sr->forceN.z, WithinRel(500.0, 1e-4));
  }
  {
    auto p = MakePlate(120.0, 20.0, 4.0, 10.0, {60.0}, "steel");
    Document& doc = p->doc;
    fem::Study& s = doc.AddStudy("Bar");
    s.meshSize = 2.0;
    s.loads.push_back(Support(doc, fem::LoadType::Fixed, p->endX0));
    s.loads.push_back(Force(doc, p->endX1, 1000.0, glm::dvec3(1, 0, 0)));
    fem::StudyRuntime rt;
    std::string err;
    REQUIRE(fem::RunStudy(doc, s, rt, &err));
    const fem::FemResults& r = rt.bodies[0].results;
    INFO("peak at x " << r.maxVmAt.x);
    CHECK(std::abs(r.maxVmAt.x - 60.0) < 6.0);
    CHECK_FALSE(r.supportEdgePeak.flagged);
  }
}
#endif // HAVE_NETGEN

// A pin that leaves a mechanism is refused, naming it -- never factored.
TEST_CASE("a pin is refused where it cannot hold the part", "[fem][pin]") {
  auto p = MakePlate(60.0, 12.0, 5.0, 5.0, {8.0, 30.0}, "pla");
  Document& doc = p->doc;
  fem::StudyRuntime rt;
  std::string err;

  SECTION("on a face that is not a cylinder") {
    fem::Study& s = doc.AddStudy("Flat");
    s.loads.push_back(Support(doc, fem::LoadType::Pin, p->endX0));
    s.loads.push_back(Force(doc, p->endX1, 10.0, glm::dvec3(0, -1, 0)));
    CHECK_FALSE(fem::RunStudy(doc, s, rt, &err));
    CHECK_THAT(err, ContainsSubstring("cylindrical face"));
  }
#ifdef HAVE_NETGEN // found on the meshed part
  SECTION("alone and free to turn: a lever on one pin is a mechanism") {
    fem::Study& s = doc.AddStudy("One pin");
    s.meshSize = 2.0;
    s.loads.push_back(Support(doc, fem::LoadType::Pin, p->bores[0]));
    s.loads.push_back(Force(doc, p->endX1, 10.0, glm::dvec3(0, -1, 0)));
    CHECK_FALSE(fem::RunStudy(doc, s, rt, &err));
    CHECK_THAT(err,
               ContainsSubstring("free to turn") &&
                   ContainsSubstring("pin " + std::to_string(s.loads[0].id)) &&
                   ContainsSubstring("fix_rotation"));
    // Held from turning, the same pin carries it.
    s.loads[0].fixRotation = true;
    const bool ok = fem::RunStudy(doc, s, rt, &err);
    INFO(err);
    CHECK(ok);
  }
  SECTION("two pins on one axis still let it turn") {
    auto q = MakePlate(60.0, 12.0, 5.0, 5.0, {8.0}, "pla");
    fem::Study& s = q->doc.AddStudy("Coaxial");
    s.meshSize = 2.0;
    fem::Load half = Support(q->doc, fem::LoadType::Pin, q->bores[0]);
    half.region.kind = fem::LoadRegion::Kind::Band;
    half.region.normal = glm::dvec3(0, 0, 1);
    half.region.fromMm = -1.0;
    half.region.toMm = 2.5;
    fem::Load other = half;
    other.id = q->doc.AllocateId();
    other.region.fromMm = 2.5;
    other.region.toMm = 6.0;
    s.loads.push_back(half);
    s.loads.push_back(other);
    s.loads.push_back(Force(q->doc, q->endX1, 10.0, glm::dvec3(0, -1, 0)));
    CHECK_FALSE(fem::RunStudy(q->doc, s, rt, &err));
    CHECK_THAT(err, ContainsSubstring("free to turn"));
  }
  SECTION("a load that lifts the part off the halves of its bores") {
    fem::Study& s = doc.AddStudy("Lift");
    s.meshSize = 2.0;
    // Only the lower halves of the two bores: each pin can push the lever
    // down and nothing else, so pushed down at its end, it falls off them.
    for (const ElementRef& bore : p->bores) {
      fem::Load low = Support(doc, fem::LoadType::Pin, bore);
      low.region.kind = fem::LoadRegion::Kind::Band;
      low.region.normal = glm::dvec3(0, 1, 0);
      low.region.fromMm = -1.0;
      low.region.toMm = 5.0; // below the axes (y = 6) by a facet or more
      s.loads.push_back(low);
    }
    s.loads.push_back(Force(doc, p->endX1, 10.0, glm::dvec3(0, -1, 0)));
    const bool ok = fem::RunStudy(doc, s, rt, &err);
    INFO(err);
    CHECK_FALSE(ok);
    CHECK_THAT(err, ContainsSubstring("pull it off its pins"));
  }
#endif // HAVE_NETGEN
  SECTION("on a modal or a topology study") {
    fem::Study& s = doc.AddStudy("Modal");
    s.type = fem::AnalysisType::Modal;
    s.loads.push_back(Support(doc, fem::LoadType::Pin, p->bores[0], true));
    CHECK_FALSE(fem::RunStudy(doc, s, rt, &err));
    CHECK_THAT(err, ContainsSubstring("modal study's supports are fixed"));
    s.type = fem::AnalysisType::Topology;
    s.loads.push_back(Force(doc, p->endX1, 10.0, glm::dvec3(0, -1, 0)));
    CHECK_FALSE(fem::RunTopology(doc, s, rt, &err));
    CHECK_THAT(err, ContainsSubstring("topology study's supports are fixed"));
  }
}

#ifdef HAVE_NETGEN
// The iterative solve agrees with the direct one on a pinned part, and an
// external (GPU) solver offered for it declines, saying why: it applies its
// element matrices in x, y, z, and a pin holds its nodes in their own frames.
TEST_CASE("a pinned study solves iteratively as it does directly, and the "
          "GPU says it declined",
          "[fem][pin][run]") {
  auto p = MakePlate(60.0, 20.0, 4.0, 8.0, {12.0}, "steel");
  Document& doc = p->doc;
  fem::Study& s = doc.AddStudy("Lug");
  s.meshSize = 2.0;
  s.loads.push_back(Support(doc, fem::LoadType::Pin, p->bores[0], true));
  s.loads.push_back(Force(doc, p->endX1, 400.0, glm::dvec3(1, 0, 0)));
  std::string err;
  fem::StudyJobInput in;
  REQUIRE(fem::PrepareStudy(doc, s, in, &err));
  fem::StudyJobInput again = in;
  fem::StudyRuntime direct, iterative;
  {
    const bool ok = fem::SolvePreparedStudy(in, direct, &err);
    INFO(err);
    REQUIRE(ok);
  }
  fem::SolveOptions opts;
  opts.iterativeAboveDofs = 0;
  bool offered = false;
  opts.makeExternal =
      [&](std::string*) -> std::unique_ptr<fem::TopoLinearSolver> {
    offered = true;
    return nullptr;
  };
  {
    const bool ok = fem::SolvePreparedStudy(again, iterative, &err, opts);
    INFO(err);
    REQUIRE(ok);
  }
  CHECK_FALSE(offered);
  CHECK_THAT(iterative.solver, ContainsSubstring("GPU declined") &&
                                   ContainsSubstring("pin support"));
  const auto& u0 = direct.bodies[0].results.displacement;
  const auto& u1 = iterative.bodies[0].results.displacement;
  REQUIRE(u0.size() == u1.size());
  double worst = 0.0;
  for (size_t i = 0; i < u0.size(); ++i)
    worst = std::max(worst, glm::length(u0[i] - u1[i]));
  CHECK(worst < 1e-5 * direct.maxDisplacement);
  CHECK_THAT(iterative.maxVonMises, WithinRel(direct.maxVonMises, 1e-4));
}
#endif // HAVE_NETGEN

#ifdef HAVE_NETGEN
namespace {
// A flanged bushing: a sleeve of diameter `od` and length `len` along +z
// from z = 0, a flange of diameter `fd` and thickness `ft` above it, and a
// bore of diameter `bd` through both.
struct Bushing {
  Document doc;
  ElementRef sleeve, bore;
};
std::unique_ptr<Bushing> MakeBushing(double od, double len, double fd,
                                     double ft, double bd,
                                     const char* material) {
  auto b = std::make_unique<Bushing>();
  Document& doc = b->doc;
  auto sleeve = std::make_unique<CylinderFeature>(
      "Sleeve", glm::dvec3(0), glm::dvec3(0, 0, 1), od / 2, len);
  sleeve->SetMaterial(material);
  doc.AddFeature(std::move(sleeve));
  const uint64_t s = doc.GetFeature(0)->GetId();
  doc.AddFeature(std::make_unique<CylinderFeature>(
      "Flange", glm::dvec3(0, 0, len), glm::dvec3(0, 0, 1), fd / 2, ft));
  const uint64_t f = doc.GetFeature(1)->GetId();
  doc.AddFeature(
      std::make_unique<BooleanFeature>("Join", s, f, BooleanOp::Union));
  const uint64_t j = doc.GetFeature(2)->GetId();
  doc.AddFeature(std::make_unique<CylinderFeature>(
      "Hole", glm::dvec3(0, 0, -1), glm::dvec3(0, 0, 1), bd / 2, len + ft + 2));
  const uint64_t h = doc.GetFeature(3)->GetId();
  doc.AddFeature(
      std::make_unique<BooleanFeature>("Bore", j, h, BooleanOp::Subtract));
  REQUIRE(doc.GetBodies().size() == 1);
  const Body& body = doc.GetBodies()[0];
  for (const auto& [index, name] : body.elementMap.names) {
    const auto info = Measure::DescribeFaceAt(body, index);
    REQUIRE(info.has_value());
    if (info->kind != "cylinder")
      continue;
    if (std::abs(info->radius - od / 2) < 1e-6)
      b->sleeve = {body.createdBy, name};
    else if (std::abs(info->radius - bd / 2) < 1e-6)
      b->bore = {body.createdBy, name};
  }
  REQUIRE_FALSE(b->sleeve.name.empty());
  REQUIRE_FALSE(b->bore.name.empty());
  return b;
}
} // namespace

TEST_CASE("a short sleeve on a pin, leaned on off its end, settles on its "
          "two ends",
          "[fem][pin]") {
  // The woodcutter's left bushing: a 14.8 x 6.2 mm sleeve under a 2 mm
  // flange, on a pin over its sleeve held from turning, loaded through its
  // bore by 35 N 22 mm off the sleeve's middle. The load tilts it, pressing
  // the sleeve at its two ends on opposite sides; the contact set once
  // cycled between them and never settled ("did not settle in 30
  // iterations" at 1.6 mm, and in 131 it ended still pulling).
  constexpr double P = 35.0, off = 22.0;
  for (const double h : {1.6}) {
    auto b = MakeBushing(14.8, 6.2, 20.0, 2.0, 8.0, "pla");
    Document& doc = b->doc;
    fem::Study& s = doc.AddStudy("Bushing");
    s.meshSize = h;
    s.converge = fem::ConvergeMode::Never;
    const fem::Load pin = Support(doc, fem::LoadType::Pin, b->sleeve, true);
    s.loads.push_back(pin);
    fem::Load r;
    r.id = doc.AllocateId();
    r.type = fem::LoadType::Remote;
    r.face = b->bore;
    r.magnitude = P;
    r.direction = glm::dvec3(1, 0, 0);
    r.point = glm::dvec3(0, 0, 3.1 + off);
    r.hasPoint = true;
    s.loads.push_back(r);
    fem::StudyRuntime rt;
    std::string err;
    const bool ok = fem::RunStudy(doc, s, rt, &err);
    INFO("mesh " << h << " mm: " << err);
    REQUIRE(ok);
    const fem::FemResults::SupportReaction* sr =
        ReactionOf(rt.bodies[0].results, pin.id);
    REQUIRE(sr != nullptr);
    std::printf("pin: bushing at %.1f mm, reaction (%.4f, %.4f, %.4f) N, "
                "pressing over %.2f of %.2f mm^2\n",
                h, sr->forceN.x, sr->forceN.y, sr->forceN.z, sr->contactAreaMm2,
                sr->faceAreaMm2);
    // The pin answers the load, and presses on part of the sleeve only.
    CHECK_THAT(sr->forceN.x, WithinRel(-P, 1e-3));
    CHECK(std::abs(sr->forceN.y) < 1e-3 * P);
    CHECK(sr->contactAreaMm2 > 0.0);
    CHECK(sr->contactAreaMm2 < 0.5 * sr->faceAreaMm2);
  }
}
#endif
