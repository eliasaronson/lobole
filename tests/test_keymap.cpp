// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

// Keymap: the chord parser, the defaults table and the conflict gate
// (docs/ui/keybindings.md). These run headless because the metadata lives
// in core — designer_tests links designer_core alone, so a broken default
// binding fails here in 8 seconds rather than in the GUI suite.

#include "core/io/Keymap.h"
#include "core/io/Settings.h"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>

namespace fs = std::filesystem;
using nlohmann::json;

namespace {

// Point XDG_CONFIG_HOME at a scratch dir so no test run can touch the
// developer's real settings.json (docs/ui/settings.md).
struct ScratchConfig {
  fs::path dir;
  std::string previous;
  bool had = false;

  explicit ScratchConfig(const std::string& name) {
    dir = fs::temp_directory_path() / ("designer-keymap-" + name);
    fs::remove_all(dir);
    fs::create_directories(dir);
    if (const char* old = std::getenv("XDG_CONFIG_HOME"); old != nullptr) {
      previous = old;
      had = true;
    }
    setenv("XDG_CONFIG_HOME", dir.string().c_str(), 1);
    // ConfigDir() is `$XDG_CONFIG_HOME/designer`, and the cases that hand-write
    // a settings.json do not go through Save() (which would create it).
    fs::create_directories(settings::ConfigDir());
  }
  ~ScratchConfig() {
    if (had)
      setenv("XDG_CONFIG_HOME", previous.c_str(), 1);
    else
      unsetenv("XDG_CONFIG_HOME");
    fs::remove_all(dir);
  }
};

} // namespace

TEST_CASE("chord text round trips", "[keymap]") {
  using namespace keymap;

  CHECK(FormatChord(ParseChord("F")) == "F");
  CHECK(FormatChord(ParseChord("Ctrl+Z")) == "Ctrl+Z");
  CHECK(FormatChord(ParseChord("Ctrl+Shift+S")) == "Ctrl+Shift+S");
  CHECK(FormatChord(ParseChord("Shift+1")) == "Shift+1");
  CHECK(FormatChord(ParseChord("Ctrl+,")) == "Ctrl+,");
  CHECK(FormatChord(ParseChord("Space")) == "Space");

  // Case and modifier order are the user's business, not the file's.
  CHECK(FormatChord(ParseChord("shift+ctrl+s")) == "Ctrl+Shift+S");
  CHECK(FormatChord(ParseChord("  ctrl + z  ")) == "Ctrl+Z");

  // Empty is "unbound", which is a state and not an error.
  std::string error = "sentinel";
  CHECK(!ParseChord("", &error));
  CHECK(error == "sentinel");

  // Anything else names what it could not read.
  error.clear();
  CHECK(!ParseChord("Ctrl+Nope", &error));
  CHECK(error.find("Nope") != std::string::npos);
  error.clear();
  CHECK(!ParseChord("Hyper+A", &error));
  CHECK(error.find("Hyper") != std::string::npos);
}

TEST_CASE("every default chord survives a round trip", "[keymap]") {
  for (const keymap::Binding& b : keymap::Defaults()) {
    std::string error;
    const keymap::Chord c = keymap::ParseChord(b.chord, &error);
    INFO(b.id << " -> '" << b.chord << "'");
    CHECK(error.empty());
    // An unbound default is allowed; a MISSPELLED one is not, and without
    // this check it would look identical.
    if (*b.chord != '\0') {
      CHECK(static_cast<bool>(c));
      CHECK(keymap::FormatChord(c) == b.chord);
    }
    if (*b.altChord != '\0') {
      std::string altError;
      CHECK(static_cast<bool>(keymap::ParseChord(b.altChord, &altError)));
      CHECK(altError.empty());
    }
  }
}

TEST_CASE("the defaults table is well formed", "[keymap]") {
  std::set<std::string> ids;
  for (const keymap::Binding& b : keymap::Defaults()) {
    INFO(b.id);
    CHECK(*b.id != '\0');
    CHECK(*b.label != '\0');
    CHECK(*b.section != '\0');
    CHECK(ids.insert(b.id).second); // no duplicate command ids
    CHECK(keymap::Find(b.id) == &b);
  }
  CHECK(keymap::Find("nothing.at.all") == nullptr);
}

// The standing gate: two commands in one context must not want the same
// chord. A context binding shadowing a GLOBAL one is the design (N is
// Coincident while sketching, align-to-face everywhere else), so it is not
// counted here.
TEST_CASE("the shipped defaults have no conflicts", "[keymap]") {
  const keymap::Keymap defaults;
  const std::vector<keymap::Conflict> conflicts = defaults.Conflicts();
  for (const keymap::Conflict& c : conflicts)
    UNSCOPED_INFO("conflict in " << keymap::ContextName(c.ctx) << ": " << c.a
                                 << " and " << c.b << " both on "
                                 << keymap::FormatChord(c.chord));
  CHECK(conflicts.empty());
}

// The rule from docs/ui/keybindings.md: EVERY toolbar button ships with a
// chord. The only rows allowed to be unbound are the ones that live in a
// flyout or a menu rather than on a toolbar — listed here by name, so adding
// a toolbar button without a chord fails instead of passing quietly.
TEST_CASE("every command ships bound, or is on the list", "[keymap]") {
  static const std::set<std::string> kUnboundByDesign = {
      // Menu-only.
      "file.export",
      // Direct edit (docs/modeling/README.md#direct-edit): four tools in one
      // toolbar flyout, reached far less often than the features that make
      // a part; a chord each would spend four mnemonics.
      "part.delete_face",
      "part.offset_face",
      "part.move_face",
      "part.replace_face",
      "view.reset_layout",
      "view.controls_help",
      // The sketch "more constraints" flyout: a constraint used once a month
      // earns a Preferences row, not an invented mnemonic.
      "sketch.constraint_curvature",
      "sketch.constraint_point_on_line",
      "sketch.constraint_symmetric",
      "sketch.constraint_midpoint",
      "sketch.constraint_fixed",
      "sketch.constraint_collinear",
      // Derive: a copy of another part is placed once per part from the
      // toolbar or the RPC, like an import — not something typed mid-flow.
      "part.derive",
      // Insert template: chosen from the project's library and placed on a
      // face, like Derive -- not a verb typed mid-flow.
      "part.template",
      // A bearing: two or four per assembly, placed from the toolbar beside
      // the fastener, which has F. Not a verb repeated mid-flow.
      "assembly.bearing",
      // A worm pair: two parts made once each, from the toolbar beside the
      // thread they are made like. Not a verb repeated mid-flow.
      "part.worm",
      "part.worm_wheel",
      // Sheet metal: three tools used in sequence on a part that IS sheet
      // metal, in the order they appear on the toolbar — not typed mid-flow
      // the way a fillet or a hole is. Every free letter left is worth more
      // to a tool used on every part.
      "part.sheet_base_flange",
      "part.sheet_edge_flange",
      "part.sheet_flat_pattern",
      "part.sheet_unfold",
      "part.sheet_refold",
      "part.sheet_hem",
      "part.sheet_jog",
      "part.sheet_corner_relief",
      "part.sheet_normal_cut",
  };
  for (const keymap::Binding& b : keymap::Defaults()) {
    INFO(b.id);
    const bool bound = *b.chord != '\0';
    CHECK(bound == (kUnboundByDesign.count(b.id) == 0));
  }
}

TEST_CASE("overrides resolve, reset and report conflicts", "[keymap]") {
  keymap::Keymap k;
  REQUIRE(k.Text("sketch.line") == "L");
  REQUIRE(k.IsDefault("sketch.line"));

  std::string error;
  REQUIRE(k.Set("sketch.line", "shift+j", &error));
  CHECK(error.empty());
  CHECK(k.Text("sketch.line") == "Shift+J"); // stored normalised, not as typed
  CHECK(!k.IsDefault("sketch.line"));

  // A rebind that collides is ALLOWED and reported — refusing it would strand
  // anyone doing a two-step swap.
  REQUIRE(k.Set("sketch.circle", "Shift+J"));
  const std::vector<keymap::Conflict> conflicts = k.Conflicts();
  REQUIRE(conflicts.size() == 1);
  CHECK(conflicts[0].a == "sketch.line");
  CHECK(conflicts[0].b == "sketch.circle");
  CHECK(keymap::FormatChord(conflicts[0].chord) == "Shift+J");

  CHECK(k.Bound(keymap::Context::Sketch, keymap::ParseChord("Shift+J"),
                "sketch.line") == std::vector<std::string>{"sketch.circle"});

  // Unbinding is a real state: the command keeps its row and loses its chord.
  REQUIRE(k.Set("sketch.circle", ""));
  CHECK(k.Text("sketch.circle").empty());
  CHECK(!k.IsDefault("sketch.circle"));

  k.Reset("sketch.circle");
  CHECK(k.Text("sketch.circle") == "C");
  k.ResetAll();
  CHECK(k.Text("sketch.line") == "L");
  CHECK(k.Conflicts().empty());

  // Garbage is refused and changes nothing.
  REQUIRE(!k.Set("sketch.line", "Ctrl+Nope", &error));
  CHECK(!error.empty());
  CHECK(k.Text("sketch.line") == "L");
}

TEST_CASE("the alternate chord belongs to the default only", "[keymap]") {
  keymap::Keymap k;
  CHECK(keymap::FormatChord(k.Alt("edit.redo")) == "Ctrl+Shift+Z");
  REQUIRE(k.Set("edit.redo", "Ctrl+Shift+Y"));
  // Once redo is somebody's own chord, the inherited alternate is just a
  // second thing to collide with.
  CHECK(!k.Alt("edit.redo"));
}

TEST_CASE("a keymap survives a settings round trip", "[keymap][settings]") {
  ScratchConfig scratch("roundtrip");

  settings::Settings s;
  REQUIRE(s.keys.Set("part.fillet", "Shift+B"));
  REQUIRE(s.keys.Set("sketch.line", "")); // an explicit unbind
  std::string error;
  REQUIRE(settings::Save(s, &error));

  const settings::Settings loaded = settings::Load();
  CHECK(loaded.keys.Text("part.fillet") == "Shift+B");
  CHECK(loaded.keys.Text("sketch.line").empty());
  CHECK(!loaded.keys.IsDefault("sketch.line"));
  // Untouched rows are NOT written, so a later change to a default still
  // reaches anyone who never rebound it.
  CHECK(loaded.keys.IsDefault("sketch.circle"));
  CHECK(loaded.keys.Overrides().size() == 2);
  CHECK(loaded == s);
}

TEST_CASE("a binding this build does not know is kept", "[keymap][settings]") {
  ScratchConfig scratch("unknown-id");

  {
    json j;
    j["settings_format"] = 1;
    j["keys"] =
        json{{"part.fillet", "Shift+B"}, {"future.command", "Ctrl+Shift+P"}};
    std::ofstream(settings::ConfigPath()) << j.dump(2);
  }

  settings::Settings s = settings::Load();
  CHECK(s.keys.Text("future.command") == "Ctrl+Shift+P");
  // Saving from this build must not drop it — an older build running once
  // cannot be allowed to delete a newer build's keymap.
  s.keys.Set("part.chamfer", "Shift+C");
  REQUIRE(settings::Save(s, settings::Settings{}));

  const json onDisk = json::parse(std::ifstream(settings::ConfigPath()));
  CHECK(onDisk["keys"]["future.command"] == "Ctrl+Shift+P");
  CHECK(onDisk["keys"]["part.chamfer"] == "Shift+C");
}

TEST_CASE("a bad chord in the file loses one binding, not the keymap",
          "[keymap][settings]") {
  ScratchConfig scratch("bad-chord");
  {
    json j;
    j["settings_format"] = 1;
    j["ui_font_scale"] = 1.2;
    j["keys"] = json{{"part.fillet", "Ctrl+Nope"}};
    std::ofstream(settings::ConfigPath()) << j.dump(2);
  }

  const settings::Settings s = settings::Load();
  CHECK(s.uiFontScale == 1.2f); // the rest of the file still applies
  CHECK(s.keys.Text("part.fillet") == "B"); // and the default stands
}
