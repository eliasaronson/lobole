// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

// The geometry step of a simulation (docs/simulation/README.md
// #the-geometry-step): features suppressed for the analysis only, the
// symmetry cut and the geometry check, each through the simulation's one
// planner and its real run.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "core/Document.h"
#include "core/Feature.h"
#include "core/Measure.h"
#include "core/Occurrence.h"
#include "core/features/DatumPlaneFeature.h"
#include "core/features/ExtrudeFeature.h"
#include "core/features/HoleFeature.h"
#include "core/features/SketchFeature.h"
#include "core/fem/FemSolver.h"
#include "core/fem/SimDocument.h"
#include "core/fem/SimGeometry.h"
#include "core/fem/SimStudy.h"
#include "core/io/SerializeSimulation.h"

#include <glm/gtc/matrix_transform.hpp>
#include <nlohmann/json.hpp>

#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <string>

using Catch::Matchers::ContainsSubstring;

namespace {

// A face of the first body whose outward normal is `n`, and whose name
// contains `has` when given.
ElementRef FaceFacing(const Document& doc, const glm::dvec3& n,
                      const std::string& has = "") {
  const Body& body = doc.GetBodies().at(0);
  for (const auto& [index, name] : body.elementMap.names)
    if (const auto info = Measure::DescribeFaceAt(body, index))
      if (info->kind == "plane" && glm::dot(info->normal, n) > 0.999 &&
          (has.empty() || name.find(has) != std::string::npos))
        return ElementRef{body.createdBy, name};
  return {};
}

ElementRef NamedFace(const Document& doc, const std::string& has) {
  const Body& body = doc.GetBodies().at(0);
  for (const auto& [index, name] : body.elementMap.names)
    if (name.find(has) != std::string::npos)
      return ElementRef{body.createdBy, name};
  return {};
}

// A 160 x 60 x 6 steel plate, its top at z = 6, with a row of `holes`
// through-holes of `dia` mm (one Hole feature) along its middle.
struct Plate {
  Document doc;
  uint64_t holesId = 0;
  Plate(int holes = 6, double dia = 4.0) {
    auto pad =
        std::make_unique<SketchFeature>("Plate", SketchPlane(PlaneType::XY));
    pad->AddRectangle({0, 0}, {160, 60});
    doc.AddFeature(std::move(pad));
    auto extrude = std::make_unique<ExtrudeFeature>(
        "Pad", doc.GetFeature(0)->GetId(), 6.0, ExtrudeMode::NewBody);
    extrude->SetMaterial("steel");
    doc.AddFeature(std::move(extrude));
    auto centres = std::make_unique<SketchFeature>(
        "Centres", SketchPlane(glm::dvec3(0, 0, 6), glm::dvec3(0, 0, 1),
                               glm::dvec3(1, 0, 0)));
    for (int i = 0; i < holes; ++i)
      centres->AddEntity(
          sketch::Point{{static_cast<float>(25.0 + 22.0 * i), 30.0f}, i + 1});
    doc.AddFeature(std::move(centres));
    const uint64_t at = doc.GetFeature(doc.FeatureCount() - 1)->GetId();
    auto hole = std::make_unique<HoleFeature>("Fastener holes", at,
                                              HoleKind::Simple, dia, 6.0);
    hole->through = true;
    doc.AddFeature(std::move(hole));
    REQUIRE(doc.Regenerate());
    holesId = doc.GetFeature(doc.FeatureCount() - 1)->GetId();
    REQUIRE(doc.GetFeature(doc.FeatureCount() - 1)->GetError().empty());
  }
  // The plate clamped at x = 0 and pushed down at x = 160.
  fem::Study Bend() const {
    fem::Study s;
    s.name = "Bend";
    s.converge = fem::ConvergeMode::Never;
    fem::Load fixed;
    fixed.id = 101;
    fixed.type = fem::LoadType::Fixed;
    fixed.face = FaceFacing(doc, {-1, 0, 0});
    s.loads.push_back(fixed);
    fem::Load force;
    force.id = 102;
    force.type = fem::LoadType::Force;
    force.face = FaceFacing(doc, {1, 0, 0});
    force.magnitude = 200.0;
    force.direction = glm::dvec3(0, 0, -1);
    s.loads.push_back(force);
    return s;
  }
};

uint64_t AddStudy(fem::SimDocument& sim, fem::Study setup) {
  const uint64_t id = sim.AddStudy(setup.name).id;
  setup.id = id;
  REQUIRE(sim.ReplaceStudy(std::move(setup)));
  return id;
}

fem::SimDocument PartSim(double meshSize) {
  fem::SimDocument sim;
  sim.SetSubject({fem::SubjectKind::Part, 1, 0, "Plate"});
  sim.SetMesh({ExprValue{meshSize}, {}});
  return sim;
}

fem::SimContext Context(const Document& doc) {
  fem::SimContext ctx;
  ctx.part = &doc;
  ctx.subjectStamps = {{1, 7}};
  ctx.analysisParts = std::make_shared<fem::AnalysisPartCache>();
  return ctx;
}

size_t Tets(const std::vector<fem::StudyBodyMesh>& meshes) {
  size_t n = 0;
  for (const fem::StudyBodyMesh& m : meshes)
    n += m.mesh.tets.size();
  return n;
}

double Volume(const Document& doc) {
  double v = 0.0;
  for (const Body& b : doc.GetBodies()) {
    GProp_GProps props;
    BRepGProp::VolumeProperties(b.shape.GetShape(), props);
    v += props.Mass();
  }
  return v;
}

} // namespace

TEST_CASE("Suppressed fastener holes mesh the plate to a fraction of the "
          "elements and leave the part as it is",
          "[fem][simdoc][simgeom]") {
#ifndef HAVE_NETGEN
  SKIP("built without Netgen: nothing here meshes");
#endif
  Plate plate;
  const double volumeBefore = Volume(plate.doc);
  const uint64_t versionBefore = plate.doc.Version();
  // The automatic size: the holes, far smaller than it, are what the mesh
  // spends its elements on.
  fem::SimDocument sim = PartSim(0.0);
  const uint64_t id = AddStudy(sim, plate.Bend());
  const fem::SimContext ctx = Context(plate.doc);
  std::vector<fem::StudyBodyMesh> withHoles, without;
  std::string err;
  const bool meshedWith = fem::MeshSimStudy(sim, ctx, id, &withHoles, &err);
  INFO(err);
  REQUIRE(meshedWith);

  fem::SimGeometry g;
  g.suppressed.push_back({0, plate.holesId});
  sim.SetGeometry(g);
  const bool meshedWithout = fem::MeshSimStudy(sim, ctx, id, &without, &err);
  INFO(err);
  REQUIRE(meshedWithout);
  const size_t a = Tets(withHoles), b = Tets(without);
  INFO("with holes " << a << " tets, suppressed " << b);
  // Measured: 538 against 6764, a twelfth. Stated with room: under a fifth.
  CHECK(b * 5 < a);

  // The part: still holed, its feature not suppressed, not edited.
  CHECK(std::abs(Volume(plate.doc) - volumeBefore) < 1e-6 * volumeBefore);
  CHECK(plate.doc.Version() == versionBefore);
  size_t index = 0;
  const Feature* holes = plate.doc.FindFeatureById(plate.holesId, &index);
  REQUIRE(holes != nullptr);
  CHECK_FALSE(plate.doc.EffectiveSuppressed(index));
  // ...and the analysis part is the plate without them.
  std::string why;
  const auto analysis = fem::AnalysisPart(plate.doc, {plate.holesId}, &why);
  REQUIRE(analysis != nullptr);
  CHECK(Volume(*analysis) > volumeBefore);
  CHECK(std::abs(Volume(*analysis) - 160.0 * 60.0 * 6.0) < 1.0);
}

TEST_CASE("A load on a suppressed feature's face is refused naming it",
          "[fem][simdoc][simgeom]") {
  Plate plate;
  fem::SimDocument sim = PartSim(6.0);
  fem::Study s = plate.Bend();
  fem::Load bore;
  bore.id = 103;
  bore.type = fem::LoadType::Pressure;
  bore.face = NamedFace(plate.doc, "bore");
  REQUIRE(bore.face.IsSet());
  bore.magnitude = 1.0;
  s.loads.push_back(bore);
  const uint64_t id = AddStudy(sim, s);
  fem::SimGeometry g;
  g.suppressed.push_back({0, plate.holesId});
  sim.SetGeometry(g);
  fem::SimContext ctx = Context(plate.doc);
  ctx.sim = &sim;
  fem::StudyPlan plan;
  std::string err;
  CHECK_FALSE(fem::PlanSimStudy(ctx, id, plan, &err));
  CHECK_THAT(err, ContainsSubstring("load 103"));
  CHECK_THAT(err, ContainsSubstring("'Fastener holes' made"));
  CHECK_THAT(err, ContainsSubstring("suppressed for the analysis"));
  CHECK(fem::SimStudyInputKey(ctx, id) == 0);
  // The check says it as the run would.
  const auto problems = fem::CheckSimStudy(ctx, id);
  REQUIRE_FALSE(problems.empty());
  CHECK_THAT(problems.front().message, ContainsSubstring("'Fastener holes'"));

  // Without that load, a force on the top face the holes went through reads
  // the face whole again.
  fem::Study top = plate.Bend();
  top.loads[1].face = FaceFacing(plate.doc, {0, 0, 1});
  REQUIRE(top.loads[1].face.name.find(";M") != std::string::npos);
  top.id = id;
  REQUIRE(sim.ReplaceStudy(top));
  CHECK(fem::PlanSimStudy(ctx, id, plan, &err));
  CHECK(fem::SimStudyInputKey(ctx, id) != 0);
}

TEST_CASE("Suppression, a feature it breaks and the input hash",
          "[fem][simdoc][simgeom]") {
#ifndef HAVE_NETGEN
  SKIP("built without Netgen: nothing here meshes");
#endif
  Plate plate(2);
  std::string err;
  CHECK(fem::AnalysisPart(plate.doc, {9999}, &err) == nullptr);
  CHECK_THAT(err, ContainsSubstring("no feature 9999"));
  // The pad is what everything stands on: nothing would be left.
  const uint64_t pad = plate.doc.GetFeature(1)->GetId();
  CHECK(fem::AnalysisPart(plate.doc, {pad}, &err) == nullptr);
  CHECK_THAT(err, ContainsSubstring("'Pad'"));

  fem::SimDocument sim = PartSim(6.0);
  const uint64_t id = AddStudy(sim, plate.Bend());
  fem::SimContext ctx = Context(plate.doc);
  ctx.sim = &sim;
  fem::StudyRuntime rt;
  const bool ran = fem::RunSimStudy(sim, ctx, id, rt, &err);
  INFO(err);
  REQUIRE(ran);
  const uint64_t before = fem::SimStudyInputKey(ctx, id);
  REQUIRE(before != 0);
  CHECK_FALSE(sim.StudyResultsStale(id, [](uint64_t) { return 7; }));
  fem::SimGeometry g;
  g.suppressed.push_back({0, plate.holesId});
  sim.SetGeometry(g);
  // Every result reads stale, and the input moved.
  CHECK(sim.StudyResultsStale(id, [](uint64_t) { return 7; }));
  CHECK(fem::SimStudyInputKey(ctx, id) != before);
  sim.SetGeometry({});
  CHECK(fem::SimStudyInputKey(ctx, id) == before);
}

namespace {

// An 80 x 10 x 10 steel bar along x, its end at x = 0 clamped and the
// other pushed down, with a datum plane through its middle at y = 5.
struct Bar {
  Document doc;
  uint64_t datumId = 0;
  Bar(double datumAt = 5.0) {
    auto box = std::make_unique<BoxFeature>("Bar", glm::vec3(0, 0, 0), 80.0,
                                            10.0, 10.0);
    box->SetMaterial("steel");
    doc.AddFeature(std::move(box));
    auto dp =
        std::make_unique<DatumPlaneFeature>("Middle", DatumPlaneKind::Offset);
    dp->basePlane = PlaneType::XZ;
    dp->offset = datumAt;
    doc.AddFeature(std::move(dp));
    REQUIRE(doc.Regenerate());
    datumId = doc.GetFeature(1)->GetId();
    const auto* d = dynamic_cast<const DatumPlaneFeature*>(doc.GetFeature(1));
    REQUIRE(d != nullptr);
    REQUIRE(std::abs(std::abs(d->OutputPlane().GetNormal().y) - 1.0) < 1e-9);
  }
  fem::Study Bend(fem::AnalysisType type = fem::AnalysisType::Static) const {
    fem::Study s;
    s.name = "Bend";
    s.type = type;
    s.converge = fem::ConvergeMode::Never;
    fem::Load fixed;
    fixed.id = 101;
    fixed.type = fem::LoadType::Fixed;
    fixed.face = FaceFacing(doc, {-1, 0, 0});
    s.loads.push_back(fixed);
    if (type == fem::AnalysisType::Static) {
      fem::Load force;
      force.id = 102;
      force.type = fem::LoadType::Force;
      force.face = FaceFacing(doc, {1, 0, 0});
      force.magnitude = 500.0;
      force.direction = glm::dvec3(0, 0, -1);
      s.loads.push_back(force);
    }
    return s;
  }
};

double Reaction(const fem::StudyRuntime& rt, uint64_t support) {
  double z = 0.0;
  for (const fem::StudyBodyResult& b : rt.bodies)
    for (const auto& r : b.results.reactions)
      if (r.support == support)
        z += r.forceN.z;
  return z;
}

} // namespace

TEST_CASE("A half model on a symmetry plane answers as the whole",
          "[fem][simdoc][simgeom]") {
#ifndef HAVE_NETGEN
  SKIP("built without Netgen: nothing here meshes");
#endif
  Bar bar;
  std::string err;
  // The whole bar.
  fem::SimDocument whole = PartSim(2.5);
  const uint64_t wid = AddStudy(whole, bar.Bend());
  fem::StudyRuntime full;
  const bool ranWhole =
      fem::RunSimStudy(whole, Context(bar.doc), wid, full, &err);
  INFO(err);
  REQUIRE(ranWhole);
  // Half of it, cut on the datum, a roller on the cut.
  fem::SimDocument half = PartSim(2.5);
  const uint64_t hid = AddStudy(half, bar.Bend());
  fem::SimGeometry g;
  g.symmetry.datum = bar.datumId;
  half.SetGeometry(g);
  fem::StudyRuntime cut;
  const bool ranHalf = fem::RunSimStudy(half, Context(bar.doc), hid, cut, &err);
  INFO(err);
  REQUIRE(ranHalf);
  INFO("whole: " << full.maxDisplacement << " mm, " << full.maxVonMises
                 << " MPa; half: " << cut.maxDisplacement << " mm, "
                 << cut.maxVonMises << " MPa");
  // The answer the half model is for: the same deflection and stress.
  // Measured within 0.01 % and 0.4 %; stated within 1 % and 3 %.
  CHECK(std::abs(cut.maxDisplacement - full.maxDisplacement) <
        0.01 * full.maxDisplacement);
  CHECK(std::abs(cut.maxVonMises - full.maxVonMises) < 0.03 * full.maxVonMises);
  // It solved half the bar: about half the elements, half the clamp's
  // reaction against half the force.
  const size_t fullTets = full.bodies.at(0).mesh.stats.tetCount;
  const size_t halfTets = cut.bodies.at(0).mesh.stats.tetCount;
  CHECK(halfTets < 0.6 * fullTets); // measured 1458 against 2965
  CHECK(std::abs(Reaction(full, 101) - 500.0) < 5.0);
  CHECK(std::abs(Reaction(cut, 101) - 250.0) < 5.0);
  // The cut is an input: it hashes.
  CHECK(fem::SimStudyInputKey(
            [&] {
              fem::SimContext c = Context(bar.doc);
              c.sim = &half;
              return c;
            }(),
            hid) != 0);
}

TEST_CASE("A symmetry cut refuses what a half model cannot be",
          "[fem][simdoc][simgeom]") {
  std::string err;
  fem::StudyPlan plan;
  {
    // A modal study finds only the symmetric modes.
    Bar bar;
    fem::SimDocument sim = PartSim(5.0);
    const uint64_t id = AddStudy(sim, bar.Bend(fem::AnalysisType::Modal));
    fem::SimGeometry g;
    g.symmetry.datum = bar.datumId;
    sim.SetGeometry(g);
    fem::SimContext ctx = Context(bar.doc);
    ctx.sim = &sim;
    CHECK_FALSE(fem::PlanSimStudy(ctx, id, plan, &err));
    CHECK_THAT(err, ContainsSubstring("static or a thermal study's"));
  }
  {
    // A plane that does not halve the bar: it is not symmetric about it.
    Bar bar(3.0);
    fem::SimDocument sim = PartSim(5.0);
    const uint64_t id = AddStudy(sim, bar.Bend());
    fem::SimGeometry g;
    g.symmetry.datum = bar.datumId;
    sim.SetGeometry(g);
    fem::SimContext ctx = Context(bar.doc);
    ctx.sim = &sim;
    CHECK_FALSE(fem::PlanSimStudy(ctx, id, plan, &err));
    CHECK_THAT(err, ContainsSubstring("does not halve"));
    CHECK_THAT(err, ContainsSubstring("70.0 %"));
    // Flipped, the other side: 30 %.
    g.symmetry.flip = true;
    sim.SetGeometry(g);
    CHECK_FALSE(fem::PlanSimStudy(ctx, id, plan, &err));
    CHECK_THAT(err, ContainsSubstring("30.0 %"));
  }
  {
    // A torque is no mirror image of itself.
    Bar bar;
    fem::SimDocument sim = PartSim(5.0);
    fem::Study s = bar.Bend();
    s.loads[1].type = fem::LoadType::Torque;
    s.loads[1].axisDirection = glm::dvec3(1, 0, 0);
    s.loads[1].axisOrigin = glm::dvec3(80, 5, 5);
    const uint64_t id = AddStudy(sim, s);
    fem::SimGeometry g;
    g.symmetry.datum = bar.datumId;
    sim.SetGeometry(g);
    fem::SimContext ctx = Context(bar.doc);
    ctx.sim = &sim;
    CHECK_FALSE(fem::PlanSimStudy(ctx, id, plan, &err));
    CHECK_THAT(err, ContainsSubstring("load 102 (torque)"));
  }
  {
    // A feature that is no datum plane.
    Bar bar;
    fem::SimDocument sim = PartSim(5.0);
    const uint64_t id = AddStudy(sim, bar.Bend());
    fem::SimGeometry g;
    g.symmetry.datum = bar.doc.GetFeature(0)->GetId();
    sim.SetGeometry(g);
    fem::SimContext ctx = Context(bar.doc);
    ctx.sim = &sim;
    CHECK_FALSE(fem::PlanSimStudy(ctx, id, plan, &err));
    CHECK_THAT(err, ContainsSubstring("no datum plane"));
  }
}

TEST_CASE("The geometry check names what is smaller than the elements with "
          "its feature",
          "[fem][simdoc][simgeom]") {
  // Holes of 3 mm in a plate meshed at 8: their bores are narrower than an
  // element and their rims shorter than... not quite: 9.4 mm. The bores are
  // found, and the ligament between two close holes is a wall.
  Plate plate(6, 3.0);
  fem::SimDocument sim = PartSim(8.0);
  const uint64_t id = AddStudy(sim, plate.Bend());
  fem::SimContext ctx = Context(plate.doc);
  ctx.sim = &sim;
  fem::GeometryCheck check;
  std::string err;
  REQUIRE(fem::CheckSimGeometry(ctx, id, check, &err));
  REQUIRE(check.bodies.size() == 1);
  CHECK(check.bodies[0].elementMm == 8.0);
  int bores = 0, walls = 0;
  std::string said;
  for (const fem::GeometryFinding& f : check.findings)
    said += std::string(fem::GeometryFindingKindName(f.kind)) + " " +
            f.element + " " + std::to_string(f.sizeMm) + " by '" +
            f.featureName + "'\n";
  INFO(said);
  for (const fem::GeometryFinding& f : check.findings) {
    if (f.kind == fem::GeometryFinding::Kind::Face &&
        f.element.find("bore") != std::string::npos) {
      ++bores;
      CHECK(f.featureName == "Fastener holes");
      CHECK(f.feature == plate.holesId);
      CHECK(f.suppressible);
      CHECK(f.sizeMm < 8.0);
    }
    if (f.kind == fem::GeometryFinding::Kind::Gap && f.throughMaterial)
      ++walls;
  }
  CHECK(bores == 6);
  // The 6 mm plate under 8 mm elements: its top and bottom are a wall.
  CHECK(walls >= 1);
  // Suppressed, the holes are not there to find.
  fem::SimGeometry g;
  g.suppressed.push_back({0, plate.holesId});
  sim.SetGeometry(g);
  REQUIRE(fem::CheckSimGeometry(ctx, id, check, &err));
  for (const fem::GeometryFinding& f : check.findings)
    CHECK(f.featureName != "Fastener holes");
}

TEST_CASE("The geometry step is saved and read back, and refused when it "
          "does not read",
          "[fem][simdoc][simgeom]") {
  fem::SimDocument sim = PartSim(4.0);
  fem::SimGeometry g;
  g.suppressed.push_back({0, 5});
  g.symmetry.face = ElementRef{1, "1/face2"};
  g.symmetry.flip = true;
  sim.SetGeometry(g);
  const auto tmp =
      std::filesystem::temp_directory_path() / "sim_geometry_rt.json";
  REQUIRE(DocumentIO::SaveSimulation(sim, tmp.string()).has_value());
  auto back = DocumentIO::LoadSimulation(tmp.string());
  REQUIRE(back.has_value());
  CHECK((*back)->Geometry() == g);

  nlohmann::json j;
  std::ifstream(tmp) >> j;
  const auto refused = [&](const std::function<void(nlohmann::json&)>& edit,
                           const std::string& says) {
    nlohmann::json bad = j;
    edit(bad);
    std::ofstream(tmp) << bad.dump();
    const auto r = DocumentIO::LoadSimulation(tmp.string());
    REQUIRE_FALSE(r.has_value());
    CHECK_THAT(r.error(), ContainsSubstring(says));
  };
  refused(
      [](nlohmann::json& b) { b["geometry"]["suppressed"][0]["document"] = 3; },
      "with no `document`");
  refused(
      [](nlohmann::json& b) {
        b["geometry"]["suppressed"].push_back({{"feature", 5}});
      },
      "listed twice");
  refused([](nlohmann::json& b) { b["geometry"]["symmetry"]["datum"] = 4; },
          "exactly one of `datum` and `face`");
  refused([](nlohmann::json& b) { b["geometry"]["mirror"] = true; }, "mirror");
  std::filesystem::remove(tmp);
}

TEST_CASE("An assembly's lone part takes its suppression, and one beside a "
          "neighbour refuses it",
          "[fem][simdoc][simgeom]") {
  Plate plate(2);
  AssemblyDocument asmDoc;
  Occurrence o;
  o.name = "Plate";
  o.partId = 1;
  o.fixed = true;
  const uint64_t occ = asmDoc.AddOccurrence(o);
  Occurrence other = o;
  other.name = "Stand";
  other.position = glm::vec3(0.0f, 0.0f, -50.0f);
  const uint64_t stand = asmDoc.AddOccurrence(other);
  fem::SimContext ctx;
  ctx.assembly = &asmDoc;
  ctx.placedBodies = [&](uint64_t id) {
    std::vector<Body> out = plate.doc.GetBodies();
    if (id == stand)
      for (Body& b : out)
        b.shape = b.shape.Transformed(
            glm::translate(glm::dmat4(1.0), glm::dvec3(0, 0, -50)));
    return out;
  };
  ctx.partOf = [&](uint64_t) {
    return fem::SimPart{&plate.doc, glm::dmat4(1.0), 1};
  };
  ctx.analysisParts = std::make_shared<fem::AnalysisPartCache>();

  fem::SimDocument sim;
  sim.SetSubject({fem::SubjectKind::Assembly, 9, 0, "Rig"});
  sim.SetMesh({ExprValue{8.0}, {}});
  sim.SetRoles({{occ, fem::ParticipantRole::Meshed, ExprValue{0.0}}});
  fem::Study s = plate.Bend();
  for (fem::Load& l : s.loads)
    l.occurrenceId = occ;
  const uint64_t id = AddStudy(sim, s);
  fem::SimGeometry g;
  g.suppressed.push_back({1, plate.holesId});
  sim.SetGeometry(g);
  ctx.sim = &sim;
  // Planned as the part: the plate without its holes.
  fem::StudyPlan plan;
  std::string err;
  INFO(err);
  REQUIRE(fem::PlanSimStudy(ctx, id, plan, &err));
  REQUIRE(plan.bodies.size() == 1);
  GProp_GProps props;
  BRepGProp::VolumeProperties(plan.bodies[0].shape, props);
  CHECK(std::abs(props.Mass() - 160.0 * 60.0 * 6.0) < 1.0);
  // Beside a rigid neighbour the assembly planner places the parts as they
  // are: refused by name, not planned with the holes.
  sim.SetRoles({{occ, fem::ParticipantRole::Meshed, ExprValue{0.0}},
                {stand, fem::ParticipantRole::Rigid, ExprValue{0.0}}});
  CHECK_FALSE(fem::PlanSimStudy(ctx, id, plan, &err));
  CHECK_THAT(err, ContainsSubstring("suppressed for the analysis are a part "
                                    "study's"));
  // A symmetry plane is a part simulation's.
  g = {};
  g.symmetry.datum = 3;
  sim.SetRoles({{occ, fem::ParticipantRole::Meshed, ExprValue{0.0}}});
  CHECK_THAT(
      fem::SimGeometryRefusal(ctx, g, [&](uint64_t) { return &plate.doc; }),
      ContainsSubstring("part simulation's"));
}
