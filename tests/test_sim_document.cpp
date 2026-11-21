// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

// The simulation document in core and its one planner
// (docs/simulation/README.md#the-simulation-document-core).

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "core/Document.h"
#include "core/Feature.h"
#include "core/Measure.h"
#include "core/Occurrence.h"
#include "core/fem/FemSolver.h"
#include "core/fem/SimDocument.h"
#include "core/fem/SimStudy.h"
#include "study_fixtures.h"

#include <BRepPrimAPI_MakeBox.hxx>

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <memory>
#include <string>

using Catch::Matchers::ContainsSubstring;

namespace {

// An 80 x 10 x 10 steel bar, a part, its end clamped and its other end
// pushed down: the bar of tests/test_study_inputs.cpp.
struct Bar {
  Document doc;
  Bar() {
    auto box = std::make_unique<BoxFeature>("Bar", glm::vec3(0, 0, 0), 80.0,
                                            10.0, 10.0);
    box->SetMaterial("steel");
    doc.AddFeature(std::move(box));
  }
  ElementRef Face(int i) const {
    const uint64_t cb = doc.GetBodies()[0].createdBy;
    return ElementRef{cb, std::to_string(cb) + "/face" + std::to_string(i)};
  }
  // The bending study's setup, its loads on `occurrence` (0 on a part).
  fem::Study Bend(uint64_t occurrence = 0) const {
    fem::Study s;
    s.name = "Bend";
    s.converge = fem::ConvergeMode::Never;
    fem::Load fixed;
    fixed.id = 101;
    fixed.type = fem::LoadType::Fixed;
    fixed.face = Face(0);
    fixed.occurrenceId = occurrence;
    s.loads.push_back(fixed);
    fem::Load force;
    force.id = 102;
    force.type = fem::LoadType::Force;
    force.face = Face(1);
    force.magnitude = 500.0;
    force.direction = glm::dvec3(0, 0, -1);
    force.occurrenceId = occurrence;
    s.loads.push_back(force);
    return s;
  }
};

// Adds `setup` to `sim` as a new study, keeping the new study's id.
uint64_t AddStudy(fem::SimDocument& sim, fem::Study setup) {
  const uint64_t id = sim.AddStudy(setup.name).id;
  setup.id = id;
  REQUIRE(sim.ReplaceStudy(std::move(setup)));
  return id;
}

fem::SimDocument PartSim(uint64_t docId = 1) {
  fem::SimDocument sim;
  sim.SetSubject({fem::SubjectKind::Part, docId, 0, "Bar"});
  sim.SetMesh({ExprValue{4.0}, {}});
  return sim;
}

fem::SimContext PartContext(const Bar& bar) {
  fem::SimContext ctx;
  ctx.part = &bar.doc;
  ctx.subjectStamps = {{1, 7}};
  return ctx;
}

// The bar as the one occurrence of an assembly, placed where the part is.
struct BarAssembly {
  Bar bar;
  AssemblyDocument asmDoc;
  uint64_t occ = 0;
  glm::dmat4 placement{1.0};
  BarAssembly() {
    Occurrence o;
    o.name = "Bar";
    o.partId = 1;
    o.fixed = true;
    occ = asmDoc.AddOccurrence(o);
  }
  fem::SimContext Context() const {
    fem::SimContext ctx;
    ctx.assembly = &asmDoc;
    ctx.placedBodies = [this](uint64_t) {
      std::vector<Body> out;
      for (const Body& b : bar.doc.GetBodies()) {
        Body nb = b;
        nb.shape = b.shape.Transformed(placement);
        out.push_back(std::move(nb));
      }
      return out;
    };
    ctx.partOf = [this](uint64_t) { return fem::SimPart{&bar.doc, placement}; };
    return ctx;
  }
  fem::SimDocument Sim() const {
    fem::SimDocument sim;
    sim.SetSubject({fem::SubjectKind::Assembly, 9, 0, "Bar assembly"});
    sim.SetMesh({ExprValue{4.0}, {}});
    sim.SetRoles({{occ, fem::ParticipantRole::Meshed, ExprValue{0.0}}});
    return sim;
  }
};

// Two 100 x 100 x 16 steel blocks as parts, the upper resting on the lower:
// the meshed upper clamped where the rigid lower touches it, its top pressed
// (tests/test_fem.cpp's TwoBlocks).
struct Blocks {
  Document lower, upper;
  AssemblyDocument asmDoc;
  uint64_t lowerOcc = 0, upperOcc = 0;
  Blocks() {
    for (Document* d : {&lower, &upper}) {
      auto box = std::make_unique<BoxFeature>("Block", glm::vec3(0.0f), 100.0,
                                              100.0, 16.0);
      box->SetMaterial("steel");
      d->AddFeature(std::move(box));
    }
    Occurrence lo;
    lo.name = "Lower";
    lo.partId = 1;
    lo.fixed = true;
    lowerOcc = asmDoc.AddOccurrence(lo);
    Occurrence up;
    up.name = "Upper";
    up.partId = 2;
    up.position = glm::dvec3(0.0, 0.0, 16.0);
    upperOcc = asmDoc.AddOccurrence(up);
  }
  glm::dmat4 PlacementOf(uint64_t occ) const {
    return occ == lowerOcc
               ? glm::dmat4(1.0)
               : glm::translate(glm::dmat4(1.0), glm::dvec3(0.0, 0.0, 16.0));
  }
  fem::AssemblyStudyContext Today() const {
    fem::AssemblyStudyContext ctx;
    ctx.placedBodies = [this](uint64_t occ) {
      std::vector<Body> out;
      const Document& part = occ == lowerOcc ? lower : upper;
      for (const Body& b : part.GetBodies()) {
        Body nb = b;
        nb.shape = b.shape.Transformed(PlacementOf(occ));
        out.push_back(std::move(nb));
      }
      return out;
    };
    return ctx;
  }
  fem::SimContext Context() const {
    fem::SimContext ctx;
    ctx.assembly = &asmDoc;
    ctx.placedBodies = Today().placedBodies;
    ctx.partOf = [this](uint64_t occ) {
      return fem::SimPart{occ == lowerOcc ? &lower : &upper, PlacementOf(occ)};
    };
    return ctx;
  }
  // The upper block's top face, by where it is.
  std::string UpperTop() const {
    const Body& b = upper.GetBodies()[0];
    for (const auto& [faceIndex, name] : b.elementMap.names) {
      const auto info = Measure::DescribeFaceAt(b, faceIndex);
      if (info && std::abs(info->centroid.z - 16.0) < 1e-6)
        return name;
    }
    return "";
  }
  fem::Study Pressed() const {
    fem::Study s;
    s.name = "upper on lower";
    s.meshSize = 12.0;
    s.converge = fem::ConvergeMode::Never;
    s.participants = {{upperOcc, fem::ParticipantRole::Meshed, ExprValue{0.0}},
                      {lowerOcc, fem::ParticipantRole::Rigid, ExprValue{0.0}}};
    fem::Load clamp;
    clamp.id = 10;
    clamp.type = fem::LoadType::Fixed;
    clamp.contactWith = lowerOcc;
    s.loads.push_back(clamp);
    fem::Load press;
    press.id = 11;
    press.type = fem::LoadType::Pressure;
    press.occurrenceId = upperOcc;
    press.face = ElementRef{upper.GetBodies()[0].createdBy, UpperTop()};
    press.magnitude = 0.5;
    s.loads.push_back(press);
    return s;
  }
  fem::SimDocument Sim() const {
    fem::SimDocument sim;
    sim.SetSubject({fem::SubjectKind::Assembly, 9, 0, "Blocks"});
    sim.SetRoles(Pressed().participants);
    return sim;
  }
};

fem::StudyRuntime Answer(double peak) {
  fem::StudyRuntime rt;
  rt.maxVonMises = peak;
  return rt;
}

} // namespace

TEST_CASE("A simulation's study goes stale on its own edit, not another's",
          "[fem][simdoc]") {
  fem::SimDocument sim = PartSim();
  const uint64_t a = AddStudy(sim, Bar().Bend());
  const uint64_t b = AddStudy(sim, Bar().Bend());
  sim.StoreStudyResults(a, Answer(1.0));
  sim.StoreStudyResults(b, Answer(2.0));
  const auto stamp = [](uint64_t) { return uint64_t{0}; };
  CHECK_FALSE(sim.StudyResultsStale(a, stamp));
  CHECK_FALSE(sim.StudyResultsStale(b, stamp));

  fem::Study edited = *sim.FindStudy(a);
  edited.loads[1].magnitude = 600.0;
  REQUIRE(sim.ReplaceStudy(edited));
  CHECK(sim.StudyResultsStale(a, stamp));
  CHECK_FALSE(sim.StudyResultsStale(b, stamp));

  SECTION("the document's mesh is every study's") {
    sim.SetMesh({ExprValue{3.0}, {}});
    CHECK(sim.StudyResultsStale(b, stamp));
  }
  SECTION("a subject stamp it was solved against moves") {
    fem::StudyRuntime rt = Answer(2.0);
    rt.solvedPartVersions = {{1, 7}};
    sim.StoreStudyResults(b, rt);
    CHECK_FALSE(sim.StudyResultsStale(b, [](uint64_t) { return 7; }));
    CHECK(sim.StudyResultsStale(b, [](uint64_t) { return 8; }));
  }
  SECTION("an undo snapshot shares the results and meshes") {
    const fem::SimDocument snapshot = sim;
    sim.StoreStudyResults(a, Answer(3.0));
    REQUIRE(snapshot.StudyResults(a) != nullptr);
    CHECK(snapshot.StudyResults(a)->maxVonMises == 3.0);
  }
}

TEST_CASE("A simulation's study with a mesh size of its own does not share "
          "the document's mesh",
          "[fem][simdoc]") {
  fem::SimDocument sim = PartSim();
  fem::Study own = Bar().Bend();
  own.meshSize = 6.0;
  const uint64_t shared = AddStudy(sim, Bar().Bend());
  const uint64_t mine = AddStudy(sim, own);
  CHECK(fem::SimDocument::SharesMesh(*sim.FindStudy(shared)));
  CHECK_FALSE(fem::SimDocument::SharesMesh(*sim.FindStudy(mine)));
  CHECK(sim.Effective(*sim.FindStudy(shared)).meshSize.value == 4.0);
  CHECK(sim.Effective(*sim.FindStudy(mine)).meshSize.value == 6.0);
}

TEST_CASE("A new study is named by its kind, numbered past the names taken",
          "[fem][simdoc]") {
  fem::SimDocument sim = PartSim();
  CHECK(sim.AddStudy("").name == "Static 1");
  CHECK(sim.AddStudy("").name == "Static 2");
  const fem::Study& modal = sim.AddStudy("", fem::AnalysisType::Modal);
  CHECK(modal.name == "Modal 1");
  CHECK(modal.type == fem::AnalysisType::Modal);
  const fem::Study& buckle = sim.AddStudy("", fem::AnalysisType::Buckling);
  CHECK(buckle.name == "Buckling 1");
  CHECK(buckle.numModes == fem::kBucklingFactorsDefault);
  // A name typed by hand that a kind name would repeat is skipped.
  sim.AddStudy("Thermal 1", fem::AnalysisType::Thermal);
  CHECK(sim.AddStudy("", fem::AnalysisType::Thermal).name == "Thermal 2");
}

TEST_CASE("Clearing a simulation's storage drops its meshes and results",
          "[fem][simdoc]") {
  fem::SimDocument sim = PartSim();
  const uint64_t a = AddStudy(sim, Bar().Bend());
  sim.StoreStudyResults(a, Answer(1.0));
  auto mesh = std::make_shared<const fem::FemMesh>();
  sim.StoreMesh(42, {mesh, "Bar"});
  REQUIRE(sim.FindMesh(42) != nullptr);
  const uint64_t version = sim.Version();
  const auto [meshes, results] = sim.ClearStorage();
  CHECK(meshes == 1);
  CHECK(results == 1);
  CHECK(sim.FindMesh(42) == nullptr);
  CHECK(sim.StudyResults(a) == nullptr);
  CHECK(sim.Version() > version);
  // The studies stay.
  CHECK(sim.FindStudy(a) != nullptr);
}

TEST_CASE("A variable renamed reaches a simulation's studies and mesh, and "
          "one it reads is named before a delete",
          "[fem][simdoc][variables]") {
  fem::SimDocument sim = PartSim();
  fem::Study setup = Bar().Bend();
  setup.loads[1].magnitude.expr = "push";
  const uint64_t id = AddStudy(sim, setup);
  fem::SimMesh mesh;
  mesh.size.expr = "wall / 2";
  sim.SetMesh(mesh);

  CHECK(sim.FindParameterReferences("push") ==
        std::vector<std::string>{"study 'Bend'"});
  CHECK(sim.FindParameterReferences("wall") ==
        std::vector<std::string>{"simulation mesh"});
  CHECK(sim.FindParameterReferences("other").empty());

  REQUIRE(sim.RenameReferences("push", "shove"));
  CHECK(sim.FindStudy(id)->loads[1].magnitude.expr == "shove");
  REQUIRE(sim.RenameReferences("wall", "skin"));
  CHECK(sim.Mesh().size.expr == "skin / 2");
  CHECK(sim.FindParameterReferences("push").empty());
  CHECK_FALSE(sim.RenameReferences("push", "x"));
}

TEST_CASE("A mesh key is the shape and its settings", "[fem][simdoc]") {
  const TopoDS_Shape box = BRepPrimAPI_MakeBox(80.0, 10.0, 10.0).Shape();
  const TopoDS_Shape longer = BRepPrimAPI_MakeBox(81.0, 10.0, 10.0).Shape();
  fem::MeshSettings s;
  s.maxSize = 4.0;
  const uint64_t key = fem::MeshKey(box, s);
  CHECK(key != 0);
  CHECK(fem::MeshKey(BRepPrimAPI_MakeBox(80.0, 10.0, 10.0).Shape(), s) == key);
  CHECK(fem::MeshKey(longer, s) != key);
  fem::MeshSettings finer = s;
  finer.maxSize = 3.0;
  CHECK(fem::MeshKey(box, finer) != key);
  fem::MeshSettings refined = s;
  refined.faceSizes = {{2, 1.0}};
  CHECK(fem::MeshKey(box, refined) != key);
  CHECK(fem::MeshKey(TopoDS_Shape(), s) == 0);
}

TEST_CASE("A part and the part as an assembly's one occurrence plan and hash "
          "the same",
          "[fem][simdoc]") {
  Bar bar;
  fem::SimDocument partSim = PartSim();
  const uint64_t partStudy = AddStudy(partSim, bar.Bend());
  fem::SimContext partCtx = PartContext(bar);
  partCtx.sim = &partSim;

  BarAssembly world;
  fem::SimDocument asmSim = world.Sim();
  const uint64_t asmStudy = AddStudy(asmSim, world.bar.Bend(world.occ));
  fem::SimContext asmCtx = world.Context();
  asmCtx.sim = &asmSim;

  const uint64_t key = fem::SimStudyInputKey(partCtx, partStudy);
  CHECK(key != 0);
  CHECK(fem::SimStudyInputKey(asmCtx, asmStudy) == key);
  // ...and the part's hash is the one today's part study has.
  fem::Study today = partSim.Effective(*partSim.FindStudy(partStudy));
  CHECK(fem::StudyInputKey(bar.doc, today) == key);

  fem::StudyPlan a, b;
  std::string err;
  REQUIRE(fem::PlanSimStudy(partCtx, partStudy, a, &err));
  REQUIRE(fem::PlanSimStudy(asmCtx, asmStudy, b, &err));
  INFO(err);
  REQUIRE(a.bodies.size() == 1);
  REQUIRE(b.bodies.size() == 1);
  CHECK(a.bodies[0].meshKey != 0);
  CHECK(a.bodies[0].meshKey == b.bodies[0].meshKey);

  SECTION("a frame support works there, as on the part") {
    fem::Study s = *asmSim.FindStudy(asmStudy);
    fem::Load roller;
    roller.id = 103;
    roller.type = fem::LoadType::Roller;
    roller.face = world.bar.Face(2);
    roller.occurrenceId = world.occ;
    s.loads.push_back(roller);
    REQUIRE(asmSim.ReplaceStudy(s));
    fem::StudyPlan plan;
    CHECK(fem::PlanSimStudy(asmCtx, asmStudy, plan, &err));
    INFO(err);
  }
  SECTION("a load on part of a face works there, as on the part") {
    fem::Study s = *asmSim.FindStudy(asmStudy);
    s.loads[1].region.kind = fem::LoadRegion::Kind::Sphere;
    s.loads[1].region.center = glm::dvec3(80.0, 5.0, 5.0);
    s.loads[1].region.radiusMm = 4.0;
    REQUIRE(asmSim.ReplaceStudy(s));
    fem::StudyPlan plan;
    CHECK(fem::PlanSimStudy(asmCtx, asmStudy, plan, &err));
    INFO(err);
  }
  SECTION("placed away from the part's frame, it is an assembly study and "
          "says why a frame support is refused") {
    world.placement =
        glm::translate(glm::dmat4(1.0), glm::dvec3(0.0, 0.0, 50.0));
    fem::Study s = *asmSim.FindStudy(asmStudy);
    fem::Load roller;
    roller.id = 103;
    roller.type = fem::LoadType::Roller;
    roller.face = world.bar.Face(2);
    roller.occurrenceId = world.occ;
    s.loads.push_back(roller);
    REQUIRE(asmSim.ReplaceStudy(s));
    fem::StudyPlan plan;
    CHECK_FALSE(fem::PlanSimStudy(asmCtx, asmStudy, plan, &err));
    CHECK_THAT(err, ContainsSubstring("placed away from its part's own frame"));
    CHECK_THAT(err, ContainsSubstring("roller support is a part study's"));
  }
  SECTION("a load on an occurrence that does not take part is refused") {
    Occurrence other;
    other.name = "Spare";
    other.partId = 1;
    const uint64_t spare = world.asmDoc.AddOccurrence(other);
    fem::Study s = *asmSim.FindStudy(asmStudy);
    s.loads[1].occurrenceId = spare;
    REQUIRE(asmSim.ReplaceStudy(s));
    fem::StudyPlan plan;
    CHECK_FALSE(fem::PlanSimStudy(asmCtx, asmStudy, plan, &err));
    CHECK_THAT(err, ContainsSubstring("'Spare', which is not meshed"));
    CHECK(fem::SimStudyInputKey(asmCtx, asmStudy) == 0);
  }
}

TEST_CASE("A simulation refuses what its planner does not build yet",
          "[fem][simdoc]") {
  std::string err;
  fem::StudyPlan plan;
  SECTION("a missing part") {
    fem::SimDocument sim = PartSim();
    const uint64_t id = AddStudy(sim, Bar().Bend());
    fem::SimContext ctx;
    ctx.sim = &sim;
    CHECK_FALSE(fem::PlanSimStudy(ctx, id, plan, &err));
    CHECK_THAT(err, ContainsSubstring("part 'Bar' is missing"));
  }
  SECTION("a study of a topology result the simulation does not hold, as "
          "the part refuses it") {
    Bar bar;
    fem::SimDocument sim = PartSim();
    fem::Study s = bar.Bend();
    s.shapeFrom.study = 77;
    const uint64_t id = AddStudy(sim, s);
    fem::SimContext ctx = PartContext(bar);
    ctx.sim = &sim;
    CHECK_FALSE(fem::PlanSimStudy(ctx, id, plan, &err));
    std::string onPart;
    fem::StudyPlan partPlan;
    fem::Study copy = sim.Effective(*sim.FindStudy(id));
    CHECK_FALSE(fem::PlanStudy(bar.doc, copy, partPlan, &onPart));
    CHECK(err == onPart);
    CHECK_THAT(err, ContainsSubstring("not a topology study"));
  }
  SECTION("a thermal load from a study that is not thermal, naming the "
          "thermal ones") {
    Bar bar;
    fem::SimDocument sim = PartSim();
    fem::Study heat = bar.Bend();
    heat.type = fem::AnalysisType::Thermal;
    heat.loads.clear();
    const uint64_t heatId = AddStudy(sim, heat);
    const uint64_t bendId = AddStudy(sim, bar.Bend());
    fem::Study s = bar.Bend();
    s.thermalLoad.kind = fem::ThermalLoad::Kind::FromStudy;
    s.thermalLoad.fromStudy = bendId;
    const uint64_t id = AddStudy(sim, s);
    fem::SimContext ctx = PartContext(bar);
    ctx.sim = &sim;
    CHECK_FALSE(fem::PlanSimStudy(ctx, id, plan, &err));
    CHECK_THAT(err, ContainsSubstring("is a static study, not a thermal one"));
    CHECK_THAT(err,
               ContainsSubstring("thermal studies: " + std::to_string(heatId)));
  }
  SECTION("beside a neighbour: a topology run, a study of a topology result, "
          "and a thermal load from a study") {
    Blocks world;
    fem::SimDocument sim = world.Sim();
    fem::Study topo = world.Pressed();
    topo.type = fem::AnalysisType::Topology;
    const uint64_t topoId = AddStudy(sim, topo);
    fem::StudyRuntime rt;
    CHECK_FALSE(fem::RunSimStudy(sim, world.Context(), topoId, rt, &err));
    CHECK_THAT(err, ContainsSubstring("a topology study optimises one part"));
    CHECK_THAT(err, ContainsSubstring("1 rigid, bonded or contact neighbour"));
    CHECK(sim.FindStudy(topoId)->error == err);
    fem::SimContext ctx = world.Context();
    ctx.sim = &sim;
    fem::TopoInput input;
    std::string prepErr;
    CHECK_FALSE(fem::PrepareSimTopology(ctx, topoId, input, &prepErr));
    CHECK(prepErr == err);
    const auto checked = fem::CheckSimStudy(ctx, topoId);
    REQUIRE(checked.size() == 1);
    CHECK(checked[0].code == "plan");
    CHECK(checked[0].message == err);

    fem::Study shape = world.Pressed();
    shape.shapeFrom.study = topoId;
    const uint64_t shapeId = AddStudy(sim, shape);
    CHECK_FALSE(fem::PlanSimStudy(ctx, shapeId, plan, &err));
    CHECK_THAT(err, ContainsSubstring("a study of a topology result is a "
                                      "part study's"));
    CHECK(fem::SimStudyInputKey(ctx, shapeId) == 0);

    fem::Study heated = world.Pressed();
    heated.thermalLoad.kind = fem::ThermalLoad::Kind::FromStudy;
    heated.thermalLoad.fromStudy = topoId;
    const uint64_t heatedId = AddStudy(sim, heated);
    CHECK_FALSE(fem::PlanSimStudy(ctx, heatedId, plan, &err));
    CHECK_THAT(err, ContainsSubstring("an assembly study does not solve heat"));
  }
  SECTION("beside a rigid neighbour: a frame support, and a load on part of "
          "a face") {
    Blocks world;
    fem::SimDocument sim = world.Sim();
    fem::Study s = world.Pressed();
    fem::Load roller;
    roller.id = 12;
    roller.type = fem::LoadType::Roller;
    roller.occurrenceId = world.upperOcc;
    roller.face = s.loads[1].face;
    s.loads.push_back(roller);
    const uint64_t id = AddStudy(sim, s);
    fem::SimContext ctx = world.Context();
    ctx.sim = &sim;
    CHECK_FALSE(fem::PlanSimStudy(ctx, id, plan, &err));
    CHECK_THAT(err, ContainsSubstring("roller support is a part study's"));

    fem::Study banded = world.Pressed();
    banded.loads[1].region.kind = fem::LoadRegion::Kind::Sphere;
    banded.loads[1].region.radiusMm = 10.0;
    const uint64_t id2 = AddStudy(sim, banded);
    CHECK_FALSE(fem::PlanSimStudy(ctx, id2, plan, &err));
    CHECK_THAT(err, ContainsSubstring("part of a face"));
  }
}

TEST_CASE("A simulation's part study checks as the part's study does",
          "[fem][simdoc]") {
  Bar bar;
  fem::SimDocument sim = PartSim();
  fem::SimContext ctx = PartContext(bar);
  ctx.sim = &sim;
  const auto same = [&](uint64_t id) {
    const fem::Study today = sim.Effective(*sim.FindStudy(id));
    const auto expected = fem::CheckStudy(bar.doc, today);
    const auto got = fem::CheckSimStudy(ctx, id);
    REQUIRE(got.size() == expected.size());
    for (size_t i = 0; i < got.size(); ++i) {
      CHECK(got[i].code == expected[i].code);
      CHECK(got[i].message == expected[i].message);
      CHECK(got[i].loadId == expected[i].loadId);
      CHECK(got[i].fixMethod == expected[i].fixMethod);
    }
    return got;
  };
  CHECK(same(AddStudy(sim, bar.Bend())).empty());
  fem::Study unheld = bar.Bend();
  unheld.loads.erase(unheld.loads.begin());
  CHECK_FALSE(same(AddStudy(sim, unheld)).empty());
  fem::Study cut = bar.Bend();
  cut.shapeFrom.study = 77;
  CHECK_FALSE(same(AddStudy(sim, cut)).empty());

  // A load naming an occurrence on a part subject: the route's refusal.
  fem::Study wrong = bar.Bend(5);
  const auto problems = fem::CheckSimStudy(ctx, AddStudy(sim, wrong));
  REQUIRE(problems.size() == 1);
  CHECK(problems[0].code == "plan");
  CHECK_THAT(problems[0].message, ContainsSubstring("names an occurrence"));
}

#if defined(HAVE_NETGEN) && defined(HAVE_FEM_SOLVER)

namespace {

void CheckSameAnswer(const fem::StudyRuntime& a, const fem::StudyRuntime& b) {
  CHECK(a.maxVonMises == b.maxVonMises);
  CHECK(a.maxDisplacement == b.maxDisplacement);
  CHECK(a.maxVmAt == b.maxVmAt);
  REQUIRE(a.bodies.size() == b.bodies.size());
  for (size_t i = 0; i < a.bodies.size(); ++i) {
    CHECK(a.bodies[i].mesh.nodes == b.bodies[i].mesh.nodes);
    CHECK(a.bodies[i].mesh.tets.size() == b.bodies[i].mesh.tets.size());
  }
}

} // namespace

TEST_CASE("A part study through the simulation answers as today's RunStudy, "
          "and its studies share one mesh",
          "[fem][simdoc]") {
  Bar bar;
  fem::Study today = NewStudy(bar.doc, "Bend");
  {
    fem::Study setup = bar.Bend();
    setup.id = today.id;
    setup.meshSize = 4.0;
    today = setup;
  }
  fem::StudyRuntime expected;
  std::string err;
  REQUIRE(fem::RunStudy(bar.doc, today, expected, &err));
  INFO(err);

  fem::SimDocument sim = PartSim();
  const uint64_t first = AddStudy(sim, bar.Bend());
  fem::StudyRuntime rt;
  int made = -1;
  REQUIRE(fem::RunSimStudy(sim, PartContext(bar), first, rt, &err, &made));
  CheckSameAnswer(rt, expected);
  CHECK(made == 1);
  CHECK(sim.Meshes().size() == 1);
  REQUIRE(sim.StudyResults(first) != nullptr);
  CHECK(sim.StudyResults(first)->inputHash ==
        fem::StudyInputKey(bar.doc, today));
  CHECK(sim.StudyResults(first)->meshKeys.size() == 1);
  CHECK_FALSE(sim.StudyResultsStale(first, [](uint64_t) { return 7; }));

  // A second study, other loads, the same mesh settings: meshed once.
  fem::Study heavier = bar.Bend();
  heavier.loads[1].magnitude = 900.0;
  const uint64_t second = AddStudy(sim, heavier);
  fem::StudyRuntime rt2;
  REQUIRE(fem::RunSimStudy(sim, PartContext(bar), second, rt2, &err, &made));
  CHECK(made == 0);
  CHECK(sim.Meshes().size() == 1);
  CHECK(rt2.bodies[0].mesh.nodes == rt.bodies[0].mesh.nodes);
  CHECK(rt2.maxVonMises > rt.maxVonMises);
  // Editing the second study left the first current.
  CHECK_FALSE(sim.StudyResultsStale(first, [](uint64_t) { return 7; }));

  // A mesh size of its own is a mesh of its own.
  fem::Study coarse = bar.Bend();
  coarse.meshSize = 6.0;
  const uint64_t third = AddStudy(sim, coarse);
  fem::StudyRuntime rt3;
  REQUIRE(fem::RunSimStudy(sim, PartContext(bar), third, rt3, &err, &made));
  CHECK(made == 1);
  CHECK(sim.Meshes().size() == 2);

  // An edit to the subject moves the key: nothing stored is solved on.
  fem::SimContext ctx = PartContext(bar);
  ctx.sim = &sim;
  fem::StudyPlan before;
  REQUIRE(fem::PlanSimStudy(ctx, first, before, &err));
  CHECK(before.bodies[0].stored != nullptr);
  auto* box = dynamic_cast<BoxFeature*>(bar.doc.GetFeature(0));
  REQUIRE(box != nullptr);
  box->width = 81.0;
  bar.doc.Regenerate();
  fem::StudyPlan after;
  REQUIRE(fem::PlanSimStudy(ctx, first, after, &err));
  CHECK(after.bodies[0].meshKey != before.bodies[0].meshKey);
  CHECK(after.bodies[0].stored == nullptr);
}

TEST_CASE("The part as an assembly's one occurrence answers as the part",
          "[fem][simdoc]") {
  BarAssembly world;
  fem::SimDocument asmSim = world.Sim();
  const uint64_t asmStudy = AddStudy(asmSim, world.bar.Bend(world.occ));
  fem::SimDocument partSim = PartSim();
  const uint64_t partStudy = AddStudy(partSim, world.bar.Bend());
  fem::StudyRuntime a, b;
  std::string err;
  REQUIRE(
      fem::RunSimStudy(partSim, PartContext(world.bar), partStudy, a, &err));
  REQUIRE(fem::RunSimStudy(asmSim, world.Context(), asmStudy, b, &err));
  INFO(err);
  CheckSameAnswer(a, b);
  CHECK(asmSim.StudyResults(asmStudy)->inputHash ==
        partSim.StudyResults(partStudy)->inputHash);
}

TEST_CASE("An assembly study through the simulation answers as today's "
          "assembly planner",
          "[fem][simdoc][assembly]") {
  Blocks world;
  fem::Study today = world.Pressed();
  today.id = 1;
  fem::StudyPlan plan;
  std::string err;
  REQUIRE(
      fem::PlanAssemblyStudy(world.asmDoc, today, world.Today(), plan, &err));
  fem::StudyJobInput input;
  fem::StudyRuntime expected;
  fem::ConvergeOptions never;
  never.mode = fem::ConvergeMode::Never;
  REQUIRE(fem::SolveConverged(plan, input, expected, never, &err));
  INFO(err);

  fem::SimDocument sim = world.Sim();
  const uint64_t id = AddStudy(sim, world.Pressed());
  fem::StudyRuntime rt;
  int made = -1;
  REQUIRE(fem::RunSimStudy(sim, world.Context(), id, rt, &err, &made));
  CheckSameAnswer(rt, expected);
  CHECK(made == 1);
  CHECK(sim.StudyResults(id)->inputHash ==
        fem::AssemblyStudyInputKey(world.asmDoc, today, world.Today()));

  // Run again: the stored mesh, the same answer.
  fem::StudyRuntime again;
  REQUIRE(fem::RunSimStudy(sim, world.Context(), id, again, &err, &made));
  CHECK(made == 0);
  CheckSameAnswer(again, expected);
}

TEST_CASE("A simulation meshes a study as its run does, and the run then "
          "meshes nothing",
          "[fem][simdoc]") {
  Bar bar;
  fem::SimDocument sim = PartSim();
  const uint64_t id = AddStudy(sim, bar.Bend());
  std::vector<fem::StudyBodyMesh> meshes;
  std::vector<fem::MeshBodyReport> reports;
  std::string err;
  int made = -1;
  REQUIRE(fem::MeshSimStudy(sim, PartContext(bar), id, &meshes, &err, &made,
                            &reports));
  INFO(err);
  CHECK(made == 1);
  REQUIRE(meshes.size() == 1);
  CHECK(reports.size() == 1);
  CHECK(meshes[0].bodyName == bar.doc.GetBodies()[0].name);
  CHECK(meshes[0].mesh.tets.size() > 0);
  CHECK(sim.Meshes().size() == 1);
  REQUIRE(sim.DocumentMeshKeys().size() == 1);
  CHECK(sim.FindMesh(sim.DocumentMeshKeys()[0]) != nullptr);

  // Meshing again reads the store.
  REQUIRE(fem::MeshSimStudy(sim, PartContext(bar), id, nullptr, &err, &made));
  CHECK(made == 0);

  fem::StudyRuntime rt;
  REQUIRE(fem::RunSimStudy(sim, PartContext(bar), id, rt, &err, &made));
  CHECK(made == 0);
  REQUIRE(rt.bodies.size() == 1);
  CHECK(rt.bodies[0].mesh.stats.tetCount == meshes[0].mesh.tets.size());
  CHECK(sim.Meshes().size() == 1);
}

TEST_CASE("A simulation's thermal load from a thermal study solves and "
          "hashes as on the part",
          "[fem][simdoc][thermal]") {
  Bar bar;
  fem::Study heat = NewStudy(bar.doc, "Heat");
  heat.type = fem::AnalysisType::Thermal;
  heat.meshSize = 4.0;
  fem::Load hot;
  hot.id = bar.doc.AllocateId();
  hot.type = fem::LoadType::Temperature;
  hot.face = bar.Face(0);
  hot.temperatureC = 100.0;
  fem::Load cold = hot;
  cold.id = bar.doc.AllocateId();
  cold.face = bar.Face(1);
  cold.temperatureC = 20.0;
  heat.loads = {hot, cold};
  const uint64_t heatId = heat.id;
  fem::Study st = NewStudy(bar.doc, "Clamped");
  {
    fem::Study setup = bar.Bend();
    setup.id = st.id;
    setup.name = st.name;
    setup.loads.pop_back(); // clamped at one end, heated
    setup.meshSize = 4.0;
    setup.thermalLoad.kind = fem::ThermalLoad::Kind::FromStudy;
    setup.thermalLoad.fromStudy = heatId;
    setup.thermalLoad.hasStressFree = true;
    setup.thermalLoad.stressFreeC = 20.0;
    st = setup;
  }
  const uint64_t staticId = st.id;
  // The part route, handed the two studies as its siblings.
  TestStudies studies;
  studies.Add(heat);
  studies.Add(st);
  const fem::StudySiblings siblings = studies.Siblings();
  fem::StudyRuntime expected;
  std::string err;
  REQUIRE(fem::RunStudy(bar.doc, st, expected, &err, &siblings));
  INFO(err);
  REQUIRE(expected.bodies.size() == 1);
  CHECK(expected.bodies[0].thermal.load);

  // The same two studies, the same ids, on a simulation of the part.
  fem::SimDocument sim = PartSim();
  for (const fem::Study& s : studies.studies)
    sim.RestoreStudy(s);
  fem::SimContext ctx = PartContext(bar);
  ctx.sim = &sim;
  const uint64_t key = fem::SimStudyInputKey(ctx, staticId);
  CHECK(key != 0);
  CHECK(key == fem::StudyInputKey(bar.doc, st, &siblings));
  fem::StudyRuntime rt;
  REQUIRE(fem::RunSimStudy(sim, PartContext(bar), staticId, rt, &err));
  CheckSameAnswer(rt, expected);
  CHECK(rt.bodies[0].thermal.maxC == expected.bodies[0].thermal.maxC);
  const auto stamps = [](uint64_t) { return uint64_t{7}; };
  CHECK_FALSE(sim.StudyResultsStale(staticId, stamps));

  // An edit to the thermal study moves the static one's hash and stamp.
  fem::Study hotter = *sim.FindStudy(heatId);
  hotter.loads[0].temperatureC = 120.0;
  REQUIRE(sim.ReplaceStudy(hotter));
  CHECK(sim.StudyResultsStale(staticId, stamps));
  CHECK(fem::SimStudyInputKey(ctx, staticId) != key);
}

TEST_CASE("A simulation optimises its part, and a static study of the shape "
          "solves and hashes as on the part",
          "[fem][simdoc][topopt]") {
  Bar bar;
  fem::Study topo = NewStudy(bar.doc, "Generative");
  {
    fem::Study setup = bar.Bend();
    setup.id = topo.id;
    setup.name = topo.name;
    setup.type = fem::AnalysisType::Topology;
    setup.meshSize = 4.0;
    setup.volumeFraction = 0.5;
    setup.filterRadius = 6.0;
    setup.maxIterations = 10;
    topo = setup;
  }
  const uint64_t topoId = topo.id;
  fem::Study shape = NewStudy(bar.doc, "Shape");
  {
    fem::Study setup = bar.Bend();
    setup.id = shape.id;
    setup.name = shape.name;
    setup.shapeFrom = {topoId, 0.0, false};
    setup.meshSize = 4.0; // the simulation's, as its Effective form reads
    shape = setup;
  }
  const uint64_t shapeId = shape.id;
  TestStudies studies; // the part route's, handed over as siblings
  studies.Add(topo);
  studies.Add(shape);
  const fem::StudySiblings siblings = studies.Siblings();

  fem::SimDocument sim = PartSim();
  for (const fem::Study& s : studies.studies)
    sim.RestoreStudy(s);
  fem::SimContext ctx = PartContext(bar);
  ctx.sim = &sim;
  const auto stamps = [](uint64_t) { return uint64_t{7}; };

  // Before the topology study has run, the shape study is refused as the
  // part refuses it.
  {
    fem::StudyPlan plan;
    std::string err;
    CHECK_FALSE(fem::PlanSimStudy(ctx, shapeId, plan, &err));
    CHECK_THAT(err, ContainsSubstring("has no result to take a shape from"));
  }

  std::string err;
  fem::StudyRuntime topoRt;
  int made = -1;
  REQUIRE(fem::RunSimStudy(sim, PartContext(bar), topoId, topoRt, &err, &made));
  INFO(err);
  CHECK(made == 0);
  CHECK(topoRt.topology);
  REQUIRE(sim.StudyResults(topoId) != nullptr);
  CHECK(sim.StudyResults(topoId)->inputHash ==
        fem::StudyInputKey(bar.doc, topo, &siblings));
  CHECK(sim.FindStudy(topoId)->error.empty());
  CHECK_FALSE(sim.StudyResultsStale(topoId, stamps));

  // The async path's preparation is the run's.
  {
    fem::TopoInput input;
    REQUIRE(fem::PrepareSimTopology(ctx, topoId, input, &err));
    CHECK(input.mesh.tets.size() == topoRt.bodies[0].mesh.tets.size());
  }

  // The part route handed the same topology result: the shape study then
  // plans, hashes and solves alike on both.
  studies.results[topoId] = *sim.StudyResults(topoId);
  const uint64_t key = fem::SimStudyInputKey(ctx, shapeId);
  CHECK(key != 0);
  CHECK(key == fem::StudyInputKey(bar.doc, shape, &siblings));
  fem::StudyRuntime expected;
  REQUIRE(fem::RunStudy(bar.doc, shape, expected, &err, &siblings));
  fem::StudyRuntime rt;
  REQUIRE(fem::RunSimStudy(sim, PartContext(bar), shapeId, rt, &err, &made));
  CHECK(made == 0); // the shape is cut from the result, not meshed
  CheckSameAnswer(rt, expected);
  CHECK(rt.shapeVolume == expected.shapeVolume);
  CHECK_FALSE(sim.StudyResultsStale(shapeId, stamps));

  // A new topology result leaves the shape study stale, and moves its hash.
  fem::StudyRuntime other = *sim.StudyResults(topoId);
  other.bodies[0].density[0] = 0.5 * (other.bodies[0].density[0] + 0.01);
  other.densityDigest =
      fem::DensityDigest(other.bodies[0].mesh, other.bodies[0].density);
  sim.StoreStudyResults(topoId, std::move(other));
  CHECK(sim.StudyResultsStale(shapeId, stamps));
  CHECK(fem::SimStudyInputKey(ctx, shapeId) != key);
}

#endif // HAVE_NETGEN && HAVE_FEM_SOLVER
