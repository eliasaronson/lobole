// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

// A full result (keep: full, docs/simulation/README.md#a-full-result): the
// interior field kept against the mesh it was solved on, saved and read
// back value for value, so a reopened result still has every element to
// cut, sample and export; a convergence run's refinement kept with the
// result as a child of the mesh it started from; and a surface result as
// it was.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "core/Document.h"
#include "core/Feature.h"
#include "core/fem/FemSolver.h"
#include "core/fem/FemVtu.h"
#include "core/fem/SimDocument.h"
#include "core/fem/SimStudy.h"
#include "core/io/ReadContext.h"
#include "core/io/SerializeSimulation.h"

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
      ("lobole-simfull-" + std::to_string(::getpid()) + "-" + name);
  fs::remove_all(p);
  fs::create_directories(p);
  return p;
}

// An 80 x 10 x 10 steel bar clamped at one end and pushed down at the other.
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
  fem::Study Bend(fem::KeepMode keep, fem::ConvergeMode conv) const {
    fem::Study s;
    s.name = "Bend";
    s.keep = keep;
    s.converge = conv;
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

size_t SavedResultBytes(const fem::SimDocument& sim, uint64_t id) {
  nlohmann::json j;
  REQUIRE(DocumentIO::SimResultToJson(sim, *sim.StudyResults(id), j));
  return j.at("bin_bytes").get<size_t>() + j.dump().size();
}

} // namespace

#ifdef HAVE_NETGEN

TEST_CASE("A full result keeps its interior against the stored mesh, and "
          "reopens with it",
          "[fem][simdoc][io][keepfull]") {
  Bar bar;
  fem::SimDocument sim;
  sim.SetSubject({fem::SubjectKind::Part, 1, 0, "Bar"});
  sim.SetMesh({ExprValue{4.0}, {}});
  const uint64_t full =
      AddStudy(sim, bar.Bend(fem::KeepMode::Full, fem::ConvergeMode::Never));
  const uint64_t surface =
      AddStudy(sim, bar.Bend(fem::KeepMode::Surface, fem::ConvergeMode::Never));
  fem::StudyRuntime rt;
  std::string err;
  REQUIRE(fem::RunSimStudy(sim, bar.Context(), full, rt, &err));
  INFO(err);
  fem::StudyRuntime rs;
  REQUIRE(fem::RunSimStudy(sim, bar.Context(), surface, rs, &err));
  // One mesh: both studies are on the document's.
  REQUIRE(sim.KeptMeshKeys().size() == 1);
  const uint64_t key = sim.KeptMeshKeys()[0];

  const fem::StudyRuntime& kept = *sim.StudyResults(full);
  REQUIRE(kept.keepFull);
  const fem::StudyBodyResult& b = kept.bodies.at(0);
  CHECK(b.meshKey == key);
  CHECK(b.mesh.tets.size() == sim.FindMesh(key)->mesh->tets.size());
  CHECK(b.mesh.nodes.size() == sim.FindMesh(key)->mesh->nodes.size());
  CHECK(b.results.displacement.size() == b.mesh.nodes.size());
  CHECK(fem::VtuRefusal(kept).empty());
  // The surface result is what it was: no tets.
  CHECK(sim.StudyResults(surface)->bodies.at(0).mesh.tets.empty());
  CHECK_FALSE(fem::VtuRefusal(*sim.StudyResults(surface)).empty());
  // Its interior's fields only, the mesh written once: under three times
  // the surface result, against the mesh's own bytes beside it.
  const size_t fullBytes = SavedResultBytes(sim, full);
  const size_t surfaceBytes = SavedResultBytes(sim, surface);
  INFO("full " << fullBytes << " B, surface " << surfaceBytes << " B");
  CHECK(fullBytes > surfaceBytes);

  for (const char* form : {"bar.lob", "bar.json"}) {
    INFO(form);
    const fs::path dir = Scratch("rt");
    const fs::path path = dir / form;
    REQUIRE(DocumentIO::SaveSimulation(sim, path.string()));
    if (std::string(form).ends_with(".lob")) {
      const nlohmann::json rj =
          ReadJson(path / "results" / (std::to_string(full) + ".result.json"));
      CHECK(rj.at("keep") == "full");
      const nlohmann::json& bj = rj.at("bodies").at(0);
      CHECK(bj.contains("mesh"));
      CHECK_FALSE(bj.contains("nodes"));
      CHECK_FALSE(bj.contains("tets"));
    }
    auto loaded = DocumentIO::LoadSimulation(path.string());
    REQUIRE(loaded.has_value());
    fem::SimDocument& back = **loaded;
    const fem::StudyRuntime* r = back.StudyResults(full);
    REQUIRE(r != nullptr);
    CHECK(r->keepFull);
    const fem::StudyBodyResult& rb = r->bodies.at(0);
    // Value for value what the run left in memory.
    CHECK(rb.mesh.nodes == b.mesh.nodes);
    CHECK(rb.mesh.tets == b.mesh.tets);
    CHECK(rb.results.displacement == b.results.displacement);
    CHECK(rb.results.vonMises == b.results.vonMises);
    CHECK(rb.results.stress == b.results.stress);
    CHECK(rb.meshKey == key);
    CHECK(fem::VtuRefusal(*r).empty());
    CHECK(back.StudyResults(surface)->bodies.at(0).mesh.tets.empty());
    fs::remove_all(dir);
  }

  SECTION("a full result whose mesh did not read is dropped with it") {
    const fs::path dir = Scratch("damaged");
    const fs::path lob = dir / "s.lob";
    REQUIRE(DocumentIO::SaveSimulation(sim, lob.string()));
    for (const auto& e : fs::directory_iterator(lob / "meshes"))
      if (e.path().extension() == ".bin")
        fs::resize_file(e.path(), fs::file_size(e.path()) / 2);
    readctx::CollectDrops collect;
    auto back = DocumentIO::LoadSimulation(lob.string());
    REQUIRE(back.has_value());
    CHECK((*back)->StudyResults(full) == nullptr);
    CHECK((*back)->StudyResults(surface) != nullptr);
    CHECK_THAT(collect.Take().Summary(), ContainsSubstring("not kept"));
    fs::remove_all(dir);
  }
}

TEST_CASE("A converged full result keeps its refined mesh as a child",
          "[fem][simdoc][io][keepfull][slow]") {
  Bar bar;
  fem::SimDocument sim;
  sim.SetSubject({fem::SubjectKind::Part, 1, 0, "Bar"});
  sim.SetMesh({ExprValue{4.0}, {}});
  const uint64_t id =
      AddStudy(sim, bar.Bend(fem::KeepMode::Full, fem::ConvergeMode::Always));
  fem::StudyRuntime rt;
  std::string err;
  REQUIRE(fem::RunSimStudy(sim, bar.Context(), id, rt, &err));
  INFO(err);
  const fem::StudyRuntime& kept = *sim.StudyResults(id);
  REQUIRE(kept.convergence.steps.size() > 1); // it refined
  const uint64_t key = sim.KeptMeshKeys().at(0);
  const fem::StudyBodyResult& b = kept.bodies.at(0);
  CHECK(b.meshKey == 0);
  CHECK(b.parentMeshKey == key);
  CHECK(b.mesh.tets.size() > sim.FindMesh(key)->mesh->tets.size());
  const fs::path dir = Scratch("child");
  REQUIRE(DocumentIO::SaveSimulation(sim, (dir / "s.lob").string()));
  const nlohmann::json rj = ReadJson(dir / "s.lob" / "results" /
                                     (std::to_string(id) + ".result.json"));
  const nlohmann::json& bj = rj.at("bodies").at(0);
  CHECK(bj.contains("tets"));
  CHECK(bj.contains("parent_mesh"));
  CHECK_FALSE(bj.contains("mesh"));
  auto back = DocumentIO::LoadSimulation((dir / "s.lob").string());
  REQUIRE(back.has_value());
  const fem::StudyBodyResult& rb = (*back)->StudyResults(id)->bodies.at(0);
  CHECK(rb.mesh.tets == b.mesh.tets);
  CHECK(rb.results.vonMises == b.results.vonMises);
  CHECK(rb.parentMeshKey == key);
  fs::remove_all(dir);
}

#endif // HAVE_NETGEN

// Refused on the way in, naming what is accepted: a keep that is not one of
// the two, a keep on a topology study, and a body that names a stored mesh
// in a result that is not full.
TEST_CASE("A study's keep is surface or full", "[fem][simdoc][keepfull]") {
  fem::KeepMode k;
  CHECK(fem::KeepModeFromString("full", k));
  CHECK(k == fem::KeepMode::Full);
  CHECK(fem::KeepModeFromString("surface", k));
  CHECK_FALSE(fem::KeepModeFromString("whole", k));
  fem::SimDocument sim;
  sim.SetSubject({fem::SubjectKind::Part, 3, 0, "Bracket"});
  fem::Study s;
  s.id = 5;
  s.name = "Load";
  s.keep = fem::KeepMode::Full;
  sim.RestoreStudy(s);
  const fs::path dir = Scratch("keep");
  REQUIRE(DocumentIO::SaveSimulation(sim, (dir / "s.json").string()));
  nlohmann::json j = ReadJson(dir / "s.json");
  CHECK(j.at("studies").at(0).at("keep") == "full");
  const auto refusal = [&](nlohmann::json bad) {
    std::ofstream(dir / "bad.json") << bad.dump(2);
    auto r = DocumentIO::LoadSimulation((dir / "bad.json").string());
    REQUIRE_FALSE(r.has_value());
    return r.error();
  };
  nlohmann::json whole = j;
  whole["studies"][0]["keep"] = "whole";
  CHECK_THAT(refusal(whole), ContainsSubstring("surface, full"));
  nlohmann::json topo = j;
  topo["studies"][0]["analysis"] = "topology";
  CHECK_THAT(refusal(topo), ContainsSubstring("topology"));
  fs::remove_all(dir);
}
