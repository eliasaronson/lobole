// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <filesystem>

#include "core/io/Keymap.h"
#include "core/io/MouseNav.h"
#include "core/io/WindowGeometry.h"

#include <nlohmann/json_fwd.hpp>

#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

// User preferences: things that belong to the PERSON, survive a restart, and
// apply to every project (docs/ui/settings.md). Three buckets, and this is
// the middle one — a project setting travels with the model in git, session
// state belongs to the moment and is never written here. The test: would the
// user be annoyed to set it again tomorrow, and expect the same answer in
// every project?
//
// Lives in core rather than ui/ so it can be unit-tested (designer_tests
// links designer_core alone).
namespace settings {

// How a change reaches the user, which the Preferences window states per row
// so a pending change is visible instead of a lie.
enum class Tier {
  Live,     // the next frame reads it
  NextTime, // takes effect on the next save / new part / next run of a thing
};

struct Settings {
  // Appearance
  float uiFontScale = 1.0f; // 0.85 | 1.0 | 1.2 | 1.4, as View > Font Size had
  // The viewport's light rig follows the camera, so whatever side of a part
  // you look at is lit (core/LightRig.h). Locked, it stays fixed in the world
  // where it stands at the home view, and the shadows stay put as you orbit.
  bool lockLightsToModel = false;

  // Navigation
  // The wheel (and Shift-drag) zooms toward what the cursor is over, as other
  // CAD programs do; off, toward the centre of the view (docs/ui/README.md).
  bool zoomToCursor = true;
  // Which drag orbits, pans, zooms and rolls: each row a list of gestures
  // (core/io/MouseNav.h), checked together so no gesture does two things.
  // `navPreset` names the preset the rows are, or "custom"; choosing a preset
  // writes the rows, and editing a row re-derives it (SyncNavPreset).
  std::string navPreset = "lobole";
  std::vector<std::string> navOrbit = mousenav::DefaultRows()[0];
  std::vector<std::string> navPan = mousenav::DefaultRows()[1];
  std::vector<std::string> navZoom = mousenav::DefaultRows()[2];
  std::vector<std::string> navRoll = mousenav::DefaultRows()[3];
  // Multipliers on the camera's own rates (render/Camera.h), 0.25-4.
  float navOrbitSpeed = 1.0f;
  float navPanSpeed = 1.0f;
  float navZoomSpeed = 1.0f;
  // Scrolling toward you zooms in; on, it zooms out (viewport and sheet).
  bool navInvertWheel = false;

  mousenav::Rows NavRows() const {
    return {navOrbit, navPan, navZoom, navRoll};
  }
  void SetNavRows(const mousenav::Rows& rows) {
    navOrbit = rows[0];
    navPan = rows[1];
    navZoom = rows[2];
    navRoll = rows[3];
  }

  // Performance
  std::string gpuDevice; // "" = automatic (prefer discrete)
  bool shadows = true;
  std::string ssao = "half"; // off | half | full
  int shadowMap = 2048;      // 1024 | 2048
  // Idle throttle (docs/rendering/frame-pacing.md#idle-throttle): with nothing
  // happening — no input, animation, solve, or remote traffic — the frame loop
  // waits for events at a ~10 Hz floor instead of re-rendering the unchanged
  // scene at vsync rate. Quality-neutral; battery, fans, and iGPU turbo
  // headroom.
  bool idleThrottle = true;

  // Simulation: the CalculiX solver to run (docs/simulation/README.md
  // #calculix). Empty finds one: LOBOLE_CCX, a copy beside the executable,
  // then PATH.
  std::string calculixPath;

  // Sharing — who a commit is BY. Empty means "whatever git is configured
  // with", which is the right answer for anyone who has configured it; these
  // exist for people who have not (F9 C1).
  std::string gitUserName;
  std::string gitUserEmail;

  // General
  double autosaveInterval = 60.0; // seconds; 0 disables
  // Off by default: a CAD app should not open with a shell in it, and the
  // panel costs a process the moment it is used (docs/ui/terminal.md).
  bool terminalPanel = false;

  // Import: what a new STL becomes when the import names no representation
  // -- "recognize" or "facets" (mesh::Representation,
  // docs/interchange/README.md#recognising-a-faceted-mesh).
  std::string meshRepresentation = "recognize";

  // Keyboard bindings the user changed, as command id -> chord text. Only
  // the DIFFERENCES from keymap::Defaults() are stored, so a later change to
  // a default still reaches anyone who never touched that row. Not a `Field`:
  // the table above is one scalar per row with a slider or a combo, and a
  // keymap is neither (docs/ui/keybindings.md).
  keymap::Keymap keys;

  // Where the main window was last time. Remembered state, not a preference:
  // it is deliberately NOT in the kFields table, so it never appears in the
  // Preferences window — nobody wants to type their window size. Restored
  // through FitToMonitors, which drops a rect no current monitor can show
  // (core/io/WindowGeometry.h).
  WindowGeometry window;

  // Keys this build does not know, kept verbatim so an older build cannot
  // delete a newer build's preferences just by running once.
  std::vector<std::pair<std::string, std::string>> unknown;

  bool operator==(const Settings& other) const;
};

// One table drives the field list, the JSON round trip and the window rows —
// adding a setting is one row, not three edits that drift apart.
// A `std::vector<std::string>` row is a LIST of `values` (the navigation
// gestures): the JSON holds an array, the window a combo that ticks several.
using Member =
    std::variant<float Settings::*, bool Settings::*, int Settings::*,
                 double Settings::*, std::string Settings::*,
                 std::vector<std::string> Settings::*>;

struct Field {
  const char* key;     // JSON key, and the window's "###id"
  // The Preferences category the row is drawn on: each distinct string is
  // one page, in the order it first appears here (docs/ui/settings.md).
  const char* section;
  const char* label;
  const char* blurb; // the muted line under the control
  Tier tier;
  Member member;
  std::span<const float> choices;      // numeric: the only allowed values
  std::span<const char* const> values; // string: the only allowed values
  float min = 0.0f;                    // numeric range when `choices` is empty
  float max = 0.0f;
  // How the number READS to a person. A font scale of 1.0 shown as "1" tells
  // nobody anything; as "100%" it is obvious. Storage is unaffected.
  float displayScale = 1.0f;
  const char* unit = "";
  // Genuinely free text (a name, an email). Without this a string field is a
  // combo: either over `values`, or over the machine-supplied GPU list.
  // LAST on purpose — Field is initialised positionally, so a member added
  // anywhere else silently re-binds every row after it.
  bool freeText = false;
};

std::span<const Field> Fields();

// After an edit to `key` (the Preferences window's, or a request's): a preset
// chosen writes the four gesture rows; a gesture row edited by hand makes the
// preset whichever one the rows now are, or "custom".
void SyncNavPreset(Settings& s, std::string_view key);

// Puts the rows of one section back to their defaults and leaves the rest --
// the other sections, the keymap, the window and the unknown keys -- as they
// are (Preferences' "Reset <category> to defaults").
void ResetSection(Settings& s, std::string_view section,
                  std::span<const Field> fields = Fields());

// `$XDG_CONFIG_HOME/lobole` (falling back to `~/.config/lobole`) and the
// settings file inside it; `%LOCALAPPDATA%\Lobole\config` on Windows, which
// has neither variable. One resolver for all three per-user directories:
// RuntimePaths::UserConfigDir. CONFIG, not state: the state dir holds what the
// app can regenerate, this holds what the user chose.
std::string ConfigDir();
std::string ConfigPath();

// Absent file = defaults, silently. Corrupt file = defaults, plus the bad
// file set aside as "settings.json.bad-<stamp>" (SetAside) — the one copy of
// a hand-edited config is not ours to destroy — and `note`, when given, says
// so in a sentence the windowed app shows once. A newer settings_format is
// READ, not refused (see the doc).
Settings Load(std::string* note = nullptr);

// Renames `file` to "<file>.bad-<YYYYmmdd-HHMMSS>", never over an earlier
// one (a "-2", "-3" is added instead). Returns the new path, or empty with
// `ec` set when it could not (or `file` does not exist).
std::filesystem::path SetAside(const std::filesystem::path& file,
                               std::error_code& ec);

// Safe mode (docs/ui/settings.md#safe-mode): this session's preferences live
// only in memory. Save then writes nothing and answers success, so every
// caller — the Preferences window, set_settings — works on as usual.
void SetSessionOnly(bool sessionOnly);
bool SessionOnly();

// Atomic, and it re-reads first: two instances run at once here (a windowed
// dev-watch plus headless agents), so a blind write would let one silently
// revert the other. Only keys that differ from `before` are overwritten.
bool Save(const Settings& current, const Settings& before,
          std::string* error = nullptr);

// Convenience for the first save of a session (nothing to merge against).
bool Save(const Settings& current, std::string* error = nullptr);

// JSON for the get_settings / set_settings RPCs and the round-trip tests.
nlohmann::json ToJson(const Settings& s);
// Applies only the keys present in `j`; returns false and leaves `s`
// untouched on an invalid value, naming it in `error`.
bool ApplyJson(Settings& s, const nlohmann::json& j, std::string* error);

} // namespace settings
