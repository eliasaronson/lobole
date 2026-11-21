// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

// A project's simulation documents: simulations/<folder>/, kept through an
// open and a save though no tab shows them yet
// (docs/simulation/README.md#how-a-simulation-is-saved).

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "core/Command.h"
#include "core/Document.h"
#include "core/Feature.h"
#include "core/fem/SimDocument.h"
#include "ui/AppState.h"
#include "ui/DocumentSlots.h"
#include "ui/ProjectActions.h"

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

TEST_CASE("A project keeps its simulations through an open and a save",
          "[fem][simdoc][project]") {
  const fs::path root = Scratch("keep");
  const fs::path lob = root / "p.lob";
  ui::AppState state = StateWithBox();
  // Two tabs: a lone part saves in the flat form once its simulation goes.
  ui::AddPart(state, "Second");
  const uint64_t partId = state.parts[0].id;
  std::string why;
  REQUIRE(ui::AddSimulation(state, "Bend check", SimOf(partId), &why) == 0);
  CHECK(state.simulations[0].id > partId);
  // A name another document has is refused, naming it.
  CHECK(ui::AddSimulation(state, state.parts[0].name, SimOf(partId), &why) ==
        SIZE_MAX);
  CHECK_THAT(why, ContainsSubstring("already exists"));
  std::string error;
  REQUIRE(ui::SaveProject(state, lob.string(), &error));
  INFO(error);
  REQUIRE(fs::exists(lob / "simulations" / "Bend check" / "index.json"));
  const nlohmann::json docs = Documents(lob);
  REQUIRE(docs.size() == 3);
  CHECK(docs[2].at("type") == "simulation");
  CHECK(docs[2].at("dir") == "simulations/Bend check");

  auto back = Reopen(lob, true);
  REQUIRE(back->parts.size() == 2); // no tab for it
  REQUIRE(back->simulations.size() == 1);
  CHECK(back->simulations[0].name == "Bend check");
  CHECK(back->simulations[0].id == state.simulations[0].id);
  CHECK(back->simulations[0].sim->Subject().docId == partId);
  CHECK(back->simulations[0].sim->GetStudies().size() == 1);
  CHECK_FALSE(ui::ProjectDirty(*back));
  // A new document's id is past the simulation's.
  CHECK(back->nextPartId > back->simulations[0].id);

  // Saved again by an app that never looked at it: still there.
  REQUIRE(SaveOk(*back, lob));
  CHECK(Reopen(lob)->simulations.size() == 1);

  // Renamed: listed under its new folder. Deleted: no longer listed.
  REQUIRE(ui::RenameSimulation(*back, 0, "Renamed", &why));
  CHECK(ui::ProjectDirty(*back));
  REQUIRE(SaveOk(*back, lob));
  CHECK(Documents(lob)[2].at("dir") == "simulations/Renamed");
  CHECK(Reopen(lob)->simulations[0].name == "Renamed");
  REQUIRE(ui::DeleteSimulation(*back, 0, &why));
  REQUIRE(SaveOk(*back, lob));
  CHECK(Documents(lob).size() == 2);
  CHECK(Reopen(lob)->simulations.empty());
  std::error_code ec;
  fs::remove_all(root, ec);
}

TEST_CASE("A simulation listed before a tab leaves `active` naming the tab",
          "[fem][simdoc][project]") {
  const fs::path root = Scratch("active");
  const fs::path lob = root / "p.lob";
  ui::AppState state = StateWithBox();
  ui::AddPart(state, "Second");
  REQUIRE(ui::AddSimulation(state, "Sim", SimOf(state.parts[0].id)) == 0);
  std::string error;
  REQUIRE(ui::SaveProject(state, lob.string(), &error));
  // Move the simulation to the front by hand, active on "Second".
  nlohmann::json index;
  {
    std::ifstream in(lob / "project.json");
    index = nlohmann::json::parse(in);
  }
  nlohmann::json& docs = index["documents"];
  REQUIRE(docs.size() == 3);
  nlohmann::json reordered = {docs[2], docs[0], docs[1]};
  docs = reordered;
  index["active"] = 2;
  std::ofstream(lob / "project.json") << index.dump(2);
  auto back = Reopen(lob);
  CHECK(back->activePart == 1);
  CHECK(back->parts[1].name == "Second");
  CHECK(back->simulations.size() == 1);
  std::error_code ec;
  fs::remove_all(root, ec);
}
