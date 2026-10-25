// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/io/Settings.h"

#include "common/Logger.h"
#include "common/RuntimePaths.h"
#include "core/io/ReadContext.h"   // ParseStrict
#include "core/io/Serialization.h" // WriteTextAtomic

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>

namespace fs = std::filesystem;
using nlohmann::json;

namespace settings {

namespace {

// Bumped only when an existing key CHANGES MEANING; adding keys needs no bump
// (unknown keys are preserved, missing ones default). A newer number is read,
// not refused — see docs/ui/settings.md.
constexpr int kFormat = 1;
constexpr const char* kFormatKey = "settings_format";
// The keymap is one nested object rather than a `Field`, because a chord is
// not a scalar with a range or a fixed choice list. It gets its own handling
// in every function that walks kFields (docs/ui/keybindings.md).
constexpr const char* kKeysKey = "keys";
// Remembered window rect. Like kKeysKey, outside the kFields table: it is
// state, not a preference, and must not show up in Preferences.
constexpr const char* kWindowKey = "window";

nlohmann::json WindowToJson(const WindowGeometry& g) {
  return {{"x", g.x},
          {"y", g.y},
          {"w", g.w},
          {"h", g.h},
          {"maximized", g.maximized}};
}

// A malformed entry loses the REMEMBERED RECT, not the settings file:
// FitToMonitors treats an unremembered geometry as a first run. Each field
// is checked before it is read, as ReadField checks a preference: a string
// or a 1e400 where a coordinate goes used to throw out of Load, and so out
// of main, before the window opened.
WindowGeometry WindowFromJson(const nlohmann::json& j,
                              std::string* why = nullptr) {
  WindowGeometry g;
  const auto bad = [&](const std::string& reason) {
    if (why)
      *why = "window: " + reason;
    return WindowGeometry{};
  };
  if (!j.is_object())
    return bad("expected an object");
  int* const ints[] = {&g.x, &g.y, &g.w, &g.h};
  const char* const keys[] = {"x", "y", "w", "h"};
  for (size_t i = 0; i < 4; i++) {
    const auto it = j.find(keys[i]);
    if (it == j.end())
      continue;
    if (!it->is_number_integer() ||
        it->get<int64_t>() < std::numeric_limits<int>::min() ||
        it->get<int64_t>() > std::numeric_limits<int>::max() ||
        (it->is_number_unsigned() &&
         it->get<uint64_t>() >
             static_cast<uint64_t>(std::numeric_limits<int>::max())))
      return bad(std::string(keys[i]) + " is " + it->dump() +
                 ", not a whole number of pixels");
    *ints[i] = it->get<int>();
  }
  if (const auto it = j.find("maximized"); it != j.end()) {
    if (!it->is_boolean())
      return bad("maximized is " + it->dump() + ", not true or false");
    g.maximized = it->get<bool>();
  }
  return g;
}

constexpr std::array<float, 4> kFontScales{0.85f, 1.0f, 1.2f, 1.4f};
constexpr std::array<float, 2> kShadowMaps{1024.0f, 2048.0f};
constexpr std::array<const char* const, 3> kSsao{"off", "half", "full"};

// Rows are grouped by section. Each section is one Preferences category, in
// the order it first appears here; the window no longer depends on the rows
// being together, but the table should read the way it draws, and `the
// settings table groups its sections` holds it.
const Field kFields[] = {
    {"ui_font_scale",
     "Appearance",
     "Font size",
     "Scales the whole interface. Applied on the next frame.",
     Tier::Live,
     &Settings::uiFontScale,
     kFontScales,
     {},
     0.0f,
     0.0f,
     100.0f,
     "%"},
    {"lock_lights_to_model",
     "Appearance",
     "Lock lights to model",
     "Off, the lights follow the camera: every side you turn to is lit the "
     "same way, and shadows move with the view. On, they stay fixed in the "
     "world where they stand at the home view.",
     Tier::Live,
     &Settings::lockLightsToModel,
     {},
     {},
     0.0f,
     0.0f},
    {"zoom_to_cursor",
     "Navigation",
     "Zoom to cursor",
     "On, scrolling zooms toward whatever is under the pointer, which stays "
     "where it is on screen. Off, it zooms toward the centre of the view.",
     Tier::Live,
     &Settings::zoomToCursor,
     {},
     {},
     0.0f,
     0.0f},
    // Mouse navigation (core/io/MouseNav.h, docs/ui/keybindings.md). The
    // preset row writes the four gesture rows below it; each of those is a
    // LIST, so one action can sit on several gestures, and no gesture may sit
    // on two actions.
    {"nav_preset",
     "Navigation",
     "Mouse navigation",
     "Which program's mouse navigation the four rows below follow. Choosing "
     "one rewrites them; changing one by hand makes this Custom.",
     Tier::Live,
     &Settings::navPreset,
     {},
     mousenav::PresetNames(),
     0.0f,
     0.0f},
    {"nav_orbit",
     "Navigation",
     "Rotate",
     "The drags that rotate the view about the pivot. The left button "
     "navigates only with Alt held: alone it selects.",
     Tier::Live,
     &Settings::navOrbit,
     {},
     mousenav::AcceptedGestures(),
     0.0f,
     0.0f},
    {"nav_pan",
     "Navigation",
     "Pan",
     "The drags that slide the view. On a drawing sheet, the rotate drags "
     "pan too.",
     Tier::Live,
     &Settings::navPan,
     {},
     mousenav::AcceptedGestures(),
     0.0f,
     0.0f},
    {"nav_zoom",
     "Navigation",
     "Zoom",
     "The drags that zoom: down zooms out, up in, about where the drag "
     "started.",
     Tier::Live,
     &Settings::navZoom,
     {},
     mousenav::AcceptedGestures(),
     0.0f,
     0.0f},
    {"nav_roll",
     "Navigation",
     "Roll",
     "The drags that turn the view about the line of sight.",
     Tier::Live,
     &Settings::navRoll,
     {},
     mousenav::AcceptedGestures(),
     0.0f,
     0.0f},
    {"nav_orbit_speed",
     "Navigation",
     "Rotate speed",
     "How far a drag of the same length turns the view.",
     Tier::Live,
     &Settings::navOrbitSpeed,
     {},
     {},
     0.25f,
     4.0f,
     1.0f,
     "x"},
    {"nav_pan_speed",
     "Navigation",
     "Pan speed",
     "How far a drag of the same length slides the view.",
     Tier::Live,
     &Settings::navPanSpeed,
     {},
     {},
     0.25f,
     4.0f,
     1.0f,
     "x"},
    {"nav_zoom_speed",
     "Navigation",
     "Zoom speed",
     "How far one wheel notch, or a zoom drag of the same length, zooms.",
     Tier::Live,
     &Settings::navZoomSpeed,
     {},
     {},
     0.25f,
     4.0f,
     1.0f,
     "x"},
    {"nav_invert_wheel",
     "Navigation",
     "Invert wheel zoom",
     "Off, rolling the wheel away from you zooms in. On, it zooms out. The "
     "drawing sheet follows it too.",
     Tier::Live,
     &Settings::navInvertWheel,
     {},
     {},
     0.0f,
     0.0f},
    {"gpu_device",
     "Performance",
     "Graphics device",
     "Which GPU renders the viewport. Empty picks a discrete card "
     "automatically. The viewport switches to it at once, rebuilding its "
     "device in place; simulation picks it up on its next run.",
     Tier::Live,
     &Settings::gpuDevice,
     {},
     {},
     0.0f,
     0.0f},
    {"shadows",
     "Performance",
     "Shadows",
     "Key-light shadow map. Off is faster on a weak GPU.",
     Tier::Live,
     &Settings::shadows,
     {},
     {},
     0.0f,
     0.0f},
    {"ssao",
     "Performance",
     "Ambient occlusion",
     "Contact shading. Half resolution costs little; full is sharper.",
     Tier::Live,
     &Settings::ssao,
     {},
     kSsao,
     0.0f,
     0.0f},
    {"idle_throttle",
     "Performance",
     "Limit idle rendering",
     "Drop to ~10 fps while nothing is happening. Saves battery and GPU "
     "headroom; input picks the rate back up instantly.",
     Tier::Live,
     &Settings::idleThrottle,
     {},
     {},
     0.0f,
     0.0f},
    {"shadow_map",
     "Performance",
     "Shadow map size",
     "Texels per side. 2048 is crisper, 1024 cheaper.",
     Tier::Live,
     &Settings::shadowMap,
     kShadowMaps,
     {},
     0.0f,
     0.0f},
    // Sharing (F9 C1). Empty is the right answer for anyone who has
    // configured git — these fill the gap for anyone who has not, and never
    // override what git already knows.
    {"git_user_name",
     "Sharing",
     "Your name",
     "On commits this app makes, when git has no user.name of its own.",
     Tier::NextTime,
     &Settings::gitUserName,
     {},
     {},
     0.0f,
     0.0f,
     1.0f,
     "",
     true},
    {"git_user_email",
     "Sharing",
     "Your email",
     "On commits this app makes, when git has no user.email of its own.",
     Tier::NextTime,
     &Settings::gitUserEmail,
     {},
     {},
     0.0f,
     0.0f,
     1.0f,
     "",
     true},
    {"terminal_panel",
     "General",
     "Terminal panel",
     "A terminal docked on the right, for running Claude Code (or a shell, "
     "or git) beside the model. Ctrl+Shift+T opens another tab. Typing goes "
     "to the terminal only while the mouse is over it. Off by default.",
     Tier::Live,
     &Settings::terminalPanel,
     {},
     {},
     0.0f,
     0.0f},
    {"autosave_interval",
     "General",
     "Autosave every",
     "Seconds between crash-recovery snapshots. 0 turns autosave off.",
     Tier::NextTime,
     &Settings::autosaveInterval,
     {},
     {},
     0.0f,
     3600.0f},
};

// Every key this build knows, including the format marker — anything else in
// the file is somebody else's and gets carried through untouched.
bool IsKnownKey(const std::string& key) {
  if (key == kFormatKey || key == kKeysKey || key == kWindowKey)
    return true;
  for (const Field& f : kFields)
    if (key == f.key)
      return true;
  return false;
}

// Read one field out of `j` into `s`. Returns false (with `error` named) on a
// value the field does not allow, leaving `s` untouched for that field.
bool ReadField(Settings& s, const Field& f, const json& value,
               std::string* error) {
  const auto fail = [&](const std::string& why) {
    if (error)
      *error = std::string(f.key) + ": " + why;
    return false;
  };
  if (auto* m = std::get_if<std::vector<std::string> Settings::*>(&f.member)) {
    // The gesture rows: a list, each entry a gesture, kept in its canonical
    // text so "Ctrl+Middle" and "ctrl+middle" are one gesture to everything
    // that compares them.
    if (!value.is_array())
      return fail("expected a list of gestures, e.g. [\"middle\"]");
    std::vector<std::string> out;
    for (const json& item : value) {
      if (!item.is_string())
        return fail("expected a list of gestures, each a string");
      std::string why;
      const std::optional<mousenav::Gesture> g =
          mousenav::ParseGesture(item.get<std::string>(), &why);
      if (!g)
        return fail(why);
      const std::string text = mousenav::FormatGesture(*g);
      if (std::find(out.begin(), out.end(), text) == out.end())
        out.push_back(text);
    }
    s.*(*m) = std::move(out);
    return true;
  }
  if (auto* m = std::get_if<std::string Settings::*>(&f.member)) {
    if (!value.is_string())
      return fail("expected a string");
    const std::string v = value.get<std::string>();
    if (!f.values.empty()) {
      bool ok = false;
      std::string allowed;
      for (const char* c : f.values) {
        ok = ok || v == c;
        allowed += (allowed.empty() ? "" : ", ") + std::string(c);
      }
      if (!ok)
        return fail("'" + v + "' is not one of " + allowed);
    }
    s.*(*m) = v;
    return true;
  }
  // Booleans before the numeric check: a JSON `true` is not a number, so
  // testing that first rejected every bool in the table.
  if (auto* m = std::get_if<bool Settings::*>(&f.member)) {
    if (!value.is_boolean())
      return fail("expected true or false");
    s.*(*m) = value.get<bool>();
    return true;
  }
  if (!value.is_number())
    return fail("expected a number");
  const double v = value.get<double>();
  if (!f.choices.empty()) {
    bool ok = false;
    std::string allowed;
    for (float c : f.choices) {
      ok = ok || std::abs(v - c) < 1e-6;
      allowed += (allowed.empty() ? "" : ", ") + std::to_string(c);
    }
    if (!ok)
      return fail("not one of " + allowed);
  } else if (f.max > f.min && (v < f.min || v > f.max)) {
    char range[64];
    std::snprintf(range, sizeof range, "%g out of range; %g to %g", v,
                  static_cast<double>(f.min), static_cast<double>(f.max));
    return fail(range);
  }
  if (auto* m = std::get_if<float Settings::*>(&f.member))
    s.*(*m) = static_cast<float>(v);
  else if (auto* m = std::get_if<double Settings::*>(&f.member))
    s.*(*m) = v;
  else if (auto* m = std::get_if<int Settings::*>(&f.member))
    s.*(*m) = static_cast<int>(v);
  return true;
}

json FieldValue(const Settings& s, const Field& f) {
  if (auto* m = std::get_if<float Settings::*>(&f.member))
    return s.*(*m);
  if (auto* m = std::get_if<double Settings::*>(&f.member))
    return s.*(*m);
  if (auto* m = std::get_if<int Settings::*>(&f.member))
    return s.*(*m);
  if (auto* m = std::get_if<bool Settings::*>(&f.member))
    return s.*(*m);
  if (auto* m = std::get_if<std::string Settings::*>(&f.member))
    return s.*(*m);
  if (auto* m = std::get_if<std::vector<std::string> Settings::*>(&f.member))
    return s.*(*m);
  return nullptr;
}

bool IsNavRowKey(std::string_view key) {
  for (const char* k : mousenav::kActionKeys)
    if (key == k)
      return true;
  return false;
}

// A preset named in `j` writes the four rows BEFORE the keys are read, so a
// request (or a file) that names a preset and one row gets the preset with
// that row changed, whatever order the keys come in. Unknown preset: refused
// by name, as ReadField refuses any fixed-value row.
bool ApplyNavPresetFirst(Settings& s, const json& j, std::string* error) {
  const auto it = j.find("nav_preset");
  if (it == j.end())
    return true;
  for (const Field& f : kFields)
    if (std::string_view(f.key) == "nav_preset" && !ReadField(s, f, *it, error))
      return false;
  if (const mousenav::Preset* p = mousenav::FindPreset(s.navPreset))
    s.SetNavRows(p->rows);
  return true;
}

// The rows checked TOGETHER: each was checked alone as it was read, but a
// gesture claimed by two actions is only visible across them.
bool FinishNav(Settings& s, std::string* error) {
  s.navPreset = mousenav::MatchPreset(s.NavRows());
  return mousenav::NavMap::Build(s.NavRows(), error).has_value();
}

// Parse the file, or nullopt when it is absent or unreadable-as-JSON.
// The whole file, strictly: `in >> j` read the first value and stopped, so
// a file with something after it -- written twice, or a second half from a
// torn sync -- loaded its first half, and the next save dropped the rest
// without a word. A key written twice is unreadable too
// (readctx::ParseStrict).
std::optional<json> ReadFile(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in)
    return std::nullopt;
  try {
    const json j =
        readctx::ParseStrict(std::string((std::istreambuf_iterator<char>(in)),
                                         std::istreambuf_iterator<char>()));
    if (!j.is_object())
      return std::nullopt;
    return j;
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

} // namespace

bool Settings::operator==(const Settings& other) const {
  return uiFontScale == other.uiFontScale &&
         lockLightsToModel == other.lockLightsToModel &&
         zoomToCursor == other.zoomToCursor && navPreset == other.navPreset &&
         navOrbit == other.navOrbit && navPan == other.navPan &&
         navZoom == other.navZoom && navRoll == other.navRoll &&
         navOrbitSpeed == other.navOrbitSpeed &&
         navPanSpeed == other.navPanSpeed &&
         navZoomSpeed == other.navZoomSpeed &&
         navInvertWheel == other.navInvertWheel &&
         gpuDevice == other.gpuDevice && shadows == other.shadows &&
         ssao == other.ssao && shadowMap == other.shadowMap &&
         idleThrottle == other.idleThrottle &&
         autosaveInterval == other.autosaveInterval &&
         terminalPanel == other.terminalPanel &&
         gitUserName == other.gitUserName &&
         gitUserEmail == other.gitUserEmail && keys == other.keys &&
         window == other.window;
}

std::span<const Field> Fields() { return kFields; }

void SyncNavPreset(Settings& s, std::string_view key) {
  if (key == "nav_preset")
    if (const mousenav::Preset* p = mousenav::FindPreset(s.navPreset))
      s.SetNavRows(p->rows);
  if (key == "nav_preset" || IsNavRowKey(key))
    s.navPreset = mousenav::MatchPreset(s.NavRows());
}

void ResetSection(Settings& s, std::string_view section,
                  std::span<const Field> fields) {
  const Settings defaults;
  for (const Field& f : fields)
    if (section == f.section)
      std::visit([&](auto member) { s.*member = defaults.*member; }, f.member);
}

std::string ConfigDir() {
  // CONFIG, not state: the state directory holds what the app can regenerate
  // (autosaves, the recent list); this holds what the user chose.
  return RuntimePaths::UserConfigDir().string();
}

std::string ConfigPath() {
  return (fs::path(ConfigDir()) / "settings.json").string();
}

fs::path SetAside(const fs::path& file, std::error_code& ec) {
  ec.clear();
  if (!fs::exists(file, ec)) {
    if (!ec)
      ec = std::make_error_code(std::errc::no_such_file_or_directory);
    return {};
  }
  const std::time_t now = std::time(nullptr);
  std::tm tm{};
#if defined(_WIN32)
  localtime_s(&tm, &now);
#else
  localtime_r(&now, &tm);
#endif
  char stamp[32];
  std::strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &tm);
  const std::string base = file.string() + ".bad-" + stamp;
  fs::path to = base;
  for (int n = 2; fs::exists(to, ec); ++n)
    to = base + "-" + std::to_string(n);
  fs::rename(file, to, ec);
  return ec ? fs::path() : to;
}

namespace {
bool g_sessionOnly = false;
} // namespace

void SetSessionOnly(bool sessionOnly) { g_sessionOnly = sessionOnly; }
bool SessionOnly() { return g_sessionOnly; }

Settings Load(std::string* note) {
  Settings s;
  const fs::path path(ConfigPath());
  std::error_code ec;
  if (!fs::exists(path, ec))
    return s; // absent is the normal case, not a problem

  const std::optional<json> parsed = ReadFile(path);
  if (!parsed) {
    // Never overwrite it: the one copy of a hand-edited config is not ours to
    // destroy, and a user who typo'd a brace deserves to get it back. Never
    // over an earlier one either: two bad starts used to leave only the
    // second's ".bak".
    const fs::path kept = SetAside(path, ec);
    LOG_WARN("Settings file unreadable; using defaults and keeping it at {}",
             kept.empty() ? path.string() : kept.string());
    if (note)
      *note = kept.empty()
                  ? "Your settings file " + path.string() +
                        " could not be read, so this session runs on the "
                        "defaults."
                  : "Your settings file could not be read, so Loböle "
                    "started with the defaults. It was kept as " +
                        kept.string() + ".";
    return s;
  }
  {
    std::string error;
    if (!ApplyNavPresetFirst(s, *parsed, &error))
      LOG_WARN("Settings: ignoring {} ({})", error, path.string());
  }
  for (auto it = parsed->begin(); it != parsed->end(); ++it) {
    if (it.key() == "nav_preset")
      continue; // read first, above
    if (it.key() == kWindowKey) {
      std::string why;
      s.window = WindowFromJson(it.value(), &why);
      if (!why.empty())
        LOG_WARN("Settings: ignoring {} ({})", why, path.string());
      continue;
    }
    if (it.key() == kKeysKey) {
      // A bad chord in a hand-edited file loses that ONE binding, not the
      // whole keymap: the same rule the scalar fields get.
      std::string error;
      if (!keymap::ApplyOverridesJson(s.keys, it.value(), &error))
        LOG_WARN("Settings: ignoring {} ({})", error, path.string());
      continue;
    }
    if (!IsKnownKey(it.key())) {
      s.unknown.emplace_back(it.key(), it.value().dump());
      continue;
    }
    for (const Field& f : kFields) {
      if (it.key() != f.key)
        continue;
      std::string error;
      if (!ReadField(s, f, it.value(), &error))
        LOG_WARN("Settings: ignoring {} ({})", error, path.string());
      break;
    }
  }
  // Two rows claiming one gesture: neither can be preferred, so the
  // navigation goes back to the defaults WHOLE, and says why -- the rule a
  // bad single value gets, at the size of the thing that was bad.
  if (std::string error; !FinishNav(s, &error)) {
    LOG_WARN("Settings: ignoring the mouse navigation rows ({}); using the "
             "defaults ({})",
             error, path.string());
    s.SetNavRows(mousenav::DefaultRows());
    s.navPreset = mousenav::MatchPreset(s.NavRows());
  }
  return s;
}

nlohmann::json ToJson(const Settings& s) {
  json j;
  j[kFormatKey] = kFormat;
  for (const Field& f : kFields)
    j[f.key] = FieldValue(s, f);
  j[kKeysKey] = keymap::OverridesToJson(s.keys);
  j[kWindowKey] = WindowToJson(s.window);
  return j;
}

bool ApplyJson(Settings& s, const nlohmann::json& j, std::string* error) {
  if (!j.is_object()) {
    if (error)
      *error = "settings must be an object";
    return false;
  }
  // Validate against a copy first: a rejected value must not leave half the
  // request applied.
  Settings candidate = s;
  if (!ApplyNavPresetFirst(candidate, j, error))
    return false;
  for (auto it = j.begin(); it != j.end(); ++it) {
    if (it.key() == kFormatKey || it.key() == "nav_preset")
      continue;
    if (it.key() == kWindowKey) {
      // `candidate`, not `s`: this function's whole contract is that a
      // rejected value later in the object leaves nothing half-applied.
      std::string why;
      candidate.window = WindowFromJson(it.value(), &why);
      if (!why.empty()) {
        if (error)
          *error = why;
        return false;
      }
      continue;
    }
    if (it.key() == kKeysKey) {
      if (!keymap::ApplyOverridesJson(candidate.keys, it.value(), error))
        return false;
      continue;
    }
    const Field* field = nullptr;
    for (const Field& f : kFields)
      if (it.key() == f.key)
        field = &f;
    if (!field) {
      if (error)
        *error = "unknown setting: " + it.key();
      return false;
    }
    if (!ReadField(candidate, *field, it.value(), error))
      return false;
  }
  if (!FinishNav(candidate, error))
    return false;
  s = candidate;
  return true;
}

namespace {

// `before` null: no baseline, so every row, the keymap and the window are
// written as `current` has them.
bool SaveMerged(const Settings& current, const Settings* before,
                std::string* error) {
  if (g_sessionOnly)
    return true; // safe mode: this session's preferences stay in memory
  std::error_code ec;
  fs::create_directories(ConfigDir(), ec);
  const fs::path path(ConfigPath());

  // Re-read and merge rather than write our whole struct: two instances run
  // at once here (a windowed dev-watch plus headless agents), and a blind
  // write would silently revert whatever the other one changed. Only the
  // keys THIS instance actually changed are overwritten.
  json out;
  if (const std::optional<json> disk = ReadFile(path))
    out = *disk;
  out[kFormatKey] = kFormat;
  for (const Field& f : kFields) {
    const json now = FieldValue(current, f);
    if (!before || !out.contains(f.key) || now != FieldValue(*before, f))
      out[f.key] = now;
  }
  // The keymap is written whole when it differs: per-binding merging across
  // two running instances buys nothing over the last-writer-wins the rest of
  // this file already accepts for a single field.
  if (!before || !out.contains(kKeysKey) || !(current.keys == before->keys))
    out[kKeysKey] = keymap::OverridesToJson(current.keys);
  // Only when THIS instance moved its window: two instances run here, and a
  // blind write would teleport the other one on its next launch.
  if (!before || current.window != before->window)
    out[kWindowKey] = WindowToJson(current.window);

  // Keys from a newer build that we read earlier and must not drop.
  for (const auto& [key, raw] : current.unknown)
    if (!out.contains(key))
      out[key] = json::parse(raw, nullptr, false);

  return DocumentIO::WriteTextAtomic(path, out.dump(2) + "\n", error);
}

} // namespace

bool Save(const Settings& current, const Settings& before, std::string* error) {
  return SaveMerged(current, &before, error);
}

bool Save(const Settings& current, std::string* error) {
  // No baseline: every value is written. A baseline faked to differ from
  // `current` missed every row someone forgot to fake, and a value already
  // in the file then survived a save that should have replaced it.
  return SaveMerged(current, nullptr, error);
}

} // namespace settings
