// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "common/DevicePreference.h"
#include "core/io/Settings.h"
#include "ui/SettingsWindow.h" // the Preferences page registry

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;

namespace {

// Point XDG_CONFIG_HOME at a scratch directory: no test run may write the
// developer's real preferences. Per-case AND per-pid, because ctest runs
// these in parallel.
struct ScratchConfig {
  fs::path dir;
  std::string previous;
  bool had = false;

  explicit ScratchConfig(const char* name) {
    dir = fs::temp_directory_path() / ("designer-cfg-" + std::string(name) +
                                       "-" + std::to_string(::getpid()));
    fs::remove_all(dir);
    fs::create_directories(dir);
    if (const char* old = std::getenv("XDG_CONFIG_HOME")) {
      previous = old;
      had = true;
    }
    setenv("XDG_CONFIG_HOME", dir.string().c_str(), 1);
  }
  ~ScratchConfig() {
    if (had)
      setenv("XDG_CONFIG_HOME", previous.c_str(), 1);
    else
      unsetenv("XDG_CONFIG_HOME");
    fs::remove_all(dir);
  }

  void WriteRaw(const std::string& text) const {
    fs::create_directories(settings::ConfigDir());
    std::ofstream(settings::ConfigPath()) << text;
  }
  std::string ReadRaw() const {
    std::ifstream in(settings::ConfigPath());
    return std::string((std::istreambuf_iterator<char>(in)), {});
  }
};

} // namespace

TEST_CASE("Settings: absent file is defaults, and a round trip is stable",
          "[settings]") {
  ScratchConfig scratch("roundtrip");

  const settings::Settings defaults = settings::Load();
  CHECK(defaults.uiFontScale == 1.0f);
  CHECK(defaults.shadows);
  CHECK(defaults.ssao == "half");
  CHECK(defaults.shadowMap == 2048);
  CHECK(defaults.gpuDevice.empty());
  CHECK_FALSE(fs::exists(settings::ConfigPath())); // reading writes nothing

  settings::Settings s = defaults;
  s.uiFontScale = 1.4f;
  s.shadows = false;
  s.ssao = "full";
  s.shadowMap = 1024;
  s.gpuDevice = "NVIDIA GeForce RTX 4060 Laptop GPU";
  s.autosaveInterval = 30.0;
  std::string error;
  REQUIRE(settings::Save(s, &error));
  CHECK(error.empty());

  const settings::Settings loaded = settings::Load();
  CHECK(loaded == s);
}

TEST_CASE("Settings: an older build cannot delete a newer build's keys",
          "[settings]") {
  ScratchConfig scratch("unknown-keys");
  scratch.WriteRaw(R"({
  "settings_format": 7,
  "shadows": false,
  "a_setting_from_the_future": {"nested": [1, 2]}
})");

  // A newer settings_format is READ, not refused — refusing would lock a user
  // out of preferences this build understands perfectly well.
  settings::Settings s = settings::Load();
  CHECK_FALSE(s.shadows);
  REQUIRE(s.unknown.size() == 1);
  CHECK(s.unknown[0].first == "a_setting_from_the_future");

  s.shadows = true;
  REQUIRE(settings::Save(s));
  const nlohmann::json out = nlohmann::json::parse(scratch.ReadRaw());
  CHECK(out.at("shadows").get<bool>());
  REQUIRE(out.contains("a_setting_from_the_future"));
  CHECK(out.at("a_setting_from_the_future").at("nested").size() == 2);
}

TEST_CASE("Settings: a corrupt file is kept, not overwritten", "[settings]") {
  ScratchConfig scratch("corrupt");
  scratch.WriteRaw("{ this is not json");

  const settings::Settings s = settings::Load();
  CHECK(s.uiFontScale == 1.0f); // defaults, no complaint to the user

  // The one copy of a hand-edited config is not ours to destroy.
  const fs::path bak = settings::ConfigPath() + ".bak";
  REQUIRE(fs::exists(bak));
  std::ifstream in(bak);
  std::string kept((std::istreambuf_iterator<char>(in)), {});
  CHECK(kept == "{ this is not json");
}

// Two instances run at once here (a windowed dev-watch plus headless agents).
// A blind write would let whichever saved last revert the other.
TEST_CASE("Settings: a save merges instead of clobbering", "[settings]") {
  ScratchConfig scratch("merge");

  settings::Settings first = settings::Load();
  const settings::Settings baseline = first;

  // Another instance changes the font size while we hold `baseline`.
  settings::Settings other = baseline;
  other.uiFontScale = 1.4f;
  REQUIRE(settings::Save(other, baseline));

  // We change only shadows, and save against OUR baseline.
  first.shadows = false;
  REQUIRE(settings::Save(first, baseline));

  const settings::Settings merged = settings::Load();
  CHECK_FALSE(merged.shadows);       // our change
  CHECK(merged.uiFontScale == 1.4f); // and theirs survived
}

TEST_CASE("Settings: the window rect survives, and does not clobber",
          "[settings]") {
  ScratchConfig scratch("window");

  settings::Settings s = settings::Load();
  CHECK_FALSE(s.window.Remembered()); // a first run remembers nothing

  const settings::Settings baseline = s;
  s.window = {120, 60, 1600, 900, false};
  REQUIRE(settings::Save(s, baseline));

  const settings::Settings back = settings::Load();
  CHECK(back.window == s.window);

  // The same merge rule the rest of the file gets: two instances run here,
  // and a blind write would teleport the other one on its next launch.
  settings::Settings other = back;
  other.uiFontScale = 1.4f;
  REQUIRE(settings::Save(other, back)); // moves nothing
  CHECK(settings::Load().window == s.window);

  settings::Settings moved = back;
  moved.window.x = 400;
  REQUIRE(settings::Save(moved, back));
  const settings::Settings merged = settings::Load();
  CHECK(merged.window.x == 400);
  CHECK(merged.uiFontScale == 1.4f); // and the other instance's change stayed
}

TEST_CASE("Settings: the window rect is not a PREFERENCE", "[settings]") {
  // It is remembered state. If it ever appeared in the kFields table it
  // would get a row in the Preferences window, and nobody wants to type
  // their window size into a form.
  for (const settings::Field& f : settings::Fields())
    CHECK(std::string(f.key) != "window");
}

TEST_CASE("Settings: invalid values are refused by name", "[settings]") {
  settings::Settings s;
  std::string error;

  CHECK_FALSE(settings::ApplyJson(s, {{"ssao", "sometimes"}}, &error));
  CHECK(error.find("ssao") != std::string::npos);
  CHECK(s.ssao == "half"); // untouched

  CHECK_FALSE(settings::ApplyJson(s, {{"shadow_map", 4096}}, &error));
  CHECK(error.find("shadow_map") != std::string::npos);
  CHECK(s.shadowMap == 2048);

  CHECK_FALSE(settings::ApplyJson(s, {{"autosave_interval", -5}}, &error));
  CHECK_FALSE(settings::ApplyJson(s, {{"no_such_setting", 1}}, &error));
  CHECK(error.find("no_such_setting") != std::string::npos);

  // A rejected value in a MULTI-key request applies none of it.
  CHECK_FALSE(
      settings::ApplyJson(s, {{"shadows", false}, {"ssao", "nope"}}, &error));
  CHECK(s.shadows);

  CHECK(settings::ApplyJson(s, {{"shadows", false}, {"ssao", "full"}}, &error));
  CHECK_FALSE(s.shadows);
  CHECK(s.ssao == "full");
}

TEST_CASE("Settings: every field is reachable from the table", "[settings]") {
  // The table drives the JSON round trip and the window rows, so a field
  // missing from it is a field nobody can set.
  const nlohmann::json j = settings::ToJson(settings::Settings{});
  for (const settings::Field& f : settings::Fields()) {
    INFO(f.key);
    CHECK(j.contains(f.key));
    CHECK(std::string(f.label).size() > 0);
    CHECK(std::string(f.blurb).size() > 0); // each row explains itself
    CHECK(std::string(f.section).size() > 0);
  }
  CHECK(j.contains("settings_format"));
}

// GPU choice is a pure rule so the renderer (one device, at startup) and the
// FEM compute context (a new one per run) cannot drift apart.
TEST_CASE("Device preference: a name wins, but never over eligibility",
          "[settings]") {
  const std::vector<std::string> names = {"Intel(R) Arc(tm) Graphics (MTL)",
                                          "NVIDIA GeForce RTX 4060 Laptop GPU",
                                          "llvmpipe (LLVM 21.1.8, 256 bits)"};
  const std::vector<int> scores = {1, 2, 0}; // integrated, discrete, software

  // No preference: the best score, which is the discrete card.
  CHECK(devicepref::Choose(names, scores, "") == 1);
  // A substring is enough — nobody should have to type the marketing name.
  CHECK(devicepref::Choose(names, scores, "nvidia") == 1);
  CHECK(devicepref::Choose(names, scores, "Arc") == 0);
  CHECK(devicepref::Choose(names, scores, "llvmpipe") == 2);
  // A preference for a device that is gone falls back rather than refusing to
  // start: an eGPU can be unplugged between sessions.
  CHECK(devicepref::Choose(names, scores, "Radeon") == 1);

  // A negative score means the device cannot do the job (the compute path
  // uses this for "no float32 atomics"). A setting must not be able to pick
  // one — silently doing nothing would be the worst outcome.
  const std::vector<int> noAtomics = {-1, 2, -1};
  CHECK(devicepref::Choose(names, noAtomics, "llvmpipe") == 1);
  CHECK(devicepref::Choose(names, noAtomics, "Arc") == 1);

  // Nothing eligible at all.
  const std::vector<int> none = {-1, -1, -1};
  CHECK(devicepref::Choose(names, none, "") == -1);
  CHECK(devicepref::Choose({}, {}, "nvidia") == -1);
}

// The Preferences window once drew one collapsing header per section, and
// this table's ORDER grouped the rows under it, so a section split across two
// runs drew that header TWICE — two widgets sharing one "###sec_<name>" id,
// an ImGui id collision — and split the section on screen for no reason.
// Sections are pages now (ui::PreferenceCategories).
//
// That shipped: the two Sharing rows landed between the Performance rows in
// c7e50ff, and Preferences showed Performance twice until 2026-08-24. The
// window no longer depends on the order, but the table should still READ the
// way it draws — and a person adding a row edits the table, which is where
// this wants to fail.
TEST_CASE("the settings table groups its sections", "[settings]") {
  std::vector<std::string> runs;
  for (const settings::Field& f : settings::Fields())
    if (runs.empty() || runs.back() != f.section)
      runs.emplace_back(f.section);

  std::set<std::string> seen;
  for (const std::string& section : runs) {
    INFO("section '" << section
                     << "' appears in more than one run of settings::Fields() "
                        "— move its rows together");
    CHECK(seen.insert(section).second);
  }
}

// Preferences shows one page per category (docs/ui/settings.md#the-window):
// every table row on exactly one page, no page empty, and no hand-written
// page sharing a name with a section, which would put two rows in the list
// under one "###cat_<name>" id.
TEST_CASE("every preference is on exactly one page", "[settings]") {
  const auto categories = ui::PreferenceCategories();
  std::set<std::string> names;
  for (const ui::PreferenceCategory& c : categories) {
    INFO("page '" << c.name << "'");
    CHECK(names.insert(c.name).second);
    CHECK(c.icon != nullptr);
  }
  for (const settings::Field& f : settings::Fields()) {
    int pages = 0;
    for (const ui::PreferenceCategory& c : categories)
      for (const settings::Field* row : c.rows)
        if (row == &f) {
          pages++;
          CHECK(c.name == f.section);
        }
    INFO("row '" << f.key << "'");
    CHECK(pages == 1);
  }
  // A table page has rows; a page without rows is one of the hand-written
  // ones, which draw themselves.
  for (const char* hand : {"Keyboard", "Machines"})
    CHECK(names.count(hand) == 1);
  for (const ui::PreferenceCategory& c : categories)
    if (c.rows.empty()) {
      INFO("page '" << c.name << "' has no rows and is not hand-written");
      CHECK((c.name == "Keyboard" || c.name == "Machines"));
    }
  // Machines is a library: search finds its items by name.
  for (const ui::PreferenceCategory& c : categories)
    if (c.name == "Machines")
      CHECK(c.library);
}

// A row with a section nobody has used yet is a new page, in table order and
// before the hand-written ones, without a change to the window.
TEST_CASE("a new section is a new preferences page", "[settings]") {
  const settings::Field fixture[] = {
      {"a", "Appearance", "A", "", settings::Tier::Live,
       &settings::Settings::shadows},
      {"b", "Plotting", "B", "", settings::Tier::Live,
       &settings::Settings::idleThrottle},
      {"c", "Appearance", "C", "", settings::Tier::Live,
       &settings::Settings::terminalPanel},
  };
  const auto categories = ui::PreferenceCategories(fixture);
  REQUIRE(categories.size() >= 2);
  CHECK(categories[0].name == "Appearance");
  CHECK(categories[0].rows.size() == 2); // split rows, still one page
  CHECK(categories[1].name == "Plotting");
  REQUIRE(categories[1].rows.size() == 1);
  CHECK(std::string(categories[1].rows[0]->key) == "b");
  CHECK(categories[1].icon != nullptr); // a generic glyph, not none
  CHECK(categories[2].name == "Keyboard");
}

// Reset <category> puts that page's rows back and nothing else.
TEST_CASE("a section resets alone", "[settings]") {
  settings::Settings s;
  s.shadows = false;         // Performance
  s.ssao = "full";           // Performance
  s.uiFontScale = 1.4f;      // Appearance
  s.gitUserName = "someone"; // Sharing
  REQUIRE(s.keys.Set("view.fit", "F9"));
  s.unknown.emplace_back("future_key", "1");
  settings::ResetSection(s, "Performance");
  CHECK(s.shadows);
  CHECK(s.ssao == "half");
  CHECK(s.uiFontScale == 1.4f);
  CHECK(s.gitUserName == "someone");
  CHECK(!s.keys.IsDefault("view.fit")); // the keymap is not a section
  CHECK(s.unknown.size() == 1);
}
