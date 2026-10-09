// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "remote/InstanceRegistry.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {

// Point the registry at a scratch state dir: a test must never write into the
// developer's real ~/.local/state/lobole, and two tests must not see each
// other's instances.
struct ScratchState {
  fs::path dir;
  std::string previous;
  bool had = false;

  explicit ScratchState(const char* name) {
    dir = fs::temp_directory_path() / name;
    fs::remove_all(dir);
    fs::create_directories(dir);
    if (const char* old = std::getenv("XDG_STATE_HOME")) {
      previous = old;
      had = true;
    }
    setenv("XDG_STATE_HOME", dir.string().c_str(), 1);
  }
  ~ScratchState() {
    if (had)
      setenv("XDG_STATE_HOME", previous.c_str(), 1);
    else
      unsetenv("XDG_STATE_HOME");
    fs::remove_all(dir);
  }
};

} // namespace

// A client should be able to FIND the app rather than be told which port to
// assume — the MCP server defaulted to 8437 while the repo's .mcp.json
// hardcoded 8799, and any other port read as "Designer is not reachable".
TEST_CASE("An instance announces itself and withdraws", "[remote][registry]") {
  ScratchState scratch("designer-registry-basic");

  REQUIRE(remote::AnnounceInstance(8901, "gui"));
  auto live = remote::LiveInstances();
  REQUIRE(live.size() == 1);
  CHECK(live[0].port == 8901);
  CHECK(live[0].pid == static_cast<int>(::getpid()));
  // gui / headless / offscreen is the capability split a caller acts on:
  // whether renders exist, and whether there is a GUI to drive.
  CHECK(live[0].mode == "gui");

  remote::WithdrawInstance(8901);
  CHECK(remote::LiveInstances().empty());
  remote::WithdrawInstance(8901); // twice is not an error
}

TEST_CASE("Two instances are both discoverable", "[remote][registry]") {
  ScratchState scratch("designer-registry-two");
  REQUIRE(remote::AnnounceInstance(8902, "gui"));
  REQUIRE(remote::AnnounceInstance(8903, "offscreen"));
  auto live = remote::LiveInstances();
  REQUIRE(live.size() == 2);
  // Ambiguity has to reach the client — attaching to the wrong window would
  // answer questions about a document the user is not looking at.
  CHECK(((live[0].port == 8902 && live[1].port == 8903) ||
         (live[0].port == 8903 && live[1].port == 8902)));
  remote::WithdrawInstance(8902);
  remote::WithdrawInstance(8903);
}

// A crash never gets to run cleanup, so the file it leaves behind must not
// send a client at a port nobody is listening on.
TEST_CASE("A dead instance's file is pruned, not believed",
          "[remote][registry]") {
  ScratchState scratch("designer-registry-stale");
  fs::create_directories(remote::InstancesDir());
  const fs::path stale = fs::path(remote::InstancesDir()) / "8904.json";
  {
    // pid 0x7FFFFFFF: valid to write, certain not to be running.
    nlohmann::json j{{"app", "lobole"},
                     {"pid", 2147483647},
                     {"port", 8904},
                     {"mode", "gui"},
                     {"started", 1}};
    std::ofstream(stale) << j.dump();
  }
  const fs::path garbage = fs::path(remote::InstancesDir()) / "8905.json";
  std::ofstream(garbage) << "{not json";

  CHECK(remote::LiveInstances().empty());
  CHECK_FALSE(fs::exists(stale));   // and cleaned up as it was found
  CHECK_FALSE(fs::exists(garbage)); // unparseable announces nothing
}
