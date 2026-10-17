// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/io/SerializePrintSettings.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

using json = nlohmann::json;

namespace DocumentIO {

const char* PrintSettingsKeys() {
  return "walls, top_layers, bottom_layers, infill_pct, infill_pattern, "
         "supports, brim, direction";
}

json PrintSettingsToJson(const printset::Settings& s) {
  json j = json::object();
  if (s.walls)
    j["walls"] = *s.walls;
  if (s.topLayers)
    j["top_layers"] = *s.topLayers;
  if (s.bottomLayers)
    j["bottom_layers"] = *s.bottomLayers;
  if (s.infillPct)
    j["infill_pct"] = *s.infillPct;
  if (s.infill)
    j["infill_pattern"] = printset::InfillName(*s.infill);
  if (s.supports)
    j["supports"] = *s.supports;
  if (s.brim)
    j["brim"] = printset::BrimName(*s.brim);
  if (s.direction)
    j["direction"] = {s.direction->x, s.direction->y, s.direction->z};
  return j;
}

namespace {

int Count(const json& v, const std::string& what, const char* key) {
  if (!v.is_number() || !std::isfinite(v.get<double>()) ||
      v.get<double>() != std::floor(v.get<double>()) ||
      std::abs(v.get<double>()) > 1e6)
    throw std::runtime_error(what + ": " + key + " is a whole number; got " +
                             v.dump());
  return static_cast<int>(v.get<double>());
}

} // namespace

printset::Settings PrintSettingsFromJson(const json& j,
                                         const std::string& what) {
  if (!j.is_object())
    throw std::runtime_error(what + " is an object {" + PrintSettingsKeys() +
                             "}; got " + j.dump());
  printset::Settings s;
  for (const auto& [key, v] : j.items()) {
    if (key == "walls") {
      s.walls = Count(v, what, "walls");
    } else if (key == "top_layers") {
      s.topLayers = Count(v, what, "top_layers");
    } else if (key == "bottom_layers") {
      s.bottomLayers = Count(v, what, "bottom_layers");
    } else if (key == "infill_pct") {
      if (!v.is_number())
        throw std::runtime_error(what +
                                 ": infill_pct is a number, percent; "
                                 "got " +
                                 v.dump());
      s.infillPct = v.get<double>();
    } else if (key == "infill_pattern") {
      const auto i = v.is_string()
                         ? printset::InfillFromName(v.get<std::string>())
                         : std::nullopt;
      if (!i)
        throw std::runtime_error(
            what + ": infill_pattern " + v.dump() +
            " is not a pattern; accepted: " + printset::InfillNames());
      s.infill = *i;
    } else if (key == "supports") {
      if (!v.is_boolean())
        throw std::runtime_error(what + ": supports is true or false; got " +
                                 v.dump());
      s.supports = v.get<bool>();
    } else if (key == "brim") {
      const auto b = v.is_string()
                         ? printset::BrimFromName(v.get<std::string>())
                         : std::nullopt;
      if (!b)
        throw std::runtime_error(
            what + ": brim " + v.dump() +
            " is not a brim; accepted: " + printset::BrimNames());
      s.brim = *b;
    } else if (key == "direction") {
      if (!v.is_array() || v.size() != 3 ||
          !std::all_of(v.begin(), v.end(),
                       [](const json& x) { return x.is_number(); }))
        throw std::runtime_error(what + ": direction is [x, y, z]; got " +
                                 v.dump());
      s.direction = glm::dvec3(v[0].get<double>(), v[1].get<double>(),
                               v[2].get<double>());
    } else {
      throw std::runtime_error(what + ": unknown key '" + key +
                               "'; accepted: " + PrintSettingsKeys());
    }
  }
  if (const std::string why = printset::Validate(s); !why.empty())
    throw std::runtime_error(what + ": " + why);
  return s;
}

json PartPrintSettingsToJson(const printset::PartSettings& s) {
  json j = json::object();
  if (!s.part.Empty())
    j["part"] = PrintSettingsToJson(s.part);
  if (!s.bodies.empty()) {
    json b = json::object();
    for (const auto& [id, settings] : s.bodies)
      b[std::to_string(id)] = PrintSettingsToJson(settings);
    j["bodies"] = std::move(b);
  }
  return j;
}

printset::PartSettings PartPrintSettingsFromJson(const json& j) {
  if (!j.is_object())
    throw std::runtime_error("print_settings is an object {part?, bodies?}");
  printset::PartSettings s;
  for (const auto& [key, v] : j.items()) {
    if (key == "part") {
      s.part = PrintSettingsFromJson(v, "print_settings.part");
    } else if (key == "bodies") {
      if (!v.is_object())
        throw std::runtime_error("print_settings.bodies is an object keyed "
                                 "by the creating feature's id");
      for (const auto& [id, b] : v.items()) {
        // A feature id, all digits: anything else is no id.
        if (id.empty() || id.size() > 19 ||
            !std::all_of(id.begin(), id.end(),
                         [](char c) { return c >= '0' && c <= '9'; }))
          throw std::runtime_error("print_settings.bodies: '" + id +
                                   "' is not a feature id");
        const uint64_t n = std::stoull(id);
        s.bodies[n] = PrintSettingsFromJson(b, "print_settings.bodies." + id);
      }
    } else {
      throw std::runtime_error("print_settings: unknown key '" + key +
                               "'; accepted: part, bodies");
    }
  }
  // Each body's override must also hold over the part's: a part at 100 %
  // rectilinear and a body that names grid prints grid at 100 %.
  for (const auto& [id, b] : s.bodies)
    if (const std::string why = printset::Validate(printset::Merge(s.part, b));
        !why.empty())
      throw std::runtime_error("print_settings.bodies." + std::to_string(id) +
                               " over the part's: " + why);
  return s;
}

} // namespace DocumentIO
