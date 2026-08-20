#pragma once

#include <nlohmann/json_fwd.hpp>

#include <span>
#include <string>
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
  Restart,  // the swapchain and renderers are built once, at startup
};

struct Settings {
  // Appearance
  float uiFontScale = 1.0f; // 0.85 | 1.0 | 1.2 | 1.4, as View > Font Size had

  // Performance
  std::string gpuDevice; // "" = automatic (prefer discrete)
  bool shadows = true;
  std::string ssao = "half"; // off | half | full
  int shadowMap = 2048;      // 1024 | 2048

  // General
  double autosaveInterval = 60.0; // seconds; 0 disables
  // Off by default: a CAD app should not open with a shell in it, and the
  // panel costs a process the moment it is used (docs/ui/terminal-panel.md).
  bool terminalPanel = false;

  // Keys this build does not know, kept verbatim so an older build cannot
  // delete a newer build's preferences just by running once.
  std::vector<std::pair<std::string, std::string>> unknown;

  bool operator==(const Settings& other) const;
};

// One table drives the field list, the JSON round trip and the window rows —
// adding a setting is one row, not three edits that drift apart.
using Member =
    std::variant<float Settings::*, bool Settings::*, int Settings::*,
                 double Settings::*, std::string Settings::*>;

struct Field {
  const char* key;     // JSON key, and the window's "###id"
  const char* section; // window grouping
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
};

std::span<const Field> Fields();

// `$XDG_CONFIG_HOME/designer` (falling back to `~/.config/designer`) and the
// settings file inside it. CONFIG, not state: the state dir holds what the
// app can regenerate, this holds what the user chose.
std::string ConfigDir();
std::string ConfigPath();

// Absent file = defaults, silently. Corrupt file = defaults, plus the bad
// file renamed ".bak" — the one copy of a hand-edited config is not ours to
// destroy. A newer settings_format is READ, not refused (see the doc).
Settings Load();

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
