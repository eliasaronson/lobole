// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

// Every edit that is SAVED makes the project read unsaved — including the
// ones that regenerate nothing and so leave Version() where it was: a
// tolerance, the units, the tube or print process, a study, a configuration
// that is not the active one, a job's operation list. Dirty tracking and
// autosave compare the documents' save stamps (Document::SaveStamp), and a
// project that reads clean is closed without a prompt — so each kind of edit
// is checked here through ui::ProjectDirty, the question the close prompt
// and the title bar ask. docs/versioning/README.md#unsaved-changes.

#include <catch2/catch_test_macros.hpp>

#include "core/Command.h"
#include "core/Document.h"
#include "core/Feature.h"
#include "core/Occurrence.h"
#include "core/cam/JobDocument.h"
#include "core/drawing/DrawingDocument.h"
#include "core/io/Autosaver.h"
#include "ui/AppState.h"
#include "ui/AssemblyActions.h"
#include "ui/DocumentSlots.h"
#include "ui/ProjectActions.h"
#include "ui/StudyHost.h"

#include <filesystem>
#include <functional>
#include <string>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {

fs::path TempDir(const char* name) {
  auto dir =
      fs::temp_directory_path() / ("designer_savestamp_" + std::string(name) +
                                   "_" + std::to_string(::getpid()));
  fs::remove_all(dir);
  fs::create_directories(dir);
  return dir;
}

// One part with one body, saved: clean.
struct Saved {
  ui::AppState state;
  fs::path dir;
  explicit Saved(const char* name) : dir(TempDir(name)) {
    state.document = std::make_unique<Document>();
    state.commands = std::make_unique<CommandStack>();
    state.document->AddFeature(
        std::make_unique<BoxFeature>("B", glm::vec3(0), 10.0, 10.0, 10.0));
    Save();
  }
  ~Saved() { fs::remove_all(dir); }
  // What every save ends in (FileDialogs, save_project, history restore).
  void Save() {
    ui::MarkProjectSaved(state, (dir / "p.json").string(), "save");
    REQUIRE_FALSE(ui::ProjectDirty(state));
  }
  Document& Doc() { return *state.document; }
};

// `edit` must leave Version() where it was and still read dirty, and a save
// must make it read clean again.
void ExpectUnversionedEditDirties(const char* name,
                                  const std::function<void(Saved&)>& edit) {
  Saved s(name);
  const uint64_t version = s.Doc().Version();
  edit(s);
  CHECK(s.Doc().Version() == version); // the premise: nothing regenerated
  CHECK(ui::ProjectDirty(s.state));
  s.Save();
  CHECK_FALSE(ui::ProjectDirty(s.state));
}

Tolerance BoreTolerance() {
  Tolerance t;
  t.ref.featureId = 1;
  t.ref.name = "face0";
  t.isoClass = "h7";
  return t;
}

} // namespace

TEST_CASE("A tolerance edit is an unsaved change", "[savestamp]") {
  ExpectUnversionedEditDirties(
      "tol_set", [](Saved& s) { s.Doc().SetTolerance(BoreTolerance()); });
  ExpectUnversionedEditDirties("tol_cmd", [](Saved& s) {
    s.state.commands->Execute(
        std::make_unique<SetToleranceCommand>(BoreTolerance()), s.Doc());
  });

  // Removed, and restored (redo/undo's two halves).
  Saved s("tol_remove");
  const uint64_t id = s.Doc().SetTolerance(BoreTolerance()).id;
  s.Save();
  REQUIRE(s.Doc().RemoveTolerance(id));
  CHECK(ui::ProjectDirty(s.state));
  s.Save();
  s.Doc().RestoreTolerance(BoreTolerance());
  CHECK(ui::ProjectDirty(s.state));
}

TEST_CASE("Undoing an unversioned edit is an unsaved change too",
          "[savestamp]") {
  // Undo back to exactly what was saved still reads dirty: the stamp only
  // grows. That is the safe direction — a prompt too many, never one short.
  Saved s("undo");
  s.state.commands->Execute(
      std::make_unique<SetToleranceCommand>(BoreTolerance()), s.Doc());
  s.Save();
  s.state.commands->Undo(s.Doc());
  CHECK(s.Doc().GetTolerances().empty());
  CHECK(ui::ProjectDirty(s.state));
}

TEST_CASE("Units, tube and print process are unsaved changes", "[savestamp]") {
  ExpectUnversionedEditDirties("units", [](Saved& s) {
    units::System sys;
    sys.length = units::Length::Inch;
    s.Doc().SetUnits(sys);
  });
  ExpectUnversionedEditDirties("tube", [](Saved& s) {
    tube::TubeRules rules;
    rules.material = "steel";
    s.Doc().SetTube(rules);
  });
  ExpectUnversionedEditDirties("print", [](Saved& s) {
    s.state.commands->Execute(std::make_unique<SetPrintProcessCommand>(
                                  printfit::Process{"fdm", "petg"}),
                              s.Doc());
  });
}

TEST_CASE("Editing a configuration that is not active is an unsaved change",
          "[savestamp]") {
  ExpectUnversionedEditDirties(
      "cfg_add", [](Saved& s) { s.Doc().AddConfiguration("Alt"); });

  Saved s("cfg_edit");
  const uint64_t id = s.Doc().AddConfiguration("Alt").id;
  REQUIRE(s.Doc().ActiveConfiguration() == 0);
  s.Save();
  const uint64_t version = s.Doc().Version();
  Configuration edited = *s.Doc().FindConfiguration(id);
  edited.name = "Renamed";
  REQUIRE(s.Doc().ReplaceConfiguration(edited));
  CHECK(s.Doc().Version() == version); // not active: nothing rebuilt
  CHECK(ui::ProjectDirty(s.state));
  s.Save();
  REQUIRE(s.Doc().RemoveConfiguration(id));
  CHECK(ui::ProjectDirty(s.state));
}

TEST_CASE("Adding, editing and loading a study are unsaved changes",
          "[savestamp]") {
  ExpectUnversionedEditDirties("study_add", [](Saved& s) {
    s.state.commands->Execute(std::make_unique<AddStudyCommand>("S"), s.Doc());
  });

  Saved s("study_edit");
  s.state.commands->Execute(std::make_unique<AddStudyCommand>("S"), s.Doc());
  const uint64_t id = s.Doc().GetStudies().front().id;
  s.Save();
  const uint64_t version = s.Doc().Version();
  // The panel's and the RPC's one door for an edit.
  ui::ModifyStudy(s.state, id, "Add load", [&](fem::Study& st) {
    fem::Load load;
    load.id = s.Doc().AllocateId();
    load.type = fem::LoadType::Force;
    st.loads.push_back(load);
  });
  REQUIRE(s.Doc().FindStudy(id)->loads.size() == 1);
  CHECK(s.Doc().Version() == version);
  CHECK(ui::ProjectDirty(s.state));
  s.Save();
  ui::ModifyStudy(s.state, id, "Rename",
                  [](fem::Study& st) { st.name = "Renamed"; });
  CHECK(ui::ProjectDirty(s.state));
  s.Save();
  s.state.commands->Execute(std::make_unique<RemoveStudyCommand>(id), s.Doc());
  CHECK(ui::ProjectDirty(s.state));
}

TEST_CASE("A study run is not an edit", "[savestamp]") {
  // StudyForRun is how a solve writes the study's transient fields (its
  // error): reading or running a saved project must not make it unsaved.
  Saved s("study_run");
  s.state.commands->Execute(std::make_unique<AddStudyCommand>("S"), s.Doc());
  const uint64_t id = s.Doc().GetStudies().front().id;
  s.Save();
  s.Doc().StudyForRun(id)->error = "no fixed support";
  CHECK_FALSE(ui::ProjectDirty(s.state));
}

TEST_CASE("Assets and the commit-geometry switch are unsaved changes",
          "[savestamp]") {
  ExpectUnversionedEditDirties("asset", [](Saved& s) {
    s.Doc().SetAsset("img", std::vector<unsigned char>{1, 2, 3});
  });
  ExpectUnversionedEditDirties(
      "commit", [](Saved& s) { s.Doc().SetCommitGeometry(false); });
}

TEST_CASE("Autosave writes an edit that regenerates nothing", "[savestamp]") {
  const fs::path dir = TempDir("autosave");
  const std::string project = (dir / "part.json").string();
  Document doc;
  doc.AddFeature(
      std::make_unique<BoxFeature>("B", glm::vec3(0), 1.0, 1.0, 1.0));
  const uint64_t saved = doc.SaveStamp();
  Autosaver saver(1.0);
  saver.Prime(0.0);
  CHECK_FALSE(saver.MaybeSave(doc, project, saved, 10.0)); // clean
  doc.SetTolerance(BoreTolerance());
  CHECK(saver.MaybeSave(doc, project, saved, 20.0));
  // ...and again after a second one, which moves no version either.
  doc.SetPrintProcess(printfit::Process{"fdm", "petg"});
  CHECK(saver.MaybeSave(doc, project, saved, 30.0));
  fs::remove_all(dir);
}

TEST_CASE("Assembly units and an assembly undo are unsaved changes",
          "[savestamp]") {
  Saved s("assembly");
  ui::AddAssembly(s.state, "Asm");
  s.Save();
  units::System sys;
  sys.mass = units::Mass::Pound;
  s.state.assembly->SetUnits(sys);
  CHECK(ui::ProjectDirty(s.state));
  s.Save();

  // Edit, save, undo: the snapshot carries the version it was taken at,
  // which must not come back as the saved one.
  ui::SnapshotAssembly(s.state);
  s.state.assembly->BumpVersion();
  s.Save();
  REQUIRE(ui::UndoAssembly(s.state));
  CHECK(ui::ProjectDirty(s.state));
  s.Save();
  REQUIRE(ui::RedoAssembly(s.state));
  CHECK(ui::ProjectDirty(s.state));
}

TEST_CASE("A drawing undo is an unsaved change", "[savestamp]") {
  Saved s("drawing");
  ui::AddDrawing(s.state, "Sheet");
  s.Save();
  ui::SnapshotDrawing(s.state);
  s.state.drawing->BumpVersion();
  s.Save();
  REQUIRE(ui::UndoDrawing(s.state));
  CHECK(ui::ProjectDirty(s.state));
}

TEST_CASE("Job edits are unsaved changes, and autosave sees them",
          "[savestamp]") {
  Saved s("job");
  ui::AddJob(s.state, "Job");
  s.Save();

  // A GUI edit says so.
  s.state.job->AddSetup("Op 10");
  ui::MarkJobEdited(s.state);
  CHECK(ui::ProjectDirty(s.state));
  s.Save();

  // An RPC edit says nothing: its guard compares content, and whatever
  // records an undo step is an edit.
  {
    ui::JobUndoGuard guard(s.state, false, /*compareContent=*/true);
    s.state.job->AddSetup("Op 20");
  }
  CHECK(ui::ProjectDirty(s.state));
  s.Save();

  // An undo lands on content that was saved once, never on the saved stamp.
  REQUIRE(ui::UndoJob(s.state));
  CHECK(ui::ProjectDirty(s.state));

  // Several documents: the job slot's own stamp is what autosave reads.
  Autosaver saver(1.0);
  saver.Prime(0.0);
  CHECK(ui::AutosaveProject(s.state, saver, 10.0) >= 1);
}
