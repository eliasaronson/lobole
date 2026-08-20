#include "core/io/Settings.h"

#include "common/Logger.h"
#include "core/io/Serialization.h" // WriteTextAtomic

#include <nlohmann/json.hpp>

#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
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

constexpr std::array<float, 4> kFontScales{0.85f, 1.0f, 1.2f, 1.4f};
constexpr std::array<float, 2> kShadowMaps{1024.0f, 2048.0f};
constexpr std::array<const char* const, 3> kSsao{"off", "half", "full"};

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
    {"gpu_device",
     "Performance",
     "Graphics device",
     "Which GPU renders the viewport. Empty picks a discrete card "
     "automatically. Simulation picks it up on its next run.",
     Tier::Restart,
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
  if (key == kFormatKey)
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
    return fail("out of range");
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
  return nullptr;
}

// Parse the file, or nullopt when it is absent or unreadable-as-JSON.
std::optional<json> ReadFile(const fs::path& path) {
  std::ifstream in(path);
  if (!in)
    return std::nullopt;
  try {
    json j;
    in >> j;
    if (!j.is_object())
      return std::nullopt;
    return j;
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

} // namespace

bool Settings::operator==(const Settings& other) const {
  return uiFontScale == other.uiFontScale && gpuDevice == other.gpuDevice &&
         shadows == other.shadows && ssao == other.ssao &&
         shadowMap == other.shadowMap &&
         autosaveInterval == other.autosaveInterval;
}

std::span<const Field> Fields() { return kFields; }

std::string ConfigDir() {
  // CONFIG, not state: `~/.local/state/designer` holds what the app can
  // regenerate (autosaves, the recent list); this holds what the user chose.
  if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg)
    return (fs::path(xdg) / "designer").string();
  if (const char* home = std::getenv("HOME"); home && *home)
    return (fs::path(home) / ".config" / "designer").string();
  return (fs::temp_directory_path() / "designer-config").string();
}

std::string ConfigPath() {
  return (fs::path(ConfigDir()) / "settings.json").string();
}

Settings Load() {
  Settings s;
  const fs::path path(ConfigPath());
  std::error_code ec;
  if (!fs::exists(path, ec))
    return s; // absent is the normal case, not a problem

  const std::optional<json> parsed = ReadFile(path);
  if (!parsed) {
    // Never overwrite it: the one copy of a hand-edited config is not ours to
    // destroy, and a user who typo'd a brace deserves to get it back.
    const fs::path bak = path.string() + ".bak";
    fs::rename(path, bak, ec);
    LOG_WARN("Settings file unreadable; using defaults and keeping it at {}",
             bak.string());
    return s;
  }
  for (auto it = parsed->begin(); it != parsed->end(); ++it) {
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
  return s;
}

nlohmann::json ToJson(const Settings& s) {
  json j;
  j[kFormatKey] = kFormat;
  for (const Field& f : kFields)
    j[f.key] = FieldValue(s, f);
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
  for (auto it = j.begin(); it != j.end(); ++it) {
    if (it.key() == kFormatKey)
      continue;
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
  s = candidate;
  return true;
}

bool Save(const Settings& current, const Settings& before, std::string* error) {
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
    if (!out.contains(f.key) || now != FieldValue(before, f))
      out[f.key] = now;
  }
  // Keys from a newer build that we read earlier and must not drop.
  for (const auto& [key, raw] : current.unknown)
    if (!out.contains(key))
      out[key] = json::parse(raw, nullptr, false);

  return DocumentIO::WriteTextAtomic(path, out.dump(2) + "\n", error);
}

bool Save(const Settings& current, std::string* error) {
  // No baseline: treat every field as changed.
  Settings none;
  none.uiFontScale = -1.0f;
  none.shadowMap = -1;
  none.autosaveInterval = -1.0;
  none.shadows = !current.shadows;
  none.ssao = "\x01";
  none.gpuDevice = "\x01";
  return Save(current, none, error);
}

} // namespace settings
