// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

// Mouse navigation: the gesture parser, the presets and the clash check
// (core/io/MouseNav.h, docs/ui/keybindings.md#mouse-navigation), and the
// settings rows that hold them.

#include "core/io/MouseNav.h"
#include "core/io/Settings.h"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <unistd.h>

namespace fs = std::filesystem;
using nlohmann::json;
using namespace mousenav;

namespace {

struct ScratchConfig {
  fs::path dir;
  std::string previous;
  bool had = false;

  explicit ScratchConfig(const std::string& name) {
    dir = fs::temp_directory_path() /
          ("designer-mousenav-" + name + "-" + std::to_string(::getpid()));
    fs::remove_all(dir);
    fs::create_directories(dir);
    if (const char* old = std::getenv("XDG_CONFIG_HOME"); old != nullptr) {
      previous = old;
      had = true;
    }
    setenv("XDG_CONFIG_HOME", dir.string().c_str(), 1);
    fs::create_directories(settings::ConfigDir());
  }
  ~ScratchConfig() {
    if (had)
      setenv("XDG_CONFIG_HOME", previous.c_str(), 1);
    else
      unsetenv("XDG_CONFIG_HOME");
    fs::remove_all(dir);
  }
  void Write(const std::string& text) const {
    std::ofstream(settings::ConfigPath()) << text;
  }
};

} // namespace

TEST_CASE("every accepted gesture parses and formats back to itself",
          "[mousenav]") {
  std::set<std::string> seen;
  for (const char* text : AcceptedGestures()) {
    INFO(text);
    const std::optional<Gesture> g = ParseGesture(text);
    REQUIRE(g);
    CHECK(FormatGesture(*g) == text);
    CHECK(seen.insert(text).second);
  }
  // Case and modifier order do not matter; the text that comes back is the
  // canonical one.
  CHECK(FormatGesture(*ParseGesture("Ctrl+Middle")) == "ctrl+middle");
  CHECK(FormatGesture(*ParseGesture("alt+shift+left")) == "shift+alt+left");
  CHECK(FormatGesture(*ParseGesture(" ctrl + alt + LEFT ")) == "ctrl+alt+left");
}

TEST_CASE("a gesture outside the set is refused, naming the set",
          "[mousenav]") {
  for (const char* bad :
       {"left", "ctrl+left", "shift+left", "ctrl+shift+alt+left",
        "ctrl+shift+middle", "alt+ctrl+right", "middle+right", "wheel", "",
        "ctrl+ctrl+middle", "ctrl"}) {
    INFO(bad);
    std::string error;
    CHECK_FALSE(ParseGesture(bad, &error));
    CHECK(error.find("not a gesture") != std::string::npos);
    CHECK(error.find("alt+left") != std::string::npos); // the accepted ones
    CHECK(error.find("shift+right") != std::string::npos);
  }
}

TEST_CASE("every preset is clash-free and never binds a bare left button",
          "[mousenav]") {
  REQUIRE(Presets().size() >= 2);
  CHECK(std::string(Presets()[0].name) == "lobole");
  std::set<std::string> names;
  for (const Preset& p : Presets()) {
    INFO(p.name);
    CHECK(names.insert(p.name).second);
    std::string error;
    const std::optional<NavMap> map = NavMap::Build(p.rows, &error);
    REQUIRE(map);
    CHECK(error.empty());
    for (bool ctrl : {false, true})
      for (bool shift : {false, true})
        CHECK(map->ActionFor(Button::Left, ctrl, shift, false) == Action::None);
    // Each preset is its own name back: no two presets are the same rows,
    // or choosing the second would show the first.
    CHECK(std::string(MatchPreset(p.rows)) == p.name);
  }
  // The preset row offers every preset and custom.
  CHECK(PresetNames().size() == Presets().size() + 1);
  CHECK(std::string(PresetNames().back()) == kCustomPreset);
}

TEST_CASE("the defaults are today's navigation", "[mousenav]") {
  const NavMap map = *NavMap::Build(DefaultRows());
  CHECK(map.ActionFor(Button::Middle, false, false, false) == Action::Orbit);
  CHECK(map.ActionFor(Button::Right, false, false, false) == Action::Orbit);
  CHECK(map.ActionFor(Button::Left, false, false, true) == Action::Orbit);
  CHECK(map.ActionFor(Button::Middle, true, false, false) == Action::Pan);
  CHECK(map.ActionFor(Button::Right, true, false, false) == Action::Pan);
  CHECK(map.ActionFor(Button::Left, true, false, true) == Action::Pan);
  CHECK(map.ActionFor(Button::Middle, false, true, false) == Action::Zoom);
  CHECK(map.ActionFor(Button::Left, false, true, true) == Action::Zoom);
  CHECK(map.ActionFor(Button::Middle, false, false, true) == Action::Roll);
  // Several keys held: Ctrl first, as the inline code had it.
  CHECK(map.ActionFor(Button::Middle, true, true, true) == Action::Pan);
  CHECK(map.Navigates(Button::Middle, false));
  CHECK(map.Navigates(Button::Left, true));
  CHECK_FALSE(map.Navigates(Button::Left, false));
}

TEST_CASE("a gesture claimed by two actions is refused by name", "[mousenav]") {
  Rows rows = DefaultRows();
  rows[1].push_back("middle"); // pan too, but middle orbits
  std::string error;
  CHECK_FALSE(NavMap::Build(rows, &error));
  CHECK(error.find("nav_pan") != std::string::npos);
  CHECK(error.find("middle") != std::string::npos);
  CHECK(error.find("nav_orbit") != std::string::npos);
}

TEST_CASE("the navigation rows refuse what they do not accept",
          "[mousenav][settings]") {
  settings::Settings s;
  std::string error;

  CHECK_FALSE(
      settings::ApplyJson(s, {{"nav_orbit", json::array({"left"})}}, &error));
  CHECK(error.find("nav_orbit") != std::string::npos);
  CHECK(error.find("alt+left") != std::string::npos);
  CHECK(s.navOrbit == DefaultRows()[0]); // untouched

  CHECK_FALSE(settings::ApplyJson(s, {{"nav_orbit", "middle"}}, &error));
  CHECK(error.find("list") != std::string::npos);

  CHECK_FALSE(settings::ApplyJson(s, {{"nav_preset", "catia"}}, &error));
  CHECK(error.find("nav_preset") != std::string::npos);
  CHECK(error.find("solidworks") != std::string::npos);
  CHECK(s.navPreset == "lobole");

  // A clash across rows: neither row applies.
  CHECK_FALSE(
      settings::ApplyJson(s, {{"nav_pan", json::array({"middle"})}}, &error));
  CHECK(error.find("middle") != std::string::npos);
  CHECK(s.navPan == DefaultRows()[1]);

  CHECK_FALSE(settings::ApplyJson(s, {{"nav_orbit_speed", 5.0}}, &error));
  CHECK(error.find("nav_orbit_speed") != std::string::npos);
  CHECK(error.find("0.25 to 4") != std::string::npos);
  CHECK_FALSE(settings::ApplyJson(s, {{"nav_zoom_speed", 0.1}}, &error));
  CHECK(error.find("0.25 to 4") != std::string::npos);
  CHECK(s.navZoomSpeed == 1.0f);
}

TEST_CASE("a preset writes the rows and a hand edit makes it custom",
          "[mousenav][settings]") {
  settings::Settings s;
  std::string error;
  REQUIRE(settings::ApplyJson(s, {{"nav_preset", "blender"}}, &error));
  CHECK(s.NavRows() == FindPreset("blender")->rows);
  CHECK(s.navPreset == "blender");

  REQUIRE(settings::ApplyJson(s, {{"nav_roll", json::array({"alt+middle"})}},
                              &error));
  CHECK(s.navPreset == "custom");

  // Back to the defaults by hand: the preset is found again.
  REQUIRE(settings::ApplyJson(s,
                              {{"nav_orbit", DefaultRows()[0]},
                               {"nav_pan", DefaultRows()[1]},
                               {"nav_zoom", DefaultRows()[2]},
                               {"nav_roll", DefaultRows()[3]}},
                              &error));
  CHECK(s.navPreset == "lobole");

  // A preset and a row in one request: the preset, with that row changed,
  // whatever order the keys come in.
  REQUIRE(settings::ApplyJson(
      s, {{"nav_zoom", json::array()}, {"nav_preset", "onshape"}}, &error));
  CHECK(s.navOrbit == FindPreset("onshape")->rows[0]);
  CHECK(s.navZoom.empty());

  // The window's path.
  settings::Settings w;
  w.navPreset = "fusion";
  settings::SyncNavPreset(w, "nav_preset");
  CHECK(w.NavRows() == FindPreset("fusion")->rows);
  w.navRoll = {"alt+middle"};
  settings::SyncNavPreset(w, "nav_roll");
  CHECK(w.navPreset == "custom");
}

TEST_CASE("the navigation rows survive a settings round trip",
          "[mousenav][settings]") {
  ScratchConfig scratch("roundtrip");
  settings::Settings s;
  s.navOrbit = {"right"};
  s.navPan = {"middle", "ctrl+right"};
  s.navZoom = {"shift+alt+left"};
  s.navRoll = {};
  s.navPreset = "custom";
  s.navOrbitSpeed = 2.0f;
  s.navPanSpeed = 0.5f;
  s.navZoomSpeed = 3.0f;
  s.navInvertWheel = true;
  REQUIRE(settings::Save(s));
  const settings::Settings loaded = settings::Load();
  CHECK(loaded.navOrbit == s.navOrbit);
  CHECK(loaded.navPan == s.navPan);
  CHECK(loaded.navZoom == s.navZoom);
  CHECK(loaded.navRoll.empty());
  CHECK(loaded.navPreset == "custom");
  CHECK(loaded.navOrbitSpeed == 2.0f);
  CHECK(loaded.navPanSpeed == 0.5f);
  CHECK(loaded.navZoomSpeed == 3.0f);
  CHECK(loaded.navInvertWheel);
  CHECK(loaded == s);
}

TEST_CASE("a bad navigation row in the file is refused, not substituted",
          "[mousenav][settings]") {
  ScratchConfig scratch("badfile");
  // One unknown gesture loses that row (the rule every row gets) ...
  scratch.Write(R"({"nav_orbit": ["left"], "nav_pan": ["middle"],
                    "nav_orbit_speed": 9, "nav_preset": "maya"})");
  settings::Settings s = settings::Load();
  CHECK(s.navOrbitSpeed == 1.0f);
  // ...and the pan row it would now clash with is refused together with it:
  // the defaults orbit on the middle button.
  CHECK(s.NavRows() == DefaultRows());
  CHECK(s.navPreset == "lobole");

  // A file naming a preset alone gets that preset's rows.
  scratch.Write(R"({"nav_preset": "solidworks"})");
  s = settings::Load();
  CHECK(s.NavRows() == FindPreset("solidworks")->rows);
  CHECK(s.navPreset == "solidworks");

  // A hand edit that disagrees with the preset named beside it: the rows
  // are the truth, and the preset says custom.
  scratch.Write(R"({"nav_preset": "solidworks", "nav_roll": []})");
  s = settings::Load();
  CHECK(s.navOrbit == FindPreset("solidworks")->rows[0]);
  CHECK(s.navRoll.empty());
  CHECK(s.navPreset == "custom");
}
