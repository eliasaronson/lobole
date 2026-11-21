// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

// A project's simulation documents: tabs of their own (DocKind::Simulation),
// saved as simulations/<folder>/
// (docs/simulation/README.md#how-a-simulation-is-saved).

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "core/Command.h"
#include "core/Document.h"
#include "core/Feature.h"
#include "core/Occurrence.h"
#include "core/fem/FemSolver.h" // StudyRuntime
#include "core/fem/SimDocument.h"
#include "core/io/Autosaver.h"
#include "ui/AppActionsInternal.h" // SlotSim
#include "ui/AppState.h"
#include "ui/BackgroundJobs.h"
#include "ui/DocumentSlots.h"
#include "ui/ProjectActions.h"
#include "ui/SimRunActions.h"
#include "ui/SimulationActions.h"
#include "ui/SimulationView.h" // MeshReport
#include "ui/StudyHost.h"
#include "ui/VariableActions.h" // GlobalUndo

#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <thread>
#include <unistd.h>

using Catch::Matchers::ContainsSubstring;
namespace fs = std::filesystem;

namespace {

fs::path Scratch(const char* name) {
  const fs::path p =
      fs::temp_directory_path() /
      ("lobole-simproject-" + std::to_string(::getpid()) + "-" + name);
  std::error_code ec;
  fs::remove_all(p, ec);
  fs::create_directories(p);
  return p;
}

ui::AppState StateWithBox() {
  ui::AppState state;
  state.document = std::make_unique<Document>();
  state.commands = std::make_unique<CommandStack>();
  ui::EnsureParts(state);
  state.document->AddFeature(
      std::make_unique<BoxFeature>("Box", glm::vec3(0), 10, 10, 10));
  return state;
}

fem::SimDocument SimOf(uint64_t partId) {
  fem::SimDocument sim;
  sim.SetSubject({fem::SubjectKind::Part, partId, 0, "Part 1"});
  fem::Study& s = sim.AddStudy("Bend");
  (void)s;
  return sim;
}

nlohmann::json Documents(const fs::path& lob) {
  std::ifstream in(lob / "project.json");
  return nlohmann::json::parse(in).at("documents");
}

// `mark`: as every open in the app ends (MarkProjectOpened, which stamps
// the disk); a look that is not the project's owner leaves it.
std::unique_ptr<ui::AppState> Reopen(const fs::path& lob, bool mark = false) {
  auto state = std::make_unique<ui::AppState>();
  std::string error;
  REQUIRE(ui::OpenProject(*state, lob.string(), &error));
  INFO(error);
  if (mark)
    ui::MarkProjectOpened(*state, lob.string());
  return state;
}

// A save as the app makes one: SaveProject, then MarkProjectSaved.
bool SaveOk(ui::AppState& state, const fs::path& lob) {
  std::string error;
  const bool ok = ui::SaveProject(state, lob.string(), &error);
  UNSCOPED_INFO(error);
  if (ok)
    ui::MarkProjectSaved(state, lob.string(), "save");
  return ok;
}

} // namespace

TEST_CASE("A simulation is a tab that saves and reopens",
          "[fem][simdoc][project]") {
  const fs::path root = Scratch("keep");
  const fs::path lob = root / "p.lob";
  ui::AppState state = StateWithBox();
  ui::AddPart(state, "Second");
  const uint64_t partId = state.parts[0].id;
  std::string why;
  const size_t at = ui::AddSimulation(state, "Bend check", SimOf(partId), &why);
  REQUIRE(at == 2);
  CHECK(state.activePart == 2); // switched to, as a new job is
  CHECK(state.parts[2].kind == ui::DocKind::Simulation);
  REQUIRE(state.sim != nullptr);
  CHECK(state.document == nullptr);
  CHECK(state.parts[2].id > partId);
  CHECK(ui::SlotSim(state, 2) == state.sim.get());
  CHECK(ui::SlotSim(state, 0) == nullptr);
  // A name another tab has is refused, naming it, and changes nothing.
  CHECK(ui::AddSimulation(state, state.parts[0].name, SimOf(partId), &why) ==
        SIZE_MAX);
  CHECK_THAT(why, ContainsSubstring("already exists"));
  CHECK(state.parts.size() == 3);
  CHECK(state.activePart == 2);
  // A tab folder, as any tab may have.
  ui::MoveToFolder(state, 2, "Analyses");
  std::string error;
  REQUIRE(ui::SaveProject(state, lob.string(), &error));
  INFO(error);
  REQUIRE(fs::exists(lob / "simulations" / "Bend check" / "index.json"));
  const nlohmann::json docs = Documents(lob);
  REQUIRE(docs.size() == 3);
  CHECK(docs[2].at("type") == "simulation");
  CHECK(docs[2].at("dir") == "simulations/Bend check");
  CHECK(docs[2].at("folder") == "Analyses");

  auto back = Reopen(lob, true);
  REQUIRE(back->parts.size() == 3);
  CHECK(back->parts[2].kind == ui::DocKind::Simulation);
  CHECK(back->parts[2].name == "Bend check");
  CHECK(back->parts[2].folder == "Analyses");
  CHECK(back->parts[2].id == state.parts[2].id);
  CHECK(back->activePart == 2); // it was in front when saved
  REQUIRE(back->sim != nullptr);
  CHECK(back->sim->Subject().docId == partId);
  CHECK(back->sim->GetStudies().size() == 1);
  CHECK_FALSE(ui::ProjectDirty(*back));
  CHECK(back->nextPartId > back->parts[2].id);
  // Its subject's bodies are what the tab draws.
  CHECK(ui::ViewportBodies(*back).size() == 1);

  // Stashed behind another tab and saved again: still there.
  ui::SetActivePart(*back, 0);
  REQUIRE(back->parts[2].sim != nullptr);
  REQUIRE(SaveOk(*back, lob));
  CHECK(Reopen(lob)->parts.size() == 3);

  // Renamed as any tab is: listed under its new folder. Deleted: gone.
  REQUIRE(ui::RenameDocument(*back, 2, "Renamed", &why));
  CHECK(ui::ProjectDirty(*back));
  REQUIRE(SaveOk(*back, lob));
  CHECK(Documents(lob)[2].at("dir") == "simulations/Renamed");
  CHECK(Reopen(lob)->parts[2].name == "Renamed");
  REQUIRE(ui::DeletePart(*back, 2, &why));
  REQUIRE(SaveOk(*back, lob));
  CHECK(Documents(lob).size() == 2);
  CHECK(Reopen(lob)->parts.size() == 2);
  std::error_code ec;
  fs::remove_all(root, ec);
}

// Before a simulation was a tab, a project listed its simulations after
// every tab and `active` counted the tabs alone: the same index into the
// whole list, so such a file opens with the same tab in front.
TEST_CASE("A project written before simulations were tabs opens as it was",
          "[fem][simdoc][project]") {
  const fs::path root = Scratch("active");
  const fs::path lob = root / "p.lob";
  ui::AppState state = StateWithBox();
  ui::AddPart(state, "Second");
  REQUIRE(ui::AddSimulation(state, "Sim", SimOf(state.parts[0].id)) == 2);
  std::string error;
  REQUIRE(ui::SaveProject(state, lob.string(), &error));
  // The old shape exactly: the simulation last and folderless, `active`
  // naming the second tab.
  nlohmann::json index;
  {
    std::ifstream in(lob / "project.json");
    index = nlohmann::json::parse(in);
  }
  REQUIRE(index.at("documents").size() == 3);
  REQUIRE(index.at("documents")[2].at("type") == "simulation");
  index["active"] = 1;
  std::ofstream(lob / "project.json") << index.dump(2);
  auto back = Reopen(lob);
  CHECK(back->activePart == 1);
  CHECK(back->parts[1].name == "Second");
  REQUIRE(back->parts.size() == 3);
  CHECK(back->parts[2].kind == ui::DocKind::Simulation);
  REQUIRE(back->parts[2].sim != nullptr);
  CHECK(back->parts[2].sim->GetStudies().size() == 1);
  std::error_code ec;
  fs::remove_all(root, ec);
}

TEST_CASE("OpenSimulationFor finds a part's simulation or makes one",
          "[fem][simdoc][tabs]") {
  ui::AppState state = StateWithBox();
  REQUIRE(state.document->GetBodies().size() == 1);
  const uint64_t partId = state.parts[0].id;
  CHECK(ui::FindSimulationFor(state, fem::SubjectKind::Part, partId) ==
        SIZE_MAX);
  std::string why;
  const size_t made = ui::OpenSimulationFor(state, 0, &why);
  INFO(why);
  REQUIRE(made == 1);
  CHECK(state.activePart == 1);
  CHECK(state.parts[1].name == "Part 1 simulation");
  REQUIRE(state.sim != nullptr);
  CHECK(state.sim->Subject().kind == fem::SubjectKind::Part);
  CHECK(state.sim->Subject().docId == partId);
  CHECK(state.sim->Subject().configuration == 0);
  CHECK(state.sim->Subject().name == "Part 1");
  CHECK(ui::FindSimulationFor(state, fem::SubjectKind::Part, partId) == 1);
  // The tab draws its subject's body, from the stashed part.
  CHECK(ui::ViewportBodies(state).size() == 1);
  const uint64_t drawnAt = ui::SimRenderVersion(state);
  CHECK(drawnAt != 0);

  // A second ask switches to it rather than making another.
  ui::SetActivePart(state, 0);
  CHECK(ui::OpenSimulationFor(state, 0, &why) == 1);
  CHECK(state.activePart == 1);
  CHECK(state.parts.size() == 2);

  // An edit to the subject moves the tab's picture.
  ui::SetActivePart(state, 0);
  state.document->AddFeature(
      std::make_unique<BoxFeature>("Box 2", glm::vec3(20, 0, 0), 5, 5, 5));
  ui::SetActivePart(state, 1);
  CHECK(ui::SimRenderVersion(state) != drawnAt);
  CHECK(ui::ViewportBodies(state).size() == 2);

  // An assembly's simulation names the assembly; a simulation is no subject.
  ui::AddAssembly(state, "Rig");
  const size_t rig = static_cast<size_t>(state.activePart);
  const size_t rigSim = ui::OpenSimulationFor(state, rig, &why);
  REQUIRE(rigSim == 3);
  CHECK(state.sim->Subject().kind == fem::SubjectKind::Assembly);
  CHECK(state.sim->Subject().docId == state.parts[rig].id);
  CHECK(ui::ViewportBodies(state).empty()); // nothing placed in it yet
  CHECK(ui::OpenSimulationFor(state, rigSim, &why) == SIZE_MAX);
  CHECK_THAT(why, ContainsSubstring("not a part or an assembly"));
  // A name another tab already has gets a number.
  ui::AppState other = StateWithBox();
  ui::AddPart(other, "Part 1 simulation");
  REQUIRE(ui::OpenSimulationFor(other, 0, &why) == 2);
  CHECK(other.parts[2].name == "Part 1 simulation 2");
}

TEST_CASE("A simulation edit under its guard undoes and redoes",
          "[fem][simdoc][undo]") {
  ui::AppState state = StateWithBox();
  REQUIRE(ui::AddSimulation(state, "Sim", fem::SimDocument{}) == 1);
  const uint64_t before = state.sim->Version();
  {
    ui::SimUndoGuard outer(state);
    {
      ui::SimUndoGuard inner(state); // only the outermost records
      state.sim->AddStudy("Bend");
    }
  }
  REQUIRE(state.simHistory.undo.size() == 1);
  CHECK(state.sim->GetStudies().size() == 1);
  // A guard over no edit records nothing.
  {
    ui::SimUndoGuard idle(state, false, /*compareContent=*/true);
  }
  CHECK(state.simHistory.undo.size() == 1);
  CHECK(ui::CanGlobalUndo(state));

  ui::GlobalUndo(state); // the active tab's history, as a job's is
  CHECK(state.sim->GetStudies().empty());
  CHECK(state.sim->Version() > before + 1); // past every version it had
  REQUIRE(ui::RedoSim(state));
  REQUIRE(state.sim->GetStudies().size() == 1);
  CHECK(state.sim->GetStudies()[0].name == "Bend");

  // An edit through write access the setters do not see, said so.
  {
    ui::SimUndoGuard guard(state);
    state.sim->StudyForRun(state.sim->GetStudies()[0].id)->name = "Renamed";
    ui::MarkSimEdited(state);
  }
  CHECK(state.simHistory.undo.size() == 2);
  REQUIRE(ui::UndoSim(state));
  CHECK(state.sim->GetStudies()[0].name == "Bend");

  // The history goes with its tab and comes back with it.
  ui::SetActivePart(state, 0);
  CHECK(state.simHistory.undo.empty());
  ui::SetActivePart(state, 1);
  CHECK(state.simHistory.undo.size() == 1);
}

TEST_CASE("A duplicated simulation keeps results of its own",
          "[fem][simdoc][tabs][duplicate]") {
  ui::AppState state = StateWithBox();
  const size_t original =
      ui::AddSimulation(state, "Sim", SimOf(state.parts[0].id));
  REQUIRE(original == 1);
  const uint64_t studyId = state.sim->GetStudies()[0].id;
  state.sim->StoreStudyResults(studyId, fem::StudyRuntime{});
  const size_t copy = ui::DuplicateDocument(state, original);
  REQUIRE(copy == 2);
  CHECK(state.parts[copy].kind == ui::DocKind::Simulation);
  CHECK(state.parts[copy].name == "Sim copy");
  REQUIRE(state.sim != nullptr); // the copy is in front
  const fem::SimDocument* first = state.parts[original].sim.get();
  REQUIRE(first != nullptr);
  CHECK(state.sim.get() != first);
  CHECK(state.sim->Subject().docId == first->Subject().docId);
  // It came with the result the original had...
  CHECK(state.sim->StudyResults(studyId) != nullptr);
  // ...and what it stores or drops from here on is its own.
  state.sim->DropStudyResults(studyId);
  CHECK(state.sim->StudyResults(studyId) == nullptr);
  CHECK(first->StudyResults(studyId) != nullptr);
  fem::Study& added = state.sim->AddStudy("Copy's own");
  state.sim->StoreStudyResults(added.id, fem::StudyRuntime{});
  CHECK(first->StudyResults(added.id) == nullptr);
  CHECK(first->GetStudies().size() == 1);
}

TEST_CASE("A simulation's edits are unsaved, autosaved and recovered",
          "[fem][simdoc][project][savestamp]") {
  const fs::path root = Scratch("recover");
  const fs::path lob = root / "p.lob";
  ui::AppState state = StateWithBox();
  REQUIRE(ui::AddSimulation(state, "Sim", SimOf(state.parts[0].id)) == 1);
  REQUIRE(SaveOk(state, lob));
  CHECK_FALSE(ui::ProjectDirty(state));
  {
    ui::SimUndoGuard guard(state);
    state.sim->AddStudy("Unsaved");
  }
  CHECK(ui::ProjectDirty(state));
  // Dirty from behind another tab too.
  ui::SetActivePart(state, 0);
  CHECK(ui::ProjectDirty(state));

  Autosaver saver(1.0);
  saver.Prime(0.0);
  CHECK(ui::AutosaveProject(state, saver, 10.0) >= 1);
  CHECK(fs::exists(fs::path(Autosaver::SidecarDir(lob.string())) /
                   "simulations" / "Sim.json"));

  ui::AppState recovered;
  std::string error;
  REQUIRE(ui::RecoverProject(recovered, lob.string(), &error));
  INFO(error);
  REQUIRE(recovered.parts.size() == 2);
  const fem::SimDocument* sim = ui::SlotSim(recovered, 1);
  REQUIRE(sim != nullptr);
  REQUIRE(sim->GetStudies().size() == 2);
  CHECK(sim->GetStudies()[1].name == "Unsaved");
  CHECK(ui::ProjectDirty(recovered)); // a sidecar is never a save

  // A save makes it clean.
  REQUIRE(SaveOk(state, lob));
  CHECK_FALSE(ui::ProjectDirty(state));
  Autosaver::Clear(lob.string());
  std::error_code ec;
  fs::remove_all(root, ec);
}

// docs/versioning/README.md#autosave-and-recovery: a stored result is
// derived, and a sidecar keeps the document alone. The result is made by
// hand, as a run would store it, so this holds without the mesher too.
TEST_CASE("A simulation's autosave writes no study result",
          "[fem][simdoc][studyjobs][autosave]") {
  const fs::path root = Scratch("autosave-result");
  const fs::path lob = root / "p.lob";
  ui::AppState state = StateWithBox();
  REQUIRE(ui::AddSimulation(state, "Sim", SimOf(state.parts[0].id)) == 1);
  const uint64_t id = state.sim->GetStudies().front().id;
  fem::StudyRuntime rt;
  rt.topology = true;
  rt.inputHash = 0x1234;
  fem::StudyBodyResult b;
  b.bodyName = "Box";
  for (int i = 0; i < 10; ++i)
    b.mesh.nodes.push_back(glm::dvec3(i, 0.5 * i, 0.25 * i));
  b.mesh.tets.push_back({0, 1, 2, 3, 4, 5, 6, 7, 8, 9});
  b.density = {0.7};
  rt.bodies.push_back(std::move(b));
  state.sim->StoreStudyResults(id, std::move(rt));
  const auto anyResult = [](const fs::path& dir) {
    bool found = false;
    std::error_code ec;
    for (const auto& e : fs::recursive_directory_iterator(dir, ec))
      found |=
          e.path().filename().string().find(".result.") != std::string::npos;
    return found;
  };
  // The control: a real save writes the result, so the autosave's missing
  // one is the autosave's doing.
  REQUIRE(SaveOk(state, lob));
  REQUIRE(anyResult(lob));
  {
    ui::SimUndoGuard guard(state);
    state.sim->AddStudy("Unsaved");
  }
  Autosaver saver(1.0);
  saver.Prime(0.0);
  CHECK(ui::AutosaveProject(state, saver, 10.0) >= 1);
  const fs::path sidecars = Autosaver::SidecarDir(lob.string());
  REQUIRE(fs::exists(sidecars / "simulations" / "Sim.json"));
  CHECK_FALSE(anyResult(sidecars));
  std::ifstream in(sidecars / "simulations" / "Sim.json");
  const std::string text((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());
  CHECK(text.find("density") == std::string::npos);
  CHECK(text.find("input_hash") == std::string::npos);
  Autosaver::Clear(lob.string());
  std::error_code ec;
  fs::remove_all(root, ec);
}

// --- Running a simulation's studies through the app -------------------------
// (docs/simulation/README.md#running-a-simulations-study)

namespace {

// An 80 x 10 x 10 steel bar on the active part tab, as test_sim_document's.
ui::AppState StateWithBar() {
  ui::AppState state;
  state.document = std::make_unique<Document>();
  state.commands = std::make_unique<CommandStack>();
  ui::EnsureParts(state);
  auto box =
      std::make_unique<BoxFeature>("Bar", glm::vec3(0, 0, 0), 80.0, 10.0, 10.0);
  box->SetMaterial("steel");
  state.document->AddFeature(std::move(box));
  return state;
}

// The bar bent: one end clamped, the other pushed down; its loads on
// `occurrence` (0 on a part subject).
fem::Study Bend(const Document& bar, uint64_t occurrence = 0) {
  const uint64_t cb = bar.GetBodies()[0].createdBy;
  const auto face = [cb](int i) {
    return ElementRef{cb, std::to_string(cb) + "/face" + std::to_string(i)};
  };
  fem::Study s;
  s.name = "Bend";
  s.converge = fem::ConvergeMode::Never;
  fem::Load fixed;
  fixed.id = 101;
  fixed.type = fem::LoadType::Fixed;
  fixed.face = face(0);
  fixed.occurrenceId = occurrence;
  s.loads.push_back(fixed);
  fem::Load force;
  force.id = 102;
  force.type = fem::LoadType::Force;
  force.face = face(1);
  force.magnitude = 500.0;
  force.direction = glm::dvec3(0, 0, -1);
  force.occurrenceId = occurrence;
  s.loads.push_back(force);
  return s;
}

uint64_t AddBend(fem::SimDocument& sim, fem::Study setup) {
  const uint64_t id = sim.AddStudy(setup.name).id;
  setup.id = id;
  REQUIRE(sim.ReplaceStudy(std::move(setup)));
  return id;
}

void PumpUntil(ui::AppState& state, const std::function<bool()>& done) {
  const auto t0 = std::chrono::steady_clock::now();
  while (!done() &&
         std::chrono::steady_clock::now() - t0 < std::chrono::seconds(120)) {
    ui::PollBackgroundJobs(state);
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
}

} // namespace

#ifdef HAVE_NETGEN
TEST_CASE("A simulation tab's study runs through the app and lands in it",
          "[fem][simdoc][jobs]") {
  ui::AppState state = StateWithBar();
  const uint64_t partId = state.parts[0].id;
  fem::SimDocument made;
  made.SetSubject({fem::SubjectKind::Part, partId, 0, "Part 1"});
  made.SetMesh({ExprValue{4.0}, {}});
  const uint64_t studyId = AddBend(made, Bend(*state.document));
  REQUIRE(ui::AddSimulation(state, "Sim", std::move(made)) == 1);
  const uint64_t simSlot = state.parts[1].id;

  // The host on the tab is the simulation.
  const ui::StudyHost host = ui::StudyHostOf(state);
  REQUIRE(host.IsSimulation());
  CHECK_FALSE(host.IsAssembly());
  CHECK(host.Find(studyId) != nullptr);
  CHECK(ui::StudyStale(state, studyId)); // nothing solved yet

  // Run from the part's tab: it lands in the simulation behind it.
  ui::SetActivePart(state, 0);
  fem::SimDocument* sim = ui::SimBySlotId(state, simSlot);
  REQUIRE(sim != nullptr);
  CHECK(ui::ActiveSimSlotId(state) == 0);
  std::string err;
  REQUIRE(ui::RunSimStudyAndCache(state, simSlot, studyId, &err));
  INFO(err);
  const fem::StudyRuntime* rt = sim->StudyResults(studyId);
  REQUIRE(rt != nullptr);
  REQUIRE(rt->bodies.size() == 1);
  CHECK(rt->bodies[0].results.maxVonMises > 0.0);
  CHECK(rt->inputHash != 0);
  // Solved against the part, so its edit reads it stale.
  REQUIRE(rt->solvedPartVersions.size() == 1);
  CHECK(rt->solvedPartVersions[0].first == partId);
  CHECK_FALSE(ui::SimStudyStale(state, *sim, studyId));
  CHECK_FALSE(sim->Meshes().empty()); // the mesh it made is kept

  // The same inputs again are answered without a solve.
  ui::JobStart again;
  CHECK(ui::ReuseSimStudyResult(state, simSlot, studyId, &again));
  CHECK(again.reused);
  CHECK(ui::SimStudyInputHash(state, *sim, studyId) == rt->inputHash);
  // A run's own convergence mode is an input.
  ui::JobStart settle;
  settle.converge = static_cast<int>(fem::ConvergeMode::Auto);
  CHECK(ui::SimStudyInputHash(state, *sim, studyId, &settle) != rt->inputHash);

  // Off the frame, started from the part's tab and landing after a switch.
  ui::JobStart fresh;
  fresh.recompute = true;
  REQUIRE(ui::StartSimStudyRun(state, simSlot, studyId, &err, &fresh));
  // Keyed by (simulation, study): the same id on a part reads idle.
  CHECK(ui::GetStudyRunStatus(state, studyId, simSlot).running);
  CHECK_FALSE(ui::GetStudyRunStatus(state, studyId).running);
  CHECK_FALSE(ui::StartSimStudyRun(state, simSlot, studyId, &err, &fresh));
  CHECK_THAT(err, ContainsSubstring("already in progress"));
  const uint64_t epoch = sim->StudyResultsEpoch();
  ui::SetActivePart(state, 1);
  PumpUntil(state, [&] { return !ui::BackgroundJobRunning(state, "study"); });
  sim = ui::SimBySlotId(state, simSlot);
  CHECK(sim->StudyResultsEpoch() > epoch);
  CHECK(sim->StudyResults(studyId)->bodies[0].results.maxVonMises > 0.0);
  CHECK_FALSE(ui::StudyStale(state, studyId));

  // Meshing alone keeps the report under the simulation's slot; the check
  // reads it until the subject moves.
  REQUIRE(ui::MeshSimStudyNow(state, simSlot, studyId, &err));
  const ui::MeshReport* report = ui::SimMeshReportFor(state, simSlot, studyId);
  REQUIRE(report != nullptr);
  REQUIRE(report->bodies.size() == 1);
  CHECK(report->bodies[0].elements > 0);
  const auto hasStaleMesh = [&] {
    for (const fem::StudyProblem& p :
         ui::CheckSimStudyNow(state, simSlot, studyId))
      if (p.code == "stale_mesh")
        return true;
    return false;
  };
  CHECK_FALSE(hasStaleMesh());

  ui::SetActivePart(state, 0);
  state.document->AddFeature(
      std::make_unique<BoxFeature>("Lug", glm::vec3(0, 0, 10), 5, 5, 5));
  sim = ui::SimBySlotId(state, simSlot);
  CHECK(ui::SimStudyStale(state, *sim, studyId));
  CHECK(hasStaleMesh());

  // A topology study runs the topology path only.
  fem::Study topo;
  topo.name = "Topo";
  topo.type = fem::AnalysisType::Topology;
  const uint64_t topoId = AddBend(*sim, topo);
  CHECK_FALSE(ui::StartSimStudyRun(state, simSlot, topoId, &err));
  CHECK_THAT(err, ContainsSubstring("topology path"));
  CHECK_FALSE(ui::StartSimTopologyRun(state, simSlot, studyId, &err));
  CHECK_THAT(err, ContainsSubstring("not a topology study"));

  // A simulation that is gone takes nothing.
  CHECK_FALSE(ui::RunSimStudyAndCache(state, 999999, studyId, &err));
  CHECK_THAT(err, ContainsSubstring("no such simulation"));
}

TEST_CASE("An assembly simulation's study runs on the placed part",
          "[fem][simdoc][jobs]") {
  ui::AppState state = StateWithBar();
  const uint64_t partId = state.parts[0].id;
  const Document& bar = *state.document;
  const fem::Study bend = Bend(bar); // ids before the part is stashed
  ui::AddAssembly(state, "Rig");
  const size_t rig = static_cast<size_t>(state.activePart);
  const uint64_t rigId = state.parts[rig].id;
  Occurrence o;
  o.name = "Bar";
  o.partId = partId;
  o.fixed = true;
  // Placed away from its part's frame: the assembly planner solves it.
  o.position = glm::dvec3(0.0, 40.0, 0.0);
  const uint64_t occ = state.assembly->AddOccurrence(o);
  state.assembly->BumpVersion();

  std::string why;
  const size_t at = ui::OpenSimulationFor(state, rig, &why);
  REQUIRE(at != SIZE_MAX);
  const uint64_t simSlot = state.parts[at].id;
  fem::SimDocument* sim = ui::SimBySlotId(state, simSlot);
  REQUIRE(sim == state.sim.get());
  sim->SetRoles({{occ, fem::ParticipantRole::Meshed, ExprValue{0.0}}});
  sim->SetMesh({ExprValue{4.0}, {}});
  fem::Study study = bend;
  for (fem::Load& l : study.loads)
    l.occurrenceId = occ;
  const uint64_t studyId = AddBend(*sim, study);

  // The viewport's bodies say whose they are, for picking.
  REQUIRE(ui::SimSubjectBodies(state).size() == 1);
  CHECK(ui::SimBodyOccurrence(state, 0) == occ);
  CHECK(ui::SimBodyOccurrence(state, 1) == 0);
  const ui::StudyHost host = ui::StudyHostOf(state);
  CHECK(host.OccurrenceName(occ) == "Bar");

  // The context places the part where the occurrence puts it.
  const fem::SimContext ctx = ui::SimContextFor(state, *sim);
  REQUIRE(ctx.assembly != nullptr);
  const fem::SimPart part = ctx.partOf(occ);
  CHECK(part.doc != nullptr);
  CHECK(part.placement[3][1] == 40.0);
  CHECK(ctx.placedBodies(occ).size() == 1);

  std::string err;
  REQUIRE(ui::RunSimStudyAndCache(state, simSlot, studyId, &err));
  INFO(err);
  const fem::StudyRuntime* rt = sim->StudyResults(studyId);
  REQUIRE(rt != nullptr);
  CHECK(rt->bodies[0].results.maxVonMises > 0.0);
  // Solved against the assembly and the part it places.
  bool sawAssembly = false, sawPart = false;
  for (const auto& [id, stamp] : rt->solvedPartVersions) {
    sawAssembly = sawAssembly || id == rigId;
    sawPart = sawPart || id == partId;
  }
  CHECK(sawAssembly);
  CHECK(sawPart);
  CHECK_FALSE(ui::StudyStale(state, studyId));
  // Moving the occurrence moves what was solved.
  ui::SetActivePart(state, rig);
  state.assembly->FindById(occ)->position = glm::dvec3(0.0, 50.0, 0.0);
  state.assembly->BumpVersion();
  CHECK(ui::SimStudyStale(state, *ui::SimBySlotId(state, simSlot), studyId));
}
#endif

TEST_CASE("An edit on a simulation behind another tab is undone on its tab",
          "[fem][simdoc][undo]") {
  ui::AppState state = StateWithBox();
  REQUIRE(ui::AddSimulation(state, "Sim", SimOf(state.parts[0].id)) == 1);
  const uint64_t simSlot = state.parts[1].id;
  ui::SetActivePart(state, 0);
  fem::SimDocument* sim = ui::SimBySlotId(state, simSlot);
  REQUIRE(sim != nullptr);
  const uint64_t studyId = sim->GetStudies()[0].id;
  const uint64_t before = sim->Version();
  {
    ui::SimSlotUndoGuard outer(state, simSlot);
    ui::SimSlotUndoGuard inner(state, simSlot); // only the outermost records
    sim->AddStudy("Twist");
  }
  CHECK(state.parts[1].simHistory.undo.size() == 1);
  CHECK(sim->Version() > before);
  // An edit the setters do not see is found by its content.
  {
    ui::SimSlotUndoGuard guard(state, simSlot);
    sim->StudyForRun(studyId)->name = "Renamed";
  }
  CHECK(state.parts[1].simHistory.undo.size() == 2);
  // Nothing changed, nothing recorded; the part's own history is untouched.
  {
    ui::SimSlotUndoGuard idle(state, simSlot);
  }
  CHECK(state.parts[1].simHistory.undo.size() == 2);
  CHECK_FALSE(state.commands->CanUndo());

  // On its tab, undo takes both steps back.
  ui::SetActivePart(state, 1);
  REQUIRE(ui::UndoSim(state));
  CHECK(state.sim->FindStudy(studyId)->name == "Bend");
  REQUIRE(ui::UndoSim(state));
  CHECK(state.sim->GetStudies().size() == 1);

  // In front, the guard is the tab's own.
  {
    ui::SimSlotUndoGuard guard(state, simSlot);
    state.sim->AddStudy("Again");
  }
  CHECK(state.simHistory.undo.size() == 1);
  // A slot that is not a simulation records nothing, and does not throw.
  {
    ui::SimSlotUndoGuard none(state, state.parts[0].id);
  }
  CHECK(state.parts[0].simHistory.undo.empty());
}

namespace {
// A study reading `expr` in a load's magnitude.
fem::SimDocument SimReading(fem::SubjectKind kind, uint64_t docId,
                            const std::string& expr) {
  fem::SimDocument sim;
  sim.SetSubject({kind, docId, 0, "subject"});
  fem::Study& s = sim.AddStudy("Bend");
  fem::Load l;
  l.magnitude.expr = expr;
  s.loads.push_back(l);
  return sim;
}
const std::string& LoadExpr(ui::AppState& state, size_t slot) {
  return ui::SlotSim(state, slot)
      ->GetStudies()
      .front()
      .loads.front()
      .magnitude.expr;
}
} // namespace

TEST_CASE("A variable renamed or deleted looks inside every simulation",
          "[fem][simdoc][variables]") {
  ui::AppState state = StateWithBox();
  const uint64_t partId = state.parts[0].id;
  REQUIRE(state.document->SetParameter("t", "2", ""));
  REQUIRE(ui::SetProjectVariable(state, "gap", "3", ""));
  // A second part with a local "gap" that shadows the project's.
  const size_t shadow = ui::AddPart(state, "Shadow");
  const uint64_t shadowId = state.parts[shadow].id;
  REQUIRE(ui::SlotDoc(state, shadow)->SetParameter("gap", "5", ""));
  const size_t rig = ui::AddAssembly(state, "Rig");
  const uint64_t rigId = state.parts[rig].id;

  const size_t partSim = ui::AddSimulation(
      state, "Part sim",
      SimReading(fem::SubjectKind::Part, partId, "t * 10 + gap"));
  fem::SimMesh mesh;
  mesh.size.expr = "gap";
  ui::SlotSim(state, partSim)->SetMesh(mesh);
  const size_t shadowSim = ui::AddSimulation(
      state, "Shadow sim",
      SimReading(fem::SubjectKind::Part, shadowId, "gap * 2"));
  const size_t rigSim = ui::AddSimulation(
      state, "Rig sim",
      SimReading(fem::SubjectKind::Assembly, rigId, "gap * 4"));
  REQUIRE(partSim != SIZE_MAX);
  REQUIRE(shadowSim != SIZE_MAX);
  REQUIRE(rigSim != SIZE_MAX);
  ui::SetActivePart(state, 0);
  Document& part = *ui::SlotDoc(state, 0);
  const auto has = [](const std::vector<std::string>& refs,
                      const std::string& s) {
    return std::find(refs.begin(), refs.end(), s) != refs.end();
  };

  // A part's own variable: its simulation's study reads it.
  std::vector<std::string> refs;
  CHECK_FALSE(ui::RemovePartVariable(state, part, "t", &refs));
  CHECK(has(refs, "Part sim: study 'Bend'"));
  REQUIRE(ui::RenamePartVariable(state, part, "t", "wall"));
  CHECK(LoadExpr(state, partSim) == "wall * 10 + gap");

  // The project's: every simulation that sees it, not one that shadows it.
  refs.clear();
  CHECK_FALSE(ui::RemoveProjectVariable(state, "gap", &refs));
  CHECK(has(refs, "Part sim: study 'Bend'"));
  CHECK(has(refs, "Part sim: simulation mesh"));
  CHECK(has(refs, "Rig sim: study 'Bend'"));
  CHECK_FALSE(has(refs, "Shadow sim: study 'Bend'"));
  REQUIRE(ui::RenameProjectVariable(state, "gap", "clearance"));
  CHECK(LoadExpr(state, partSim) == "wall * 10 + clearance");
  CHECK(ui::SlotSim(state, partSim)->Mesh().size.expr == "clearance");
  CHECK(LoadExpr(state, rigSim) == "clearance * 4");
  CHECK(LoadExpr(state, shadowSim) == "gap * 2"); // its part's own
  // Undoing the project rename puts the simulations back too.
  ui::GlobalUndo(state);
  CHECK(LoadExpr(state, rigSim) == "gap * 4");
  CHECK(LoadExpr(state, partSim) == "wall * 10 + gap");
}
