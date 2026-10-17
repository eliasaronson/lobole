// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/PrintSettings.h"

#include <cmath>
#include <sstream>

namespace printset {

namespace {

struct InfillRow {
  Infill infill;
  const char* name;
  const char* bambu;
  bool solid;
};
// The solid ones are Bambu Studio's top-surface patterns that are also
// sparse ones: what it lets a 100 % infill use.
constexpr InfillRow kInfills[] = {
    {Infill::Rectilinear, "rectilinear", "zig-zag", true},
    {Infill::AlignedRectilinear, "aligned_rectilinear", "alignedrectilinear",
     true},
    {Infill::Grid, "grid", "grid", false},
    {Infill::Gyroid, "gyroid", "gyroid", false},
    {Infill::Honeycomb, "honeycomb", "honeycomb", false},
    {Infill::Cubic, "cubic", "cubic", false},
    {Infill::Triangles, "triangles", "triangles", false},
    {Infill::Line, "line", "line", false},
    {Infill::Concentric, "concentric", "concentric", true},
    {Infill::Lightning, "lightning", "lightning", false},
    {Infill::AdaptiveCubic, "adaptive_cubic", "adaptivecubic", false},
    {Infill::TriHexagon, "tri_hexagon", "tri-hexagon", false},
    {Infill::Honeycomb3d, "3d_honeycomb", "3dhoneycomb", false},
    {Infill::CrossHatch, "cross_hatch", "crosshatch", false},
    {Infill::HilbertCurve, "hilbert_curve", "hilbertcurve", true},
};

struct BrimRow {
  Brim brim;
  const char* name;
  const char* bambu;
};
constexpr BrimRow kBrims[] = {
    {Brim::None, "none", "no_brim"},
    {Brim::Outer, "outer", "outer_only"},
    {Brim::Inner, "inner", "inner_only"},
    {Brim::Both, "both", "outer_and_inner"},
    {Brim::Ears, "ears", "brim_ears"},
};

const InfillRow& RowOf(Infill i) {
  for (const InfillRow& r : kInfills)
    if (r.infill == i)
      return r;
  return kInfills[0];
}

const BrimRow& RowOf(Brim b) {
  for (const BrimRow& r : kBrims)
    if (r.brim == b)
      return r;
  return kBrims[0];
}

std::string Number(double v) {
  std::ostringstream s;
  s << v;
  return s.str();
}

} // namespace

const std::vector<Infill>& AllInfills() {
  static const std::vector<Infill> all = [] {
    std::vector<Infill> v;
    for (const InfillRow& r : kInfills)
      v.push_back(r.infill);
    return v;
  }();
  return all;
}

std::string InfillName(Infill i) { return RowOf(i).name; }

std::optional<Infill> InfillFromName(const std::string& name) {
  for (const InfillRow& r : kInfills)
    if (name == r.name)
      return r.infill;
  return std::nullopt;
}

const std::string& InfillNames() {
  static const std::string names = [] {
    std::string s;
    for (const InfillRow& r : kInfills)
      s += (s.empty() ? "" : ", ") + std::string(r.name);
    return s;
  }();
  return names;
}

const char* BambuInfillName(Infill i) { return RowOf(i).bambu; }

bool PrintsSolid(Infill i) { return RowOf(i).solid; }

std::string SolidInfillNames() {
  std::string s;
  for (const InfillRow& r : kInfills)
    if (r.solid)
      s += (s.empty() ? "" : ", ") + std::string(r.name);
  return s;
}

const std::vector<Brim>& AllBrims() {
  static const std::vector<Brim> all = [] {
    std::vector<Brim> v;
    for (const BrimRow& r : kBrims)
      v.push_back(r.brim);
    return v;
  }();
  return all;
}

std::string BrimName(Brim b) { return RowOf(b).name; }

std::optional<Brim> BrimFromName(const std::string& name) {
  for (const BrimRow& r : kBrims)
    if (name == r.name)
      return r.brim;
  return std::nullopt;
}

const std::string& BrimNames() {
  static const std::string names = [] {
    std::string s;
    for (const BrimRow& r : kBrims)
      s += (s.empty() ? "" : ", ") + std::string(r.name);
    return s;
  }();
  return names;
}

const char* BambuBrimName(Brim b) { return RowOf(b).bambu; }

bool Settings::Empty() const { return *this == Settings{}; }

Settings Merge(const Settings& under, const Settings& over) {
  Settings s = under;
  if (over.walls)
    s.walls = over.walls;
  if (over.topLayers)
    s.topLayers = over.topLayers;
  if (over.bottomLayers)
    s.bottomLayers = over.bottomLayers;
  if (over.infillPct)
    s.infillPct = over.infillPct;
  if (over.infill)
    s.infill = over.infill;
  if (over.supports)
    s.supports = over.supports;
  if (over.brim)
    s.brim = over.brim;
  if (over.direction)
    s.direction = over.direction;
  return s;
}

std::string Validate(const Settings& s) {
  if (s.walls && *s.walls < 1)
    return "walls is at least 1 (a wall loop is what holds the outline); "
           "got " +
           std::to_string(*s.walls);
  if (s.topLayers && *s.topLayers < 0)
    return "top_layers is 0 or more; got " + std::to_string(*s.topLayers);
  if (s.bottomLayers && *s.bottomLayers < 0)
    return "bottom_layers is 0 or more; got " + std::to_string(*s.bottomLayers);
  if (s.infillPct && (!std::isfinite(*s.infillPct) || *s.infillPct < 0.0 ||
                      *s.infillPct > 100.0))
    return "infill_pct is a percentage from 0 to 100; got " +
           Number(*s.infillPct);
  if (s.direction) {
    const glm::dvec3& d = *s.direction;
    if (!std::isfinite(d.x) || !std::isfinite(d.y) || !std::isfinite(d.z) ||
        glm::length(d) < 1e-9)
      return "direction is the print direction in the part's frame, a "
             "non-zero vector [x, y, z]";
  }
  if (s.infillPct && *s.infillPct >= 100.0) {
    if (!s.infill)
      return "100 % infill needs its pattern stated: the slicer preset's "
             "may be one that cannot print solid (Bambu's default grid "
             "cannot). Set infill_pattern to one of " +
             SolidInfillNames();
    if (!PrintsSolid(*s.infill))
      return "the " + InfillName(*s.infill) +
             " pattern cannot print 100 % infill (the slicer refuses it "
             "there); at 100 % use one of " +
             SolidInfillNames();
  }
  return {};
}

bool PartSettings::Empty() const { return part.Empty() && bodies.empty(); }

Settings PartSettings::For(uint64_t createdBy) const {
  const auto it = bodies.find(createdBy);
  return it == bodies.end() ? part : Merge(part, it->second);
}

std::vector<std::pair<std::string, std::string>>
BambuObjectSettings(const Settings& s, double layerHeightMm) {
  std::vector<std::pair<std::string, std::string>> out;
  if (layerHeightMm > 0.0)
    out.emplace_back("layer_height", Number(layerHeightMm));
  if (s.walls)
    out.emplace_back("wall_loops", std::to_string(*s.walls));
  if (s.topLayers) {
    out.emplace_back("top_shell_layers", std::to_string(*s.topLayers));
    out.emplace_back("top_shell_thickness", "0");
  }
  if (s.bottomLayers) {
    out.emplace_back("bottom_shell_layers", std::to_string(*s.bottomLayers));
    out.emplace_back("bottom_shell_thickness", "0");
  }
  if (s.infillPct)
    out.emplace_back("sparse_infill_density", Number(*s.infillPct) + "%");
  if (s.infill)
    out.emplace_back("sparse_infill_pattern", BambuInfillName(*s.infill));
  out.emplace_back("enable_support", s.supports.value_or(false) ? "1" : "0");
  out.emplace_back("brim_type", BambuBrimName(s.brim.value_or(Brim::None)));
  return out;
}

std::string Describe(const Settings& s) {
  const auto n = [](const std::optional<int>& v) {
    return v ? std::to_string(*v) : std::string("preset");
  };
  std::string out = "walls " + n(s.walls) + ", top/bottom " + n(s.topLayers) +
                    "/" + n(s.bottomLayers) + ", ";
  out += s.infillPct ? Number(*s.infillPct) + " %" : std::string("preset %");
  out += " " + (s.infill ? InfillName(*s.infill) : std::string("preset"));
  out += s.supports.value_or(false) ? ", supports" : ", no supports";
  const Brim b = s.brim.value_or(Brim::None);
  out += b == Brim::None ? ", no brim" : ", brim " + BrimName(b);
  return out;
}

} // namespace printset
