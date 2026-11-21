// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

// The simulation document saved and opened, and the studies of a part or an
// assembly moved into one (docs/simulation/README.md#how-a-simulation-is-saved,
// #moving-studies-into-a-simulation).

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "core/Document.h"
#include "core/Feature.h"
#include "core/Occurrence.h"
#include "core/fem/FemSolver.h"
#include "core/fem/SimDocument.h"
#include "core/fem/SimMigration.h"
#include "core/fem/SimStudy.h"
#include "core/io/ReadContext.h"
#include "core/io/SerializeSimulation.h"
#include "study_fixtures.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <unistd.h>

using Catch::Matchers::ContainsSubstring;
namespace fs = std::filesystem;

namespace {

fs::path Scratch(const std::string& name) {
  const fs::path p =
      fs::temp_directory_path() /
      ("lobole-simstore-" + std::to_string(::getpid()) + "-" + name);
  fs::remove_all(p);
  fs::create_directories(p);
  return p;
}

// An 80 x 10 x 10 steel bar clamped at one end and pushed down at the other
// (tests/test_sim_document.cpp's).
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
  fem::Study Bend() const {
    fem::Study s;
    s.name = "Bend";
    s.converge = fem::ConvergeMode::Never;
    fem::Load fixed;
    fixed.id = 101;
    fixed.type = fem::LoadType::Fixed;
    fixed.face = Face(0);
    s.loads.push_back(fixed);
    fem::Load force;
    force.id = 102;
    force.type = fem::LoadType::Force;
    force.face = Face(1);
    force.magnitude = 500.0;
    force.direction = glm::dvec3(0, 0, -1);
    s.loads.push_back(force);
    return s;
  }
  fem::SimContext Context() const {
    fem::SimContext ctx;
    ctx.part = &doc;
    ctx.subjectStamps = {{1, 7}};
    return ctx;
  }
};

uint64_t AddStudy(fem::SimDocument& sim, fem::Study setup) {
  const uint64_t id = sim.AddStudy(setup.name).id;
  setup.id = id;
  REQUIRE(sim.ReplaceStudy(std::move(setup)));
  return id;
}

nlohmann::json ReadJson(const fs::path& p) {
  std::ifstream in(p);
  return nlohmann::json::parse(in);
}

void WriteJson(const fs::path& p, const nlohmann::json& j) {
  std::ofstream(p) << j.dump(2);
}

// A minimal simulation file, for the refusals to damage.
nlohmann::json MinimalFile() {
  fem::SimDocument sim;
  sim.SetSubject({fem::SubjectKind::Part, 3, 0, "Bracket"});
  fem::Study s;
  s.id = 5;
  s.name = "Load";
  sim.RestoreStudy(s);
  const fs::path dir = Scratch("minimal");
  REQUIRE(DocumentIO::SaveSimulation(sim, (dir / "s.json").string()));
  nlohmann::json j = ReadJson(dir / "s.json");
  fs::remove_all(dir);
  return j;
}

std::string LoadError(const nlohmann::json& j, const std::string& name) {
  const fs::path dir = Scratch("refuse-" + name);
  WriteJson(dir / "s.json", j);
  auto r = DocumentIO::LoadSimulation((dir / "s.json").string());
  fs::remove_all(dir);
  REQUIRE_FALSE(r.has_value());
  return r.error();
}

} // namespace

TEST_CASE("A saved simulation's refusals name what is accepted",
          "[fem][simdoc][io]") {
  const nlohmann::json good = MinimalFile();
  {
    const fs::path dir = Scratch("good");
    WriteJson(dir / "s.json", good);
    auto r = DocumentIO::LoadSimulation((dir / "s.json").string());
    fs::remove_all(dir);
    REQUIRE(r.has_value());
    CHECK((*r)->Subject().docId == 3);
    REQUIRE((*r)->FindStudy(5) != nullptr);
    CHECK((*r)->NextId() == 6);
  }
  SECTION("an unknown subject kind") {
    nlohmann::json j = good;
    j["subject"]["kind"] = "drawing";
    CHECK_THAT(LoadError(j, "kind"),
               ContainsSubstring("unknown kind 'drawing' (expected one of: "
                                 "part, assembly)"));
  }
  SECTION("an unknown key") {
    nlohmann::json j = good;
    j["geometry"] = nlohmann::json::object();
    CHECK_THAT(LoadError(j, "key"), ContainsSubstring("geometry"));
  }
  SECTION("an unknown role, and roles on a part") {
    nlohmann::json j = good;
    j["subject"]["kind"] = "assembly";
    j["roles"] = {{{"occurrence", 4}, {"role", "glued"}}};
    CHECK_THAT(LoadError(j, "role"),
               ContainsSubstring("unknown participant role 'glued' (expected "
                                 "one of: meshed, rigid, bonded, contact, "
                                 "ignored)"));
    j["subject"]["kind"] = "part";
    j["roles"] = {{{"occurrence", 4}, {"role", "meshed"}}};
    CHECK_THAT(LoadError(j, "part-roles"),
               ContainsSubstring("a part's simulation has none"));
  }
  SECTION("no subject, or no document in it") {
    nlohmann::json j = good;
    j.erase("subject");
    CHECK_THAT(LoadError(j, "nosubject"), ContainsSubstring("`subject`"));
    j = good;
    j["subject"]["document"] = 0;
    CHECK_THAT(LoadError(j, "nodoc"), ContainsSubstring("document"));
  }
  SECTION("a mesh size below 0, an unknown mesh key") {
    nlohmann::json j = good;
    j["mesh"]["size"] = -2.0;
    CHECK_THAT(LoadError(j, "size"), ContainsSubstring("mesh: size"));
    j = good;
    j["mesh"]["grading"] = 0.3;
    CHECK_THAT(LoadError(j, "meshkey"), ContainsSubstring("grading"));
  }
  SECTION("a study that does not read") {
    nlohmann::json j = good;
    j["studies"][0]["analysis"] = "fatigue";
    CHECK_THAT(LoadError(j, "study"), ContainsSubstring("fatigue"));
  }
  SECTION("another kind of document, or a version before simulations") {
    nlohmann::json j = good;
    j["kind"] = "job";
    CHECK_THAT(LoadError(j, "job"), ContainsSubstring("not a simulation"));
    j = good;
    j["version"] = 1;
    CHECK_THAT(LoadError(j, "old"),
               ContainsSubstring("predates simulation documents"));
  }
}

TEST_CASE("A variable renamed reaches a reopened simulation's expressions",
          "[fem][simdoc][io]") {
  fem::SimDocument sim;
  sim.SetSubject({fem::SubjectKind::Part, 1, 0, "Bar"});
  fem::SimMesh mesh;
  mesh.size = ExprValue{4.0};
  mesh.size.expr = "mesh_mm";
  sim.SetMesh(mesh);
  fem::Study s;
  s.name = "Own";
  s.meshSize = ExprValue{2.0};
  s.meshSize.expr = "mesh_mm / 2";
  AddStudy(sim, s);
  for (const char* form : {"s.json", "s.lob"}) {
    INFO(form);
    const fs::path dir = Scratch("rename");
    REQUIRE(DocumentIO::SaveSimulation(sim, (dir / form).string()));
    auto loaded = DocumentIO::LoadSimulation((dir / form).string());
    fs::remove_all(dir);
    REQUIRE(loaded.has_value());
    fem::SimDocument& back = **loaded;
    CHECK(back.Mesh().size.expr == "mesh_mm");
    CHECK(back.Mesh().size.value == 4.0);
    CHECK(back.FindParameterReferences("mesh_mm").size() == 2);
    REQUIRE(back.RenameReferences("mesh_mm", "element_mm"));
    CHECK(back.Mesh().size.expr == "element_mm");
    CHECK(back.GetStudies()[0].meshSize.expr == "element_mm / 2");
    CHECK(back.FindParameterReferences("mesh_mm").empty());
  }
}

TEST_CASE("An assembly's studies move into one simulation per participants "
          "table",
          "[fem][simdoc][migration]") {
  AssemblyDocument asmDoc;
  Occurrence a, b, c;
  a.name = "A";
  a.partId = 1;
  b.name = "B";
  b.partId = 2;
  c.name = "C";
  c.partId = 3;
  const uint64_t oa = asmDoc.AddOccurrence(a);
  const uint64_t ob = asmDoc.AddOccurrence(b);
  const uint64_t oc = asmDoc.AddOccurrence(c);
  fem::OldStudies old; // as the reader keeps an old assembly file's
  const auto study = [&](const std::string& name,
                         std::vector<fem::Participant> table) {
    fem::Load l;
    l.id = asmDoc.AllocateId();
    l.type = fem::LoadType::Fixed;
    l.occurrenceId = table.empty() ? 0 : table[0].occurrenceId;
    fem::Study s = NewStudy(asmDoc, name);
    s.participants = std::move(table);
    s.loads.push_back(l);
    old.studies.push_back(s);
    return s.id;
  };
  using R = fem::ParticipantRole;
  const uint64_t s1 = study("A on B", {{oa, R::Meshed, ExprValue{0.0}},
                                       {ob, R::Rigid, ExprValue{0.0}}});
  const uint64_t s2 = study("B on C", {{ob, R::Meshed, ExprValue{0.0}},
                                       {oc, R::Rigid, ExprValue{0.0}}});
  // The first table again, in the other order: the same table.
  const uint64_t s3 =
      study("A on B, heavier",
            {{ob, R::Rigid, ExprValue{0.0}}, {oa, R::Meshed, ExprValue{0.0}}});
  {
    fem::StudyRuntime rt;
    rt.inputHash = 42;
    rt.maxVonMises = 12.5;
    rt.bodies.resize(1);
    old.results[s3] = rt;
  }
  const auto sims = fem::SimulationsFromAssembly(old, 9, "Frame");
  REQUIRE(sims.size() == 2);
  CHECK(sims[0].name == "Frame simulation");
  CHECK(sims[1].name == "Frame simulation 2");
  const fem::SimDocument& first = sims[0].sim;
  CHECK(first.Subject().kind == fem::SubjectKind::Assembly);
  CHECK(first.Subject().docId == 9);
  REQUIRE(first.GetStudies().size() == 2);
  CHECK(first.FindStudy(s1) != nullptr);
  CHECK(first.FindStudy(s3) != nullptr);
  CHECK(sims[1].sim.FindStudy(s2) != nullptr);
  CHECK(first.Roles().size() == 2);
  CHECK(first.FindRole(oa)->role == R::Meshed);
  CHECK(sims[1].sim.FindRole(oc)->role == R::Rigid);
  // The document's roles stand for each study's table, and are what it is
  // planned with.
  CHECK(first.FindStudy(s1)->participants.empty());
  CHECK(first.Effective(*first.FindStudy(s1)).participants.size() == 2);
  // Ids kept: a new one is past every study and load moved.
  for (const fem::Study& s : first.GetStudies())
    for (const fem::Load& l : s.loads)
      CHECK(first.NextId() > l.id);
  // The result moved with its study, and reads stale until restamped.
  REQUIRE(first.StudyResults(s3) != nullptr);
  CHECK(first.StudyResults(s3)->inputHash == 42);
  CHECK(first.StudyResultsStale(s3, [](uint64_t) { return 0; }));
  CHECK(first.StudyResults(s1) == nullptr);

  // A requirement's {document, study} still finds it.
  std::vector<const fem::SimDocument*> all{&sims[0].sim, &sims[1].sim};
  CHECK(fem::SimulationHoldingStudy(all, 9, s2) == &sims[1].sim);
  CHECK(fem::SimulationHoldingStudy(all, 9, s3) == &sims[0].sim);
  CHECK(fem::SimulationHoldingStudy(all, 8, s3) == nullptr);
  CHECK(fem::SimulationsFromAssembly(fem::OldStudies{}, 9, "X").empty());
}

#if defined(HAVE_NETGEN) && defined(HAVE_FEM_SOLVER)

TEST_CASE("A simulation with a mesh and a result round-trips, and reopened "
          "solves on the stored mesh",
          "[fem][simdoc][io]") {
  Bar bar;
  fem::SimDocument sim;
  sim.SetSubject({fem::SubjectKind::Part, 1, 0, "Bar"});
  sim.SetMesh({ExprValue{4.0}, {}});
  const uint64_t id = AddStudy(sim, bar.Bend());
  fem::StudyRuntime rt;
  std::string err;
  int made = -1;
  REQUIRE(fem::RunSimStudy(sim, bar.Context(), id, rt, &err, &made));
  INFO(err);
  REQUIRE(made == 1);
  REQUIRE(sim.KeptMeshKeys().size() == 1);
  // The study is on the document's mesh, so that mesh is the document's.
  CHECK(sim.DocumentMeshKeys() == sim.KeptMeshKeys());
  const uint64_t key = sim.KeptMeshKeys()[0];

  for (const char* form : {"bar.json", "bar.lob"}) {
    INFO(form);
    const fs::path dir = Scratch("rt");
    const fs::path path = dir / form;
    REQUIRE(DocumentIO::SaveSimulation(sim, path.string()));
    if (std::string(form).ends_with(".lob")) {
      CHECK(fs::exists(path / "index.json"));
      CHECK(
          fs::exists(path / "results" / (std::to_string(id) + ".result.json")));
      CHECK(fs::exists(path / "meshes"));
    } else {
      const nlohmann::json j = ReadJson(path);
      CHECK(j.at("kind") == "simulation");
      CHECK(j.at("meshes").size() == 1);
      CHECK(j.at("results").size() == 1);
    }
    // An unchanged re-save writes the same bytes (a re-save of a big mesh
    // is free).
    REQUIRE(DocumentIO::SaveSimulation(sim, path.string()));

    auto loaded = DocumentIO::LoadSimulation(path.string());
    REQUIRE(loaded.has_value());
    fem::SimDocument& back = **loaded;
    CHECK(back.Subject().docId == 1);
    CHECK(back.Mesh().size.value == 4.0);
    REQUIRE(back.FindStudy(id) != nullptr);
    CHECK(back.FindStudy(id)->loads.size() == 2);
    CHECK(back.DocumentMeshKeys() == std::vector<uint64_t>{key});
    REQUIRE(back.FindMesh(key) != nullptr);
    const fem::FemMesh& m = *back.FindMesh(key)->mesh;
    const fem::FemMesh& was = *sim.FindMesh(key)->mesh;
    CHECK(m.nodes == was.nodes); // f64: exactly what was solved on
    CHECK(m.tets == was.tets);
    CHECK(m.boundary.size() == was.boundary.size());
    CHECK(m.stats.worstQuality == was.stats.worstQuality);
    REQUIRE(back.StudyResults(id) != nullptr);
    CHECK(back.StudyResults(id)->meshKeys == std::vector<uint64_t>{key});
    CHECK(back.StudyResults(id)->maxVonMises == rt.maxVonMises);

    // Stale until it is handed its subject; current once the inputs hash as
    // they did.
    const auto stamps = [](uint64_t) { return uint64_t{7}; };
    CHECK(back.StudyResultsStale(id, stamps));
    CHECK(fem::RestampSimByInputs(back, bar.Context()) == 1);
    CHECK_FALSE(back.StudyResultsStale(id, stamps));

    // Run again: the stored mesh, no meshing, the same answer.
    fem::StudyRuntime again;
    REQUIRE(fem::RunSimStudy(back, bar.Context(), id, again, &err, &made));
    CHECK(made == 0);
    CHECK(again.maxVonMises == rt.maxVonMises);
    fs::remove_all(dir);
  }

  SECTION("a part that moved leaves the reopened result stale") {
    const fs::path dir = Scratch("moved");
    REQUIRE(DocumentIO::SaveSimulation(sim, (dir / "s.lob").string()));
    auto loaded = DocumentIO::LoadSimulation((dir / "s.lob").string());
    REQUIRE(loaded.has_value());
    auto* box = dynamic_cast<BoxFeature*>(bar.doc.GetFeature(0));
    REQUIRE(box != nullptr);
    box->width = 81.0;
    bar.doc.Regenerate();
    CHECK(fem::RestampSimByInputs(**loaded, bar.Context()) == 0);
    CHECK((*loaded)->StudyResultsStale(id, [](uint64_t) { return 7; }));
    fs::remove_all(dir);
  }

  SECTION("ClearStorage takes the meshes and results out of the next save") {
    const fs::path dir = Scratch("clear");
    const fs::path lob = dir / "s.lob";
    REQUIRE(DocumentIO::SaveSimulation(sim, lob.string()));
    REQUIRE(fs::exists(lob / "meshes"));
    fem::SimDocument& cleared = sim;
    const auto [meshes, results] = cleared.ClearStorage();
    CHECK(meshes == 1);
    CHECK(results == 1);
    REQUIRE(DocumentIO::SaveSimulation(cleared, lob.string()));
    CHECK_FALSE(fs::exists(lob / "meshes"));
    CHECK_FALSE(fs::exists(lob / "results"));
    REQUIRE(DocumentIO::SaveSimulation(cleared, (dir / "s.json").string()));
    CHECK_FALSE(ReadJson(dir / "s.json").contains("meshes"));
    auto back = DocumentIO::LoadSimulation(lob.string());
    REQUIRE(back.has_value());
    CHECK((*back)->Meshes().empty());
    CHECK((*back)->ResultIds().empty());
    CHECK((*back)->GetStudies().size() == 1); // the studies stay
    fs::remove_all(dir);
  }

  SECTION("a damaged mesh or result is dropped and reported, the document "
          "opens") {
    const fs::path dir = Scratch("damaged");
    const fs::path lob = dir / "s.lob";
    REQUIRE(DocumentIO::SaveSimulation(sim, lob.string()));
    fs::path bin;
    for (const auto& e : fs::directory_iterator(lob / "meshes"))
      if (e.path().extension() == ".bin")
        bin = e.path();
    REQUIRE(!bin.empty());
    fs::resize_file(bin, fs::file_size(bin) / 2);
    // A result for a study the document does not have.
    fs::copy_file(lob / "results" / (std::to_string(id) + ".result.json"),
                  lob / "results" / "999.result.json");
    fs::copy_file(lob / "results" / (std::to_string(id) + ".result.bin"),
                  lob / "results" / "999.result.bin");
    readctx::CollectDrops collect;
    auto back = DocumentIO::LoadSimulation(lob.string());
    REQUIRE(back.has_value());
    CHECK((*back)->Meshes().empty());
    CHECK((*back)->StudyResults(id) != nullptr);
    CHECK((*back)->StudyResults(999) == nullptr);
    const readctx::LoadReport report = collect.Take();
    CHECK_THAT(report.Summary(), ContainsSubstring("mesh"));
    CHECK_THAT(report.Summary(), ContainsSubstring("study 999"));
    // Run: it meshes again.
    REQUIRE(fem::RunSimStudy(**back, bar.Context(), id, rt, &err, &made));
    CHECK(made == 1);
    fs::remove_all(dir);
  }
}

TEST_CASE("A part's studies move into one simulation whose results stay "
          "current",
          "[fem][simdoc][migration]") {
  Bar bar;
  // As the reader keeps an old part file's studies and results.
  fem::OldStudies old;
  fem::Study setup = bar.Bend();
  setup.id = NewStudy(bar.doc).id;
  setup.meshSize = 4.0;
  fem::Refinement r;
  r.face = bar.Face(1);
  r.size = ExprValue{2.0};
  setup.refinements.push_back(r);
  const uint64_t id = setup.id;
  old.studies.push_back(setup);
  const uint64_t unrun =
      old.studies.emplace_back(NewStudy(bar.doc, "Unrun")).id;
  fem::StudyRuntime rt;
  std::string err;
  REQUIRE(fem::RunStudy(bar.doc, old.studies[0], rt, &err));
  INFO(err);
  rt.inputHash = fem::StudyInputKey(bar.doc, old.studies[0]);
  old.results[id] = rt;

  auto sims = fem::SimulationsFromPart(old, 1, "Bar");
  REQUIRE(sims.size() == 1);
  CHECK(sims[0].name == "Bar simulation");
  fem::SimDocument& sim = sims[0].sim;
  CHECK(sim.Subject().kind == fem::SubjectKind::Part);
  CHECK(sim.Subject().docId == 1);
  CHECK(sim.Subject().configuration == 0);
  // The document's mesh stays auto; each study keeps its own as overrides.
  CHECK(sim.Mesh().size.value == 0.0);
  const fem::Study* moved = sim.FindStudy(id);
  REQUIRE(moved != nullptr);
  CHECK(moved->meshSize.value == 4.0);
  CHECK(moved->refinements.size() == 1);
  CHECK(sim.FindStudy(unrun) != nullptr);
  CHECK(sim.NextId() > 102);

  // The result is current against the part as it is: the migrated study
  // hashes as the part's study did.
  CHECK(sim.StudyResultsStale(id, [](uint64_t) { return 7; }));
  CHECK(fem::SimStudyInputKey(
            [&] {
              fem::SimContext c = bar.Context();
              c.sim = &sim;
              return c;
            }(),
            id) == rt.inputHash);
  CHECK(fem::RestampSimByInputs(sim, bar.Context()) == 1);
  CHECK_FALSE(sim.StudyResultsStale(id, [](uint64_t) { return 7; }));
  CHECK(sim.StudyResults(unrun) == nullptr);

  // A requirement's {document 1, study id} finds it.
  CHECK(fem::SimulationHoldingStudy({&sim}, 1, id) == &sim);
  // A part with no studies has no simulation to make.
  CHECK(fem::SimulationsFromPart(fem::OldStudies{}, 2, "Empty").empty());
}

#endif // HAVE_NETGEN && HAVE_FEM_SOLVER
