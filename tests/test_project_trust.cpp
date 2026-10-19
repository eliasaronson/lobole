// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "core/Document.h"
#include "core/Feature.h" // BoxFeature, ImportFeature
#include "core/io/GitVault.h"
#include "core/io/ProjectPaths.h"
#include "ui/AppState.h"
#include "ui/DocumentSlots.h"
#include "ui/ProjectActions.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>
#include <unistd.h>

// A shared project is someone else's input
// (docs/versioning/format.md#paths-in-a-project,
// docs/versioning/README.md#a-shared-projects-history). Each case is a crafted
// project that, before its fix, read, wrote, deleted or ran something outside
// the project.

namespace fs = std::filesystem;

namespace {

fs::path Scratch(const char* name) {
  const fs::path p =
      fs::temp_directory_path() /
      ("lobole-trust-" + std::to_string(::getpid()) + "-" + name);
  fs::remove_all(p);
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
  // Two parts: a lone part saves in the flat form, with no project.json.
  ui::AddPart(state, "Second");
  return state;
}

nlohmann::json ReadJson(const fs::path& p) {
  std::ifstream in(p);
  return nlohmann::json::parse(in);
}

int Sh(const std::string& cmd) { return std::system(cmd.c_str()); }

} // namespace

TEST_CASE("A plain component is one name that stays where it is joined",
          "[trust]") {
  using projectpaths::IsPlainComponent;
  CHECK(IsPlainComponent("Bracket"));
  CHECK(IsPlainComponent("Part 1 (copy)"));
  CHECK(IsPlainComponent("ögla"));
  for (const char* bad :
       {"", ".", "..", "a/b", "../x", "/etc/passwd", "a\\b", "C:x", "a\nb"})
    CHECK_FALSE(IsPlainComponent(bad));
  CHECK(projectpaths::DocFolder("Bracket", 7) == "Bracket");
  CHECK(projectpaths::DocFolder("Bracket 1/2", 7) == "document-7");
  CHECK(projectpaths::DocFolder("../../x", 7) == "document-7");
}

TEST_CASE("A path a project names must stay inside it", "[trust]") {
  const fs::path root = Scratch("inside");
  fs::create_directories(root / "parts" / "A");
  CHECK(projectpaths::InsideProject(root, "parts/A").has_value());
  CHECK_FALSE(projectpaths::InsideProject(root, "../x").has_value());
  CHECK_FALSE(projectpaths::InsideProject(root, "parts/../../x").has_value());
  CHECK_FALSE(projectpaths::InsideProject(root, "/etc").has_value());
  CHECK_FALSE(projectpaths::InsideProject(root, "").has_value());
  // A symlink in the project that points out of it.
  fs::create_directory_symlink(fs::temp_directory_path(), root / "parts" / "L");
  CHECK_FALSE(projectpaths::InsideProject(root, "parts/L").has_value());
  fs::remove_all(root);
}

// A STEP file or a script names a part; the name became its folder on save,
// and its autosave file, so `../x` wrote and pruned outside the project.
TEST_CASE("A document whose name is a path is saved in a folder of its own",
          "[trust]") {
  const fs::path root = Scratch("docname");
  const fs::path proj = root / "p.dsgn";
  const fs::path outside = root / "victim";
  fs::create_directories(outside / "assets");
  std::ofstream(outside / "assets" / "keep.txt") << "mine";
  {
    ui::AppState state = StateWithBox();
    state.parts[0].name = "../victim";
    std::string error;
    REQUIRE(ui::SaveProject(state, proj.string(), &error));
  }
  CHECK(fs::exists(outside / "assets" / "keep.txt"));
  CHECK_FALSE(fs::exists(outside / "index.json"));
  const nlohmann::json meta = ReadJson(proj / "project.json");
  const auto& doc = meta.at("documents").at(0);
  CHECK(doc.at("name") == "../victim"); // still its name
  CHECK(doc.at("dir").get<std::string>().starts_with("parts/document-"));
  // And it opens again under that name.
  auto r = ui::ReadProject(proj.string());
  REQUIRE(r.has_value());
  fs::remove_all(root);
}

TEST_CASE("A project whose document lies outside it is refused", "[trust]") {
  const fs::path root = Scratch("docdir");
  const fs::path proj = root / "p.dsgn";
  {
    ui::AppState state = StateWithBox();
    std::string error;
    REQUIRE(ui::SaveProject(state, proj.string(), &error));
  }
  // Another project on the same disk, which the crafted one points at.
  fs::copy(proj / "parts", root / "elsewhere", fs::copy_options::recursive);
  nlohmann::json meta = ReadJson(proj / "project.json");
  const std::string original = meta["documents"][0]["dir"];
  const std::string folder = fs::path(original).filename().string();
  for (const std::string& dir :
       {"../elsewhere/" + folder, (root / "elsewhere" / folder).string()}) {
    INFO(dir);
    meta["documents"][0]["dir"] = dir;
    std::ofstream(proj / "project.json") << meta.dump(2);
    auto r = ui::ReadProject(proj.string());
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().find("project") != std::string::npos);
  }
  fs::remove_all(root);
}

// AdoptStagedImports moves a staged import into the project and deletes the
// original; `<staging>/x/../../file` passed its check.
TEST_CASE("Only a staged import is moved into the project", "[trust]") {
  const fs::path root = Scratch("staging");
  const char* was = std::getenv("XDG_STATE_HOME");
  const std::string saved = was ? was : "";
  setenv("XDG_STATE_HOME", (root / "state").string().c_str(), 1);
  const fs::path victim = root / "victim.step";
  std::ofstream(victim) << "someone's file";
  const fs::path staging = root / "state" / "lobole" / "import-staging";
  fs::create_directories(staging / "x");
  {
    ui::AppState state = StateWithBox();
    const fs::path crafted =
        staging / "x" / ".." / ".." / ".." / ".." / "victim.step";
    state.document->AddFeature(
        std::make_unique<ImportFeature>("Import", crafted.string()));
    std::string error;
    REQUIRE(ui::SaveProject(state, (root / "p.dsgn").string(), &error));
  }
  CHECK(fs::exists(victim));
  if (was)
    setenv("XDG_STATE_HOME", saved.c_str(), 1);
  else
    unsetenv("XDG_STATE_HOME");
  fs::remove_all(root);
}

// A project that arrives with its own .git (a zip, not a clone) brings config
// that runs commands. Git must not run there at all.
TEST_CASE("A repository whose config runs commands is not used",
          "[trust][git]") {
  if (!GitVault::Available())
    SKIP("git not on PATH");
  const fs::path dir = Scratch("fsmonitor");
  const fs::path marker =
      dir.parent_path() / ("lobole-trust-ran-" + std::to_string(::getpid()));
  fs::remove(marker);
  REQUIRE(Sh("git -C '" + dir.string() + "' init -q") == 0);
  std::ofstream(dir / "a.txt") << "x";
  REQUIRE(Sh("git -C '" + dir.string() + "' config core.fsmonitor 'touch " +
             marker.string() + "; false'") == 0);

  const std::string why = GitVault::UntrustedReason(dir.string());
  CHECK(why.find("core.fsmonitor") != std::string::npos);
  CHECK_FALSE(GitVault::IsRepo(dir.string()));
  CHECK_FALSE(GitVault::Init(dir.string())); // never reinitialized and used
  CHECK_FALSE(GitVault::Dirty(dir.string()));
  CHECK(GitVault::Unmerged(dir.string()).empty());
  std::string error;
  CHECK_FALSE(GitVault::CommitAll(dir.string(), "x", &error));
  CHECK_FALSE(fs::exists(marker));

  // Even with the key removed, a filter driver or an include is refused too.
  REQUIRE(Sh("git -C '" + dir.string() + "' config --unset core.fsmonitor") ==
          0);
  CHECK(GitVault::UntrustedReason(dir.string()).empty());
  REQUIRE(Sh("git -C '" + dir.string() + "' config filter.x.clean 'touch " +
             marker.string() + "'") == 0);
  CHECK(GitVault::UntrustedReason(dir.string()).find("filter.x.clean") !=
        std::string::npos);
  CHECK_FALSE(fs::exists(marker));
  fs::remove_all(dir);
}

TEST_CASE("A .git file is not followed to another repository", "[trust][git]") {
  if (!GitVault::Available())
    SKIP("git not on PATH");
  const fs::path root = Scratch("gitfile");
  fs::create_directories(root / "other");
  REQUIRE(GitVault::Init((root / "other").string()));
  fs::create_directories(root / "proj");
  std::ofstream(root / "proj" / ".git")
      << "gitdir: " << (root / "other" / ".git").string() << "\n";
  CHECK_FALSE(GitVault::UntrustedReason((root / "proj").string()).empty());
  CHECK_FALSE(GitVault::IsRepo((root / "proj").string()));
  fs::remove_all(root);
}

// A hook a repository carries never runs, even in one this app trusts.
TEST_CASE("A repository's hooks do not run on save", "[trust][git]") {
  if (!GitVault::Available())
    SKIP("git not on PATH");
  const fs::path dir = Scratch("hooks");
  const fs::path marker =
      dir.parent_path() / ("lobole-trust-hook-" + std::to_string(::getpid()));
  fs::remove(marker);
  REQUIRE(GitVault::Init(dir.string()));
  const fs::path hook = dir / ".git" / "hooks" / "pre-commit";
  std::ofstream(hook) << "#!/bin/sh\ntouch '" << marker.string() << "'\n";
  fs::permissions(hook, fs::perms::owner_all);
  std::ofstream(dir / "a.txt") << "x";
  std::string error;
  CHECK(GitVault::CommitAll(dir.string(), "x", &error));
  CHECK_FALSE(fs::exists(marker));
  fs::remove_all(dir);
}

// A remote's name reaches git as an argument; `--upload-pack=<command>` was
// read as an option, and fetching from the project's own remote ran it.
TEST_CASE("A remote name is never read as an option", "[trust][git]") {
  if (!GitVault::Available())
    SKIP("git not on PATH");
  const fs::path root = Scratch("optname");
  const fs::path marker =
      root.parent_path() / ("lobole-trust-opt-" + std::to_string(::getpid()));
  fs::remove(marker);
  const fs::path upstream = root / "upstream";
  const fs::path proj = root / "proj";
  fs::create_directories(upstream);
  fs::create_directories(proj);
  REQUIRE(GitVault::Init(upstream.string()));
  std::ofstream(upstream / "a.txt") << "x";
  std::string error;
  REQUIRE(GitVault::CommitAll(upstream.string(), "x", &error));
  REQUIRE(GitVault::Init(proj.string()));
  REQUIRE(
      GitVault::AddRemote(proj.string(), "origin", upstream.string(), &error));
  CHECK_FALSE(GitVault::Fetch(
      proj.string(), "--upload-pack=touch '" + marker.string() + "';false",
      &error));
  CHECK_FALSE(fs::exists(marker));
  CHECK_FALSE(GitVault::Clone("--upload-pack=touch '" + marker.string() + "'",
                              (root / "clone").string(), &error));
  CHECK_FALSE(fs::exists(marker));
  fs::remove_all(root);
}

// An `ext::` URL is a command. Git refuses it by default; a person's own
// config can allow it, and a shared repository's remote then ran on fetch.
TEST_CASE("An ext:: remote does not run, whatever the user's config says",
          "[trust][git]") {
  if (!GitVault::Available())
    SKIP("git not on PATH");
  const fs::path root = Scratch("ext");
  const fs::path marker =
      root.parent_path() / ("lobole-trust-ext-" + std::to_string(::getpid()));
  fs::remove(marker);
  const fs::path global = root / "gitconfig";
  std::ofstream(global) << "[protocol \"ext\"]\n\tallow = always\n";
  const char* was = std::getenv("GIT_CONFIG_GLOBAL");
  const std::string saved = was ? was : "";
  setenv("GIT_CONFIG_GLOBAL", global.string().c_str(), 1);
  const fs::path proj = root / "proj";
  fs::create_directories(proj);
  REQUIRE(GitVault::Init(proj.string()));
  std::string error;
  REQUIRE(GitVault::AddRemote(proj.string(), "x",
                              "ext::sh -c touch% " + marker.string(), &error));
  CHECK_FALSE(GitVault::Fetch(proj.string(), "x", &error));
  CHECK_FALSE(fs::exists(marker));
  if (was)
    setenv("GIT_CONFIG_GLOBAL", saved.c_str(), 1);
  else
    unsetenv("GIT_CONFIG_GLOBAL");
  fs::remove(marker);
  fs::remove_all(root);
}
