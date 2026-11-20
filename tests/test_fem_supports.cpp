// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

// The roller, held and cylindrical supports
// (docs/simulation/README.md#rollers-held-directions-and-cylindrical-supports),
// each checked by what it is for: a half bar on a symmetry plane is the
// whole bar, a quarter of Lame's cylinder on its rollers is the closed
// form, an inclined symmetry plane is its axis-aligned image, a prescribed
// displacement carries the reaction the stiffness says, a mode survives the
// halving; and the refusals that keep a support from meaning something it
// cannot. CalculiX's agreement on the same mesh is test_fem_calculix.cpp's.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "cad/CADMaterials.h"
#include "core/Document.h"
#include "core/Feature.h"
#include "core/Measure.h"
#include "core/features/BooleanFeature.h"
#include "core/features/FrameFeature.h"
#include "core/fem/FemSolver.h"
#include "core/fem/FemStudy.h"
#include "core/io/Serialization.h"

#include <cmath>
#include <functional>
#include <memory>
#include <string>
#include <vector>

using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

// The face of the document's only body whose description `pick` accepts.
ElementRef
FaceWhere(const Document& doc,
          const std::function<bool(const Measure::FaceInfo&)>& pick) {
  REQUIRE(doc.GetBodies().size() == 1);
  const Body& body = doc.GetBodies()[0];
  ElementRef out;
  for (const auto& [index, name] : body.elementMap.names) {
    const auto info = Measure::DescribeFaceAt(body, index);
    if (info && pick(*info)) {
      REQUIRE(out.name.empty()); // one face, or the test is ambiguous
      out = ElementRef{body.createdBy, name};
    }
  }
  REQUIRE_FALSE(out.name.empty());
  return out;
}
// The flat face whose centroid has coordinate `axis` at `at`.
ElementRef FlatAt(const Document& doc, int axis, double at) {
  return FaceWhere(doc, [&](const Measure::FaceInfo& f) {
    return f.kind == "plane" && std::abs(f.centroid[axis] - at) < 1e-6 &&
           std::abs(std::abs(f.normal[axis]) - 1.0) < 1e-9;
  });
}

fem::Load Support(Document& doc, fem::LoadType type, const ElementRef& face) {
  fem::Load l;
  l.id = doc.AllocateId();
  l.type = type;
  l.face = face;
  return l;
}
fem::Load Held(Document& doc, const ElementRef& face, std::array<bool, 3> hold,
               std::array<double, 3> mm = {0.0, 0.0, 0.0}) {
  fem::Load l = Support(doc, fem::LoadType::Held, face);
  l.hold = hold;
  for (int k = 0; k < 3; ++k)
    l.prescribedMm[k] = mm[k];
  return l;
}
fem::Load Force(Document& doc, const ElementRef& face, double n,
                const glm::dvec3& dir) {
  fem::Load l = Support(doc, fem::LoadType::Force, face);
  l.magnitude = n;
  l.direction = dir;
  return l;
}

const fem::FemResults::SupportReaction* ReactionOf(const fem::FemResults& r,
                                                   uint64_t id) {
  for (const auto& sr : r.reactions)
    if (sr.support == id)
      return &sr;
  return nullptr;
}

fem::Material Steel() {
  return fem::MaterialFromLibrary(CADMaterials::MaterialLibrary::GetMaterial(
      CADMaterials::MaterialLibrary::MaterialTypeFromString("steel")));
}

void AddBox(Document& doc, double x, double y, double z) {
  auto box = std::make_unique<BoxFeature>("Bar", glm::dvec3(0), x, y, z);
  box->SetMaterial("steel");
  doc.AddFeature(std::move(box));
}

// The mean of `f` over the nodes of the solved mesh `accept` takes.
double MeanOver(const fem::StudyBodyResult& b,
                const std::function<bool(const glm::dvec3&)>& accept,
                const std::function<double(size_t)>& f) {
  double sum = 0.0;
  int n = 0;
  for (size_t i = 0; i < b.mesh.nodes.size(); ++i)
    if (accept(b.mesh.nodes[i])) {
      sum += f(i);
      ++n;
    }
  REQUIRE(n > 0);
  return sum / n;
}

std::string Run(Document& doc, fem::Study& s, fem::StudyRuntime& rt) {
  s.converge = fem::ConvergeMode::Never;
  std::string err;
  if (!fem::RunStudy(doc, s, rt, &err))
    return err.empty() ? std::string("failed") : err;
  return {};
}

} // namespace

#ifdef HAVE_NETGEN

// A bar with three symmetry planes (x = 0, y = 0, z = 0) is free to stretch
// and nothing else: a held end moved by delta stretches it uniformly, and a
// tet10 represents that field exactly. So the held face carries
// E A delta / L to the solver's precision, and every node moves by
// x delta / L along the bar and -nu y delta / L across it.
TEST_CASE("a prescribed displacement carries the reaction the stiffness "
          "predicts",
          "[fem][supports][run]") {
  const double L = 40.0, w = 8.0, delta = 0.02;
  Document doc;
  AddBox(doc, L, w, w);
  fem::Study& s = doc.AddStudy("Stretch");
  s.meshSize = 4.0;
  for (int a = 0; a < 3; ++a)
    s.loads.push_back(Support(doc, fem::LoadType::Roller, FlatAt(doc, a, 0.0)));
  s.loads.push_back(
      Held(doc, FlatAt(doc, 0, L), {true, false, false}, {delta, 0.0, 0.0}));
  const uint64_t end = s.loads.back().id;
  fem::StudyRuntime rt;
  const std::string err = Run(doc, s, rt);
  INFO(err);
  REQUIRE(err.empty());
  const fem::Material m = Steel();
  const auto& b = rt.bodies[0];
  const auto* r = ReactionOf(b.results, end);
  REQUIRE(r);
  const double expected = m.youngMPa * w * w * delta / L;
  INFO("reaction " << r->forceN.x << " N, E A delta / L " << expected);
  // The support pulls the bar out: it exerts +x on the part.
  CHECK_THAT(r->forceN.x, WithinRel(expected, 1e-6));
  CHECK_THAT(r->forceN.y, WithinAbs(0.0, 1e-6 * expected));
  // The whole field, node by node.
  double worst = 0.0;
  for (size_t i = 0; i < b.mesh.nodes.size(); ++i) {
    const glm::dvec3 x = b.mesh.nodes[i];
    const glm::dvec3 want(x.x * delta / L, -m.poisson * x.y * delta / L,
                          -m.poisson * x.z * delta / L);
    worst = std::max(worst, glm::length(b.results.displacement[i] - want));
  }
  CHECK(worst < 1e-6 * delta);
  CHECK_THAT(b.results.maxVonMises, WithinRel(m.youngMPa * delta / L, 1e-6));
}

// The bar of a cantilever, cut in half along its plane of symmetry and held
// there by a roller, bends as the whole bar does under half the load.
TEST_CASE("a half bar on a roller is the whole bar", "[fem][supports][run]") {
  const double L = 60.0, w = 10.0, F = 400.0;
  const auto tip = [&](double width, double force, bool half) {
    Document doc;
    AddBox(doc, L, width, w);
    fem::Study& s = doc.AddStudy("Cantilever");
    s.meshSize = 2.5;
    s.loads.push_back(Support(doc, fem::LoadType::Fixed, FlatAt(doc, 0, 0.0)));
    if (half)
      s.loads.push_back(
          Support(doc, fem::LoadType::Roller, FlatAt(doc, 1, width)));
    s.loads.push_back(
        Force(doc, FlatAt(doc, 0, L), force, glm::dvec3(0, 0, -1)));
    fem::StudyRuntime rt;
    const std::string err = Run(doc, s, rt);
    INFO(err);
    REQUIRE(err.empty());
    const auto& b = rt.bodies[0];
    const double uz = MeanOver(
        b, [&](const glm::dvec3& x) { return std::abs(x.x - L) < 1e-9; },
        [&](size_t i) { return b.results.displacement[i].z; });
    const auto* root = ReactionOf(b.results, s.loads[0].id);
    REQUIRE(root);
    return std::make_pair(uz, root->forceN.z);
  };
  const auto [wholeTip, wholeRoot] = tip(w, F, false);
  const auto [halfTip, halfRoot] = tip(w / 2, F / 2, true);
  INFO("tip " << wholeTip << " mm whole, " << halfTip << " mm half");
  CHECK_THAT(halfTip, WithinRel(wholeTip, 0.005));
  CHECK_THAT(halfRoot, WithinRel(F / 2, 1e-6));
  CHECK_THAT(wholeRoot, WithinRel(F, 1e-6));
}

// Lame's thick cylinder under inner pressure, a quarter of it on two
// symmetry planes and held in plane strain by rollers on its ends: the
// radial displacement at the bore is p a^2 (1 + nu) (b^2 / a + (1 - 2 nu)
// a) / (E (b^2 - a^2)) (Timoshenko & Goodier, Theory of Elasticity, s. 28).
TEST_CASE("a quarter of Lame's cylinder on rollers is the closed form",
          "[fem][supports][run]") {
  const double a = 10.0, bR = 20.0, h = 4.0, p = 10.0;
  Document doc;
  doc.AddFeature(std::make_unique<CylinderFeature>("Outer", glm::dvec3(0),
                                                   glm::dvec3(0, 0, 1), bR, h));
  const uint64_t outer = doc.GetFeature(0)->GetId();
  doc.AddFeature(std::make_unique<CylinderFeature>(
      "Bore", glm::dvec3(0, 0, -1), glm::dvec3(0, 0, 1), a, h + 2));
  const uint64_t bore = doc.GetFeature(1)->GetId();
  doc.AddFeature(std::make_unique<BooleanFeature>("Ring", outer, bore,
                                                  BooleanOp::Subtract));
  const uint64_t ring = doc.GetFeature(2)->GetId();
  doc.AddFeature(std::make_unique<BoxFeature>("Quadrant", glm::dvec3(0, 0, -1),
                                              2 * bR, 2 * bR, h + 2));
  const uint64_t quad = doc.GetFeature(3)->GetId();
  doc.AddFeature(std::make_unique<BooleanFeature>("Quarter", ring, quad,
                                                  BooleanOp::Intersect));
  doc.GetFeature(4)->SetMaterial("steel");
  doc.Regenerate();
  fem::Study& s = doc.AddStudy("Lame");
  s.meshSize = 2.0;
  s.loads.push_back(Support(doc, fem::LoadType::Roller, FlatAt(doc, 0, 0.0)));
  s.loads.push_back(Support(doc, fem::LoadType::Roller, FlatAt(doc, 1, 0.0)));
  s.loads.push_back(Support(doc, fem::LoadType::Roller, FlatAt(doc, 2, 0.0)));
  s.loads.push_back(Support(doc, fem::LoadType::Roller, FlatAt(doc, 2, h)));
  fem::Load pr =
      Support(doc, fem::LoadType::Pressure,
              FaceWhere(doc, [&](const Measure::FaceInfo& f) {
                return f.kind == "cylinder" && std::abs(f.radius - a) < 1e-6;
              }));
  pr.magnitude = p;
  s.loads.push_back(pr);
  fem::StudyRuntime rt;
  const std::string err = Run(doc, s, rt);
  INFO(err);
  REQUIRE(err.empty());
  const fem::Material m = Steel();
  const double nu = m.poisson, E = m.youngMPa;
  const double want = p * a * a * (1 + nu) * (bR * bR / a + (1 - 2 * nu) * a) /
                      (E * (bR * bR - a * a));
  const auto& b = rt.bodies[0];
  const double ur = MeanOver(
      b,
      [&](const glm::dvec3& x) {
        return std::abs(std::hypot(x.x, x.y) - a) < 1e-6;
      },
      [&](size_t i) {
        const glm::dvec3 x = b.mesh.nodes[i];
        const glm::dvec3 r = glm::normalize(glm::dvec3(x.x, x.y, 0.0));
        return glm::dot(b.results.displacement[i], r);
      });
  INFO("u_r at the bore " << ur << " mm, Lame " << want);
  CHECK_THAT(ur, WithinRel(want, 0.003));
  // Plane strain: nothing moves along the axis, but for the mesh's own
  // error (the exact field has none to give it).
  double uz = 0.0;
  for (const glm::dvec3& u : b.results.displacement)
    uz = std::max(uz, std::abs(u.z));
  CHECK(uz < 1e-3 * want);
}

// A round bar pulled along its axis, held at one end by a roller (its axial
// motion) and on its side by a cylindrical support that holds only the
// tangential direction (its turn and its slide across): free to stretch and
// to thin, so its end moves sigma L / E, whichever way the bar points. An
// inclined bar's roller is an inclined symmetry plane, and it reads what
// its axis-aligned image reads.
TEST_CASE("an inclined symmetry plane is its axis-aligned image",
          "[fem][supports][run]") {
  const double L = 50.0, r = 5.0, F = 1000.0;
  const auto stretch = [&](const glm::dvec3& dir) {
    Document doc;
    auto bar =
        std::make_unique<CylinderFeature>("Bar", glm::dvec3(0), dir, r, L);
    bar->SetMaterial("steel");
    doc.AddFeature(std::move(bar));
    const glm::dvec3 d = glm::normalize(dir);
    const auto cap = [&](double at) {
      return FaceWhere(doc, [&](const Measure::FaceInfo& f) {
        return f.kind == "plane" &&
               std::abs(glm::dot(f.centroid, d) - at) < 1e-6;
      });
    };
    fem::Study& s = doc.AddStudy("Pull");
    s.meshSize = 2.5;
    s.loads.push_back(Support(doc, fem::LoadType::Roller, cap(0.0)));
    fem::Load side = Support(doc, fem::LoadType::Cylindrical,
                             FaceWhere(doc, [](const Measure::FaceInfo& f) {
                               return f.kind == "cylinder";
                             }));
    side.hold = {false, true, false};
    s.loads.push_back(side);
    s.loads.push_back(Force(doc, cap(L), F, d));
    fem::StudyRuntime rt;
    const std::string err = Run(doc, s, rt);
    INFO(err);
    REQUIRE(err.empty());
    const auto& b = rt.bodies[0];
    const double end = MeanOver(
        b,
        [&](const glm::dvec3& x) {
          return std::abs(glm::dot(x, d) - L) < 1e-6;
        },
        [&](size_t i) { return glm::dot(b.results.displacement[i], d); });
    const auto* roller = ReactionOf(b.results, s.loads[0].id);
    REQUIRE(roller);
    return std::make_pair(end, glm::dot(roller->forceN, d));
  };
  const double sigma = F / (3.14159265358979323846 * r * r);
  const double want = sigma * L / Steel().youngMPa;
  const auto [aligned, alignedR] = stretch(glm::dvec3(1, 0, 0));
  const auto [inclined, inclinedR] =
      stretch(glm::dvec3(std::cos(0.5), std::sin(0.5), 0.3));
  INFO("end " << aligned << " mm aligned, " << inclined << " mm inclined, "
              << want << " mm sigma L / E");
  // The disc a cylinder's cap meshes to is not the circle, so the stress
  // is the force over the meshed area: within a part in a thousand.
  CHECK_THAT(aligned, WithinRel(want, 2e-3));
  CHECK_THAT(inclined, WithinRel(aligned, 1e-3));
  CHECK_THAT(alignedR, WithinRel(-F, 1e-6));
  CHECK_THAT(inclinedR, WithinRel(-F, 1e-6));
}

// A cantilever's first bending mode, across its depth, is symmetric about
// the plane through its middle: the half bar on a roller there has it as
// its first mode, and the whole bar's sideways twin is held out.
TEST_CASE("a mode survives halving on a roller", "[fem][supports][run]") {
  const double L = 80.0, w = 8.0;
  const auto first = [&](double width, bool half) {
    Document doc;
    AddBox(doc, L, width, w);
    fem::Study& s = doc.AddStudy("Modes");
    s.type = fem::AnalysisType::Modal;
    s.numModes = 2;
    s.meshSize = 2.5;
    s.loads.push_back(Support(doc, fem::LoadType::Fixed, FlatAt(doc, 0, 0.0)));
    if (half)
      s.loads.push_back(
          Support(doc, fem::LoadType::Roller, FlatAt(doc, 1, width)));
    fem::StudyRuntime rt;
    const std::string err = Run(doc, s, rt);
    INFO(err);
    REQUIRE(err.empty());
    const auto& modes = rt.bodies[0].results.modes;
    REQUIRE(modes.size() >= 2);
    return std::make_pair(modes[0].frequencyHz, modes[1].frequencyHz);
  };
  const auto [whole0, whole1] = first(w, false);
  const auto [half0, half1] = first(w / 2, true);
  INFO("whole " << whole0 << ", " << whole1 << " Hz; half " << half0 << ", "
                << half1 << " Hz");
  // The square bar's two first modes are one frequency.
  CHECK_THAT(whole1, WithinRel(whole0, 0.01));
  CHECK_THAT(half0, WithinRel(whole0, 0.01));
  // The half bar's second is the whole bar's sideways one only were the
  // roller absent: held there, it is a higher mode.
  CHECK(half1 > 2.0 * half0);
}

// The refusals, each naming what is accepted.
TEST_CASE("roller, held and cylindrical supports refuse what they cannot "
          "mean",
          "[fem][supports][run]") {
  Document doc;
  AddBox(doc, 40.0, 10.0, 10.0);
  const ElementRef x0 = FlatAt(doc, 0, 0.0), x1 = FlatAt(doc, 0, 40.0),
                   y1 = FlatAt(doc, 1, 10.0);
  const auto refusal = [&](fem::AnalysisType type,
                           const std::vector<fem::Load>& loads) {
    fem::Study& s = doc.AddStudy("Refused");
    s.type = type;
    s.meshSize = 4.0;
    s.loads = loads;
    fem::StudyRuntime rt;
    const std::string err = Run(doc, s, rt);
    doc.RemoveStudy(s.id);
    return err;
  };
  const auto fixed = Support(doc, fem::LoadType::Fixed, x0);
  SECTION("a held support that holds nothing") {
    CHECK_THAT(refusal(fem::AnalysisType::Static,
                       {fixed, Held(doc, x1, {false, false, false})}),
               ContainsSubstring("holds no direction"));
  }
  SECTION("a cylindrical support off a cylinder") {
    fem::Load c = Support(doc, fem::LoadType::Cylindrical, x1);
    c.hold = {true, false, false};
    CHECK_THAT(refusal(fem::AnalysisType::Static, {fixed, c}),
               ContainsSubstring("goes on a cylindrical face"));
  }
  SECTION("a prescribed displacement on a modal or buckling study") {
    const auto moved = Held(doc, x1, {true, false, false}, {0.1, 0.0, 0.0});
    CHECK_THAT(refusal(fem::AnalysisType::Modal, {fixed, moved}),
               ContainsSubstring("a static study's"));
    CHECK_THAT(refusal(fem::AnalysisType::Buckling, {fixed, moved}),
               ContainsSubstring("a static study's"));
  }
  SECTION("a displacement on a direction not held") {
    fem::Load h = Held(doc, x1, {true, false, false});
    h.prescribedMm[1] = 0.2;
    CHECK_THAT(refusal(fem::AnalysisType::Static, {fixed, h}),
               ContainsSubstring("y is not held"));
  }
  SECTION("a roller with directions or a frame") {
    fem::Load r = Support(doc, fem::LoadType::Roller, x1);
    r.hold = {true, false, false};
    CHECK_THAT(refusal(fem::AnalysisType::Static, {fixed, r}),
               ContainsSubstring("takes no hold"));
  }
  SECTION("any of them on a topology study") {
    CHECK_THAT(refusal(fem::AnalysisType::Topology,
                       {fixed, Support(doc, fem::LoadType::Roller, x1),
                        Force(doc, y1, 10.0, glm::dvec3(0, -1, 0))}),
               ContainsSubstring("topology study's supports are fixed"));
  }
  SECTION("two held faces that meet at displacements that disagree") {
    const std::string why =
        refusal(fem::AnalysisType::Static,
                {Held(doc, x1, {true, false, false}, {0.1, 0.0, 0.0}),
                 Held(doc, y1, {true, false, false}, {0.2, 0.0, 0.0})});
    CHECK_THAT(why, ContainsSubstring("displacements that disagree"));
    CHECK_THAT(why, ContainsSubstring("held support"));
  }
  SECTION("a held face that moves a fixed face's nodes") {
    CHECK_THAT(
        refusal(fem::AnalysisType::Static,
                {fixed, Held(doc, y1, {true, false, false}, {0.1, 0.0, 0.0})}),
        ContainsSubstring("holds where it is"));
  }
  SECTION("a frame that is not a datum") {
    fem::Load h = Held(doc, x1, {true, false, false});
    h.frame = doc.GetFeature(0)->GetId(); // the box: no frame
    CHECK_THAT(refusal(fem::AnalysisType::Static, {fixed, h}),
               ContainsSubstring("is no Frame"));
  }
  SECTION("rollers alone that leave the part free to slide") {
    CHECK_THAT(refusal(fem::AnalysisType::Static,
                       {Support(doc, fem::LoadType::Roller, x0),
                        Force(doc, x1, 10.0, glm::dvec3(1, 0, 0))}),
               ContainsSubstring("free to"));
  }
}

// Held in a datum's frame: a Frame turned about z holds its own x, and the
// bar's end moves along it.
TEST_CASE("a held support takes a datum's axes", "[fem][supports][run]") {
  const double L = 30.0, w = 6.0, delta = 0.01;
  Document doc;
  AddBox(doc, L, w, w);
  auto frame = std::make_unique<FrameFeature>("Turned");
  const double c = std::cos(0.4), sn = std::sin(0.4);
  frame->xAxis = glm::dvec3(c, sn, 0.0);
  frame->zAxis = glm::dvec3(0.0, 0.0, 1.0);
  doc.AddFeature(std::move(frame));
  const uint64_t frameId = doc.GetFeature(1)->GetId();
  fem::Study& s = doc.AddStudy("Turned hold");
  s.meshSize = 3.0;
  s.loads.push_back(Support(doc, fem::LoadType::Fixed, FlatAt(doc, 0, 0.0)));
  fem::Load end =
      Held(doc, FlatAt(doc, 0, L), {true, true, true}, {delta, 0.0, 0.0});
  end.frame = frameId;
  s.loads.push_back(end);
  fem::StudyRuntime rt;
  const std::string err = Run(doc, s, rt);
  INFO(err);
  REQUIRE(err.empty());
  const auto& b = rt.bodies[0];
  for (size_t i = 0; i < b.mesh.nodes.size(); ++i)
    if (std::abs(b.mesh.nodes[i].x - L) < 1e-9) {
      const glm::dvec3 u = b.results.displacement[i];
      CHECK_THAT(u.x, WithinAbs(delta * c, 1e-12));
      CHECK_THAT(u.y, WithinAbs(delta * sn, 1e-12));
      CHECK_THAT(u.z, WithinAbs(0.0, 1e-12));
    }
}

#endif // HAVE_NETGEN

// What a file and the expression walk keep: the directions, a held
// support's displacements as expressions of the variables, and its frame.
TEST_CASE("held and cylindrical supports save, reopen and take expressions",
          "[fem][supports]") {
  Document doc;
  AddBox(doc, 20.0, 10.0, 10.0);
  REQUIRE(doc.SetParameter("lift", "0.125"));
  fem::Study& s = doc.AddStudy("Saved");
  fem::Load h;
  h.id = 7;
  h.type = fem::LoadType::Held;
  h.face = ElementRef{doc.GetFeature(0)->GetId(), "x"};
  h.hold = {false, true, true};
  h.prescribedMm[2].expr = "lift * 2";
  h.frame = 0;
  s.loads.push_back(h);
  fem::Load c = h;
  c.id = 8;
  c.type = fem::LoadType::Cylindrical;
  c.hold = {true, false, false};
  c.prescribedMm = {};
  s.loads.push_back(c);
  std::string err;
  REQUIRE(fem::EvaluateStudyExprs(s, doc.Symbols(), &err));
  CHECK(s.loads[0].prescribedMm[2].value == 0.25);
  const auto j = DocumentIO::StudyToJson(s);
  fem::Study back = DocumentIO::StudyFromJson(j);
  REQUIRE(back.loads.size() == 2);
  CHECK(back.loads[0].type == fem::LoadType::Held);
  CHECK(back.loads[0].hold == std::array<bool, 3>{false, true, true});
  CHECK(back.loads[0].prescribedMm[2].expr == "lift * 2");
  CHECK(back.loads[0].prescribedMm[2].value == 0.25);
  CHECK(back.loads[1].type == fem::LoadType::Cylindrical);
  CHECK(back.loads[1].hold == std::array<bool, 3>{true, false, false});
  // A key another type takes is refused, not dropped.
  auto bad = j;
  bad["loads"][1]["dx_mm"] = 0.1;
  CHECK_THROWS_WITH(DocumentIO::StudyFromJson(bad),
                    ContainsSubstring("is a held support's"));
  bad = j;
  bad["loads"][0]["hold"] = {"radial"};
  CHECK_THROWS_WITH(DocumentIO::StudyFromJson(bad),
                    ContainsSubstring("holds x, y or z"));
}
