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
#include "core/fem/FemSolver.h" // StudyRuntime
#include "core/fem/SimDocument.h"
#include "core/io/Autosaver.h"
#include "ui/AppActionsInternal.h" // SlotSim
#include "ui/AppState.h"
#include "ui/DocumentSlots.h"
#include "ui/ProjectActions.h"
#include "ui/VariableActions.h" // GlobalUndo

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <string>
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
