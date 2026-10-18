// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/PrintAllowance.h"

#include "core/PrintCheck.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

namespace printfit {
namespace {

constexpr double kAbove = kAboveMm;

// The sources, once (docs/tolerances/README.md#printed-parts says where each
// number comes from).
constexpr const char* kHoleSource =
    "FDM holes print 0.1-0.4 mm undersize on the diameter and the fix is to "
    "model them about 0.2 mm oversize (3dprintbounty.com, 3D printing "
    "tolerances guide); PETG holes read 0.2-0.3 mm under on Ø10, and PETG "
    "shrinks 0.5-0.8 % against PLA's 0.3-0.5 % "
    "(docs/tolerances/README.md#printed-parts)";
constexpr const char* kShaftSource =
    "a PETG sliding fit wants 0.2-0.3 mm per side in all (sovol3d.com, FDM "
    "tolerances and clearances; 0.1-0.2 mm per side in the community "
    "range) and the hole row takes most of it: an outside diameter grows by "
    "about a tenth";
constexpr const char* kInternalThreadSource =
    "FreeCAD Fasteners workbench, 3D-printer mode: a nut at 1.03·D + 0.10 "
    "(M8 -> +0.34); BOSL2 threading.scad enlarges an internal thread by "
    "4·$slop, 0.4 mm at the usual $slop of 0.1";
constexpr const char* kExternalThreadSource =
    "FreeCAD Fasteners workbench, 3D-printer mode: a screw at 0.99·d - 0.05 "
    "(M8 -> -0.13)";
constexpr const char* kPlaHoleSource =
    "FDM holes print 0.1-0.4 mm undersize on the diameter and the fix is to "
    "model them about 0.2 mm oversize (3dprintbounty.com, 3D printing "
    "tolerances guide); PLA shrinks 0.3-0.5 % against PETG's 0.5-0.8 %, so "
    "the PETG table's extra 0.1 from Ø10 is not carried over";
constexpr const char* kPlaShaftSource =
    "an outside diameter grows by about a tenth in FDM, the material's "
    "shrinkage well inside that (3dprintbounty.com, 3D printing tolerances "
    "guide)";
constexpr const char* kPlaGapSource =
    "A DEFAULT to calibrate: print-in-place PLA wants about 0.25 mm per side "
    "as a starting point (sovol3d.com, print-in-place hinges and joints: "
    "0.15-0.35 mm by printer and filament); on Bambu Lab printers 0.2 mm "
    "still turns, 0.25 is the best fit and 0.3 is loose (makerworld.com "
    "clearance tests). A printer can want much less -- one P1S slid at 0.1 "
    "a side in K5 PLA and 0.15 in a plain PLA -- so print the coupon and "
    "keep the answer in a print profile";
constexpr const char* kPetgGapSource =
    "print-in-place clearance is 0.15-0.35 mm per side by printer and "
    "filament (sovol3d.com, print-in-place hinges and joints); PETG strings "
    "and bonds to itself more than PLA, so it takes 0.3; not yet measured "
    "here, as the PLA row has been";

constexpr const char* kPressSource =
    "A DEFAULT to confirm with the coupon's press row: a printed pin pressed "
    "into a printed bore is commonly modelled with the bore at the pin's "
    "size, or up to 0.1 mm under it for a tight fit (Prusa knowledge base, "
    "modelling tolerances; the 3D-printing press-fit guides agree within "
    "that). FDM already prints a bore small and a pin large -- the hole and "
    "shaft rows -- which is where the hold comes from at 0, and why a bore "
    "modelled much under the pin splits a thin cap. Print print_coupon.py's "
    "press row and keep the smallest step that holds by hand and does not "
    "split (--record --press-side)";

// TPU 95A (docs/tolerances/README.md#tpu): every row a published
// starting point, none measured here -- each to confirm with a coupon.
constexpr const char* kTpuHoleSource =
    "TO CONFIRM WITH A COUPON. FDM holes print 0.1-0.4 mm undersize on the "
    "diameter and are modelled about 0.2 mm oversize (3dprintbounty.com, 3D "
    "printing tolerances guide); nothing published measures TPU 95A apart "
    "from that, so the FDM row is carried and not refined";
constexpr const char* kTpuShaftSource =
    "TO CONFIRM WITH A COUPON. An outside diameter grows by about a tenth "
    "in FDM (3dprintbounty.com, 3D printing tolerances guide); TPU 95A's "
    "own is not published";
constexpr const char* kTpuGapSource =
    "TO CONFIRM WITH A COUPON. Print-in-place in TPU 95A wants about 0.5 mm "
    "a side to slide: the material sags into a gap under its own weight as "
    "it prints and bonds to itself (blog.uavmodel.com, print-in-place "
    "clearance guides, 2026: 0.5 mm sliding, 0.3 for a hinge pin that "
    "deforms to turn; another table gives TPU 95A 0.5 mm and up). The "
    "guides disagree, and none measured a Bambu printer";
constexpr const char* kTpuPressSource =
    "TO CONFIRM WITH A COUPON. TPU stretches over what it is pressed on: a "
    "TPU sleeve fitted at 0, or 0.1 mm under on the radius, holds "
    "(forum.prusa3d.com, \"question about designing for TPU\": 0 to -0.1 mm "
    "a radius on a 95A); the bore is modelled at the pin's size";

std::vector<Table> BuildTables() {
  std::vector<Table> out;
  Table petg;
  petg.process = "fdm";
  petg.material = "petg";
  petg.conditions =
      "0.4 mm nozzle, slicer XY and hole compensation at their default of 0, "
      "and an STL export fine enough that its chords are not the error";
  petg.rows = {
      {Printed::Hole, 0.0, 10.0, 0.2, kHoleSource},
      {Printed::Hole, 10.0, kAbove, 0.3, kHoleSource},
      {Printed::Shaft, 0.0, kAbove, -0.1, kShaftSource},
      {Printed::InternalThread, 0.0, kAbove, 0.4, kInternalThreadSource},
      {Printed::ExternalThread, 0.0, kAbove, -0.1, kExternalThreadSource},
      {Printed::Gap, 0.0, kAbove, 0.6, kPetgGapSource},
      {Printed::Press, 0.0, kAbove, 0.0, kPressSource},
  };
  Table pla;
  pla.process = "fdm";
  pla.material = "pla";
  pla.conditions = petg.conditions;
  pla.rows = {
      {Printed::Hole, 0.0, kAbove, 0.2, kPlaHoleSource},
      {Printed::Shaft, 0.0, kAbove, -0.1, kPlaShaftSource},
      {Printed::InternalThread, 0.0, kAbove, 0.4, kInternalThreadSource},
      {Printed::ExternalThread, 0.0, kAbove, -0.1, kExternalThreadSource},
      {Printed::Gap, 0.0, kAbove, 0.5, kPlaGapSource},
      {Printed::Press, 0.0, kAbove, 0.0, kPressSource},
  };
  // No thread rows: a printed thread in a 95A flexes off its flank, and
  // nothing published says what allowance holds one; a TPU thread is
  // refused naming the table rather than read from PLA's.
  Table tpu;
  tpu.process = "fdm";
  tpu.material = "tpu";
  tpu.conditions =
      "TPU 95A (Bambu Lab TPU 95A HF's data sheet), 0.4 mm nozzle, slicer "
      "XY and hole compensation at 0; every row a published starting point "
      "to confirm with a coupon";
  tpu.rows = {
      {Printed::Hole, 0.0, kAbove, 0.2, kTpuHoleSource},
      {Printed::Shaft, 0.0, kAbove, -0.1, kTpuShaftSource},
      {Printed::Gap, 0.0, kAbove, 1.0, kTpuGapSource},
      {Printed::Press, 0.0, kAbove, 0.0, kTpuPressSource},
  };
  out.push_back(std::move(petg));
  out.push_back(std::move(pla));
  out.push_back(std::move(tpu));
  return out;
}

std::string Join(const std::vector<std::string>& v) {
  std::string s;
  for (const std::string& x : v)
    s += (s.empty() ? "" : ", ") + x;
  return s;
}

std::string Mm(double v) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.4g", v);
  return buf;
}

} // namespace

const std::vector<Printed>& AllPrinted() {
  static const std::vector<Printed> k = {
      Printed::Hole,           Printed::Shaft, Printed::InternalThread,
      Printed::ExternalThread, Printed::Gap,   Printed::Press};
  return k;
}

std::string PrintedName(Printed kind) {
  switch (kind) {
  case Printed::Hole:
    return "hole";
  case Printed::Shaft:
    return "shaft";
  case Printed::InternalThread:
    return "internal_thread";
  case Printed::ExternalThread:
    return "external_thread";
  case Printed::Gap:
    return "gap";
  case Printed::Press:
    return "press";
  }
  return "hole";
}

std::optional<Printed> PrintedFromName(const std::string& name) {
  for (Printed k : AllPrinted())
    if (PrintedName(k) == name)
      return k;
  return std::nullopt;
}

const std::string& PrintedNames() {
  static const std::string s = [] {
    std::string out;
    for (Printed k : AllPrinted())
      out += (out.empty() ? "" : ", ") + PrintedName(k);
    return out;
  }();
  return s;
}

const std::vector<Table>& Tables() {
  static const std::vector<Table> t = BuildTables();
  return t;
}

std::vector<std::string> ProcessNames() {
  std::vector<std::string> out;
  for (const Table& t : Tables()) {
    bool seen = false;
    for (const std::string& s : out)
      seen = seen || s == t.process;
    if (!seen)
      out.push_back(t.process);
  }
  return out;
}

std::vector<std::string> MaterialsFor(const std::string& process) {
  std::vector<std::string> out;
  for (const Table& t : Tables())
    if (t.process == process)
      out.push_back(t.material);
  return out;
}

namespace {

std::string ValidatePair(const std::string& process,
                         const std::string& material) {
  const std::vector<std::string> materials = MaterialsFor(process);
  if (materials.empty())
    return "unknown process '" + process +
           "'. Accepted: " + Join(ProcessNames());
  if (FindBuiltin(process, material) == nullptr)
    return "the " + process + " allowance has no data for '" + material +
           "'. Accepted for " + process + ": " + Join(materials) +
           ". A material is added with sourced numbers, or measured off a "
           "print_coupon — never borrowed from a neighbour";
  return {};
}

// Which way a kind's allowance may point: +1 opened (not negative), -1
// closed (not positive), 2 strictly opened (a gap of zero fuses), 0 either
// way (a press: whether the bore is modelled under its pin or over it is
// what the coupon measures, since the process already closes a bore).
int SignOf(Printed kind) {
  switch (kind) {
  case Printed::Hole:
  case Printed::InternalThread:
    return 1;
  case Printed::Shaft:
  case Printed::ExternalThread:
    return -1;
  case Printed::Gap:
    return 2;
  case Printed::Press:
    return 0;
  }
  return 1;
}

std::string RangeText(const Row& r) {
  return "Ø" + Mm(r.fromMm) +
         (r.toMm >= kAbove ? " and up" : "-Ø" + Mm(r.toMm));
}

} // namespace

std::string Validate(const Process& p) {
  if (std::string why = ValidatePair(p.process, p.material); !why.empty())
    return why;
  if (p.profile) {
    if (p.profile->table.process != p.process ||
        p.profile->table.material != p.material)
      return "profile '" + p.profile->name + "' is " +
             p.profile->table.process + " " + p.profile->table.material +
             ", not " + p.process + " " + p.material;
    if (std::string why = ValidateProfile(*p.profile); !why.empty())
      return why;
  }
  return {};
}

const Table* FindBuiltin(const std::string& process,
                         const std::string& material) {
  for (const Table& t : Tables())
    if (t.process == process && t.material == material)
      return &t;
  return nullptr;
}

const Table* Find(const Process& p) {
  if (p.profile)
    return &p.profile->table;
  return FindBuiltin(p.process, p.material);
}

Process FromProfile(const Profile& p) {
  return Process{p.table.process, p.table.material, p};
}

std::string Label(const Process& p) {
  const std::string pair = p.process + " " + p.material;
  return p.profile ? "'" + p.profile->name + "' (" + pair + ")" : pair;
}

std::string ValidateProfile(const Profile& p) {
  std::string name = p.name;
  name.erase(std::remove_if(name.begin(), name.end(),
                            [](unsigned char c) { return std::isspace(c); }),
             name.end());
  if (name.empty())
    return "a print profile needs a name";
  const std::string who = "print profile '" + p.name + "': ";
  if (std::string why = ValidatePair(p.table.process, p.table.material);
      !why.empty())
    return who + why;
  if (!std::isfinite(p.stlDeflectionMm) || p.stlDeflectionMm < 0.0)
    return who +
           "stl_deflection_mm must be 0 (not stated) or a positive "
           "chord deviation, not " +
           Mm(p.stlDeflectionMm);
  if (!std::isfinite(p.layerHeightMm) || p.layerHeightMm < 0.0)
    return who +
           "layer_height_mm must be 0 (not stated) or the slicer preset's "
           "positive layer height, not " +
           Mm(p.layerHeightMm);
  if (!std::isfinite(p.zGapMm) || p.zGapMm < 0.0)
    return who +
           "z_gap_mm (the vertical air a face needs over another part) "
           "must be 0 (not measured) or a positive number of whole layers "
           "in mm, not " +
           Mm(p.zGapMm);
  if (p.bedMm) {
    const char* axis[] = {"x", "y", "z"};
    for (int i = 0; i < 3; i++)
      if (!std::isfinite((*p.bedMm)[i]) || !((*p.bedMm)[i] > 0.0))
        return who +
               "bed_mm is the build volume [x, y, z] in mm, each "
               "positive; its " +
               axis[i] + " is " + Mm((*p.bedMm)[i]);
  }
  // v181: keep-out zones and the wall line width.
  for (size_t i = 0; i < p.bedExcludeMm.size(); i++) {
    const auto& z = p.bedExcludeMm[i];
    if (!std::isfinite(z[0]) || !std::isfinite(z[1]) || !std::isfinite(z[2]) ||
        !std::isfinite(z[3]) || !(z[2] > z[0]) || !(z[3] > z[1]))
      return who + "bed_exclude_mm[" + std::to_string(i) +
             "] is a rectangle [x0, y0, x1, y1] in mm with x1 > x0 and y1 > "
             "y0; got [" +
             Mm(z[0]) + ", " + Mm(z[1]) + ", " + Mm(z[2]) + ", " + Mm(z[3]) +
             "]";
  }
  if (!p.bedExcludeMm.empty() && !p.bedMm)
    return who + "bed_exclude_mm needs the bed_mm it keeps parts of clear";
  if (!std::isfinite(p.lineWidthMm) || p.lineWidthMm < 0.0)
    return who +
           "line_width_mm (the slicer's wall line width) must be 0 (not "
           "stated) or a positive number of mm, not " +
           Mm(p.lineWidthMm);
  // v185: the slender limit, and where it came from.
  if (!std::isfinite(p.slenderRatio) || p.slenderRatio < 0.0)
    return who +
           "slender_ratio (the tallest a piece stands over the least width "
           "it stands on) must be 0 (not stated) or a positive ratio, not " +
           Mm(p.slenderRatio);
  if (p.slenderRatio > 0.0 && p.slenderSource.empty())
    return who + "a slender_ratio of " + Mm(p.slenderRatio) +
           " needs its slender_ratio_source: the tower test or data sheet it "
           "was read from";
  if (p.slenderRatio <= 0.0 && !p.slenderSource.empty())
    return who + "slender_ratio_source without the slender_ratio it is the "
                 "source of";
  if (p.zGapMm > 0.0) {
    // A slicer ends air at a layer: air between layers is not air it can
    // print, so it is refused rather than rounded either way.
    if (!(p.layerHeightMm > 0.0))
      return who + "a z_gap_mm of " + Mm(p.zGapMm) +
             " needs the layer_height_mm it is whole layers of";
    const double layers = p.zGapMm / p.layerHeightMm;
    const double whole = std::round(layers);
    if (whole < 1.0 || std::abs(layers - whole) > 1e-6) {
      const double below = std::max(1.0, std::floor(layers));
      const auto count = [](double n) {
        return n == 1.0 ? std::string("1 layer") : Mm(n) + " layers";
      };
      return who + "z_gap_mm " + Mm(p.zGapMm) + " is not whole layers of " +
             Mm(p.layerHeightMm) +
             "; accepted: " + Mm(below * p.layerHeightMm) + " (" +
             count(below) + ") or " + Mm((below + 1.0) * p.layerHeightMm) +
             " (" + count(below + 1.0) + ")";
    }
  }
  if (p.table.rows.empty())
    return who + "a profile needs at least one row (" + PrintedNames() + ")";
  for (size_t i = 0; i < p.table.rows.size(); i++) {
    const Row& r = p.table.rows[i];
    const std::string at =
        who + PrintedName(r.kind) + " row " + std::to_string(i + 1) + ": ";
    if (!std::isfinite(r.fromMm) || !std::isfinite(r.toMm) || r.fromMm < 0.0 ||
        !(r.toMm > r.fromMm))
      return at +
             "the size range must run from 0 or more to something "
             "larger, not Ø" +
             Mm(r.fromMm) + " to Ø" + Mm(r.toMm);
    if (!std::isfinite(r.allowanceMm))
      return at + "the allowance is not a number";
    const int sign = SignOf(r.kind);
    if (sign == 2 && !(r.allowanceMm > 0.0))
      return at +
             "a gap's allowance must be positive (the whole diametral "
             "clearance; a gap of " +
             Mm(r.allowanceMm) + " fuses)";
    if (sign == 1 && r.allowanceMm < 0.0)
      return at + "a " + PrintedName(r.kind) +
             " is modelled larger, so its allowance is not negative (got " +
             Mm(r.allowanceMm) + ")";
    if (sign == -1 && r.allowanceMm > 0.0)
      return at + "a " + PrintedName(r.kind) +
             " is modelled smaller, so its allowance is not positive (got " +
             Mm(r.allowanceMm) + ")";
    // Rows of a kind run in ascending size and do not overlap.
    for (size_t j = 0; j < i; j++) {
      const Row& q = p.table.rows[j];
      if (q.kind != r.kind)
        continue;
      if (r.fromMm < q.toMm)
        return at + RangeText(r) +
               (r.fromMm < q.fromMm ? " is out of order after "
                                    : " overlaps ") +
               RangeText(q) +
               " — rows of a kind run in ascending size "
               "and do not overlap";
    }
  }
  return {};
}

const Profile* FindProfile(const std::vector<Profile>& store,
                           const std::string& name) {
  if (name.empty())
    return nullptr;
  for (const Profile& p : store)
    if (p.name == name)
      return &p;
  return nullptr;
}

ProfileMatch MatchProfile(const std::vector<Profile>& store, const Process& p) {
  if (!p.profile)
    return ProfileMatch::Builtin;
  const Profile* mine = FindProfile(store, p.profile->name);
  if (mine == nullptr)
    return ProfileMatch::Missing;
  return *mine == *p.profile ? ProfileMatch::Same : ProfileMatch::Differs;
}

const char* ProfileMatchName(ProfileMatch m) {
  switch (m) {
  case ProfileMatch::Builtin:
    return "builtin";
  case ProfileMatch::Same:
    return "same";
  case ProfileMatch::Differs:
    return "differs";
  case ProfileMatch::Missing:
    return "missing";
  }
  return "builtin";
}

std::string ProfileFileName(const std::string& name) {
  std::string slug;
  bool dash = false;
  for (const char c : name) {
    if (std::isalnum(static_cast<unsigned char>(c))) {
      slug += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      dash = false;
    } else if (!slug.empty() && !dash) {
      slug += '-';
      dash = true;
    }
  }
  while (!slug.empty() && slug.back() == '-')
    slug.pop_back();
  return (slug.empty() ? std::string("profile") : slug) + ".json";
}

std::optional<Profile> ProfileFromBuiltin(const std::string& name,
                                          const std::string& process,
                                          const std::string& material) {
  const Table* t = FindBuiltin(process, material);
  if (t == nullptr)
    return std::nullopt;
  return Profile{name, *t, BuiltinStlDeflectionMm(process, material)};
}

std::string Fingerprint(const Process& p) {
  std::string s = p.process + "/" + p.material;
  const Table* t = Find(p);
  if (p.profile)
    s += "/" + p.profile->name;
  if (t == nullptr)
    return s;
  char buf[96];
  for (const Row& r : t->rows) {
    std::snprintf(buf, sizeof(buf), "|%d:%.17g:%.17g:%.17g",
                  static_cast<int>(r.kind), r.fromMm, r.toMm, r.allowanceMm);
    s += buf;
  }
  return s;
}

// TPU's gap is wide (0.5 a side), and a tenth of it, 0.05, is a quarter of
// the 0.2 its holes are opened by: its chords are held to a tenth of that.
double BuiltinStlDeflectionMm(const std::string& process,
                              const std::string& material) {
  return process == "fdm" && material == "tpu" ? 0.02 : 0.0;
}

double SmallestGapSideMm(const Table& t) {
  double gap = 0.0, any = 0.0;
  for (const Row& r : t.rows) {
    const double a = std::abs(r.allowanceMm);
    if (!(a > 0.0))
      continue;
    if (r.kind == Printed::Press)
      continue; // an interference opens nothing
    if (r.kind == Printed::Gap)
      gap = gap > 0.0 ? std::min(gap, a) : a;
    any = any > 0.0 ? std::min(any, a) : a;
  }
  return (gap > 0.0 ? gap : any) / 2.0;
}

double StlDeflectionMm(const Process& p) {
  if (p.profile && p.profile->stlDeflectionMm > 0.0)
    return p.profile->stlDeflectionMm;
  if (const double own = BuiltinStlDeflectionMm(p.process, p.material);
      own > 0.0)
    return own;
  const Table* t = Find(p);
  return t == nullptr ? 0.0 : SmallestGapSideMm(*t) / 10.0;
}

std::optional<PressBand> PrintedPress(const Process& p, bool bore,
                                      double modelledMm, std::string* why) {
  if (p.process != "fdm") {
    if (why)
      *why = "a printed press is the fit an FDM printer's bore and pin need, "
             "and this part is printed by " +
             p.process;
    return std::nullopt;
  }
  const Table* t = Find(p);
  if (t == nullptr) {
    if (why)
      *why = Validate(p);
    return std::nullopt;
  }
  // The press row: the profile's own, else its pair's built-in default
  // (Allowance::builtinFallback).
  const bool own =
      std::any_of(t->rows.begin(), t->rows.end(),
                  [](const Row& r) { return r.kind == Printed::Press; });
  const Table* rows = own ? t : FindBuiltin(p.process, p.material);
  if (rows == nullptr) {
    if (why)
      *why = "the " + Label(p) + " table has no press row";
    return std::nullopt;
  }
  const auto press =
      bore ? ForModelled(*rows, Label(p), Printed::Press, modelledMm, why)
           : ForBasic(*rows, Label(p), Printed::Press, modelledMm, why);
  if (!press)
    return std::nullopt;
  const auto gap = ForBasic(p, Printed::Gap, press->basicMm, why);
  if (!gap)
    return std::nullopt;
  PressBand b;
  b.basicMm = press->basicMm;
  b.pressMm = press->allowanceMm;
  b.toleranceMm = kPressToleranceShare * gap->allowanceMm;
  b.builtinFallback = !own && p.profile.has_value();
  const double centre = bore ? b.pressMm : 0.0;
  b.lowerMm = centre - b.toleranceMm;
  b.upperMm = centre + b.toleranceMm;
  return b;
}

std::optional<Allowance> ForBasic(const Process& p, Printed kind,
                                  double basicMm, std::string* why) {
  const Table* t = Find(p);
  if (t == nullptr) {
    if (why)
      *why = Validate(p);
    return std::nullopt;
  }
  // A profile with no press row reads its pair's built-in one, and says so
  // (Allowance::builtinFallback).
  if (kind == Printed::Press && p.profile &&
      std::none_of(t->rows.begin(), t->rows.end(),
                   [](const Row& r) { return r.kind == Printed::Press; }))
    if (const Table* builtin = FindBuiltin(p.process, p.material)) {
      auto a = ForBasic(*builtin, Label(p), kind, basicMm, why);
      if (a)
        a->builtinFallback = true;
      return a;
    }
  return ForBasic(*t, Label(p), kind, basicMm, why);
}

std::optional<Allowance> ForBasic(const Table& t, const std::string& label,
                                  Printed kind, double basicMm,
                                  std::string* why) {
  if (!(basicMm > 0.0)) {
    if (why)
      *why = "a printed diameter must be positive (got " + Mm(basicMm) + ")";
    return std::nullopt;
  }
  for (const Row& r : t.rows)
    if (r.kind == kind && basicMm >= r.fromMm && basicMm < r.toMm)
      return Allowance{kind, basicMm, r.allowanceMm, basicMm + r.allowanceMm,
                       &r};
  if (why)
    *why = "the " + label + " table has no " + PrintedName(kind) +
           " row for Ø" + Mm(basicMm);
  return std::nullopt;
}

std::optional<Allowance> ForModelled(const Process& p, Printed kind,
                                     double modelledMm, std::string* why) {
  const Table* t = Find(p);
  if (t == nullptr) {
    if (why)
      *why = Validate(p);
    return std::nullopt;
  }
  return ForModelled(*t, Label(p), kind, modelledMm, why);
}

std::optional<Allowance> ForModelled(const Table& t, const std::string& label,
                                     Printed kind, double modelledMm,
                                     std::string* why) {
  // Rows are in ascending size, so the first whose basic range holds the
  // inverse is the answer; failing that, the gap between two rows' model
  // ranges prints at the step between them.
  const Row* prev = nullptr;
  for (const Row& r : t.rows) {
    if (r.kind != kind)
      continue;
    const double basic = modelledMm - r.allowanceMm;
    if (basic >= r.fromMm && basic < r.toMm && basic > 0.0)
      return Allowance{kind, basic, r.allowanceMm, modelledMm, &r};
    if (prev != nullptr && basic < r.fromMm &&
        modelledMm - prev->allowanceMm >= prev->toMm)
      return Allowance{kind, r.fromMm, modelledMm - r.fromMm, modelledMm, &r};
    prev = &r;
  }
  if (why)
    *why = "a " + PrintedName(kind) + " modelled at Ø" + Mm(modelledMm) +
           " is outside the " + label + " table";
  return std::nullopt;
}

double ProcessRepeatabilityMm(const Process& p) {
  return p.process == "fdm" ? 0.2 : 0.0;
}

std::string ProcessRepeatabilitySource(const Process& p) {
  return p.process == "fdm" ? "desktop FDM holds about ±0.2 mm in practice "
                              "(3dprintbounty.com, 3D printing tolerances "
                              "guide)"
                            : std::string{};
}

double MaxOverhangDeg(const Process& p) {
  return p.process == "fdm" ? 45.0 : 0.0;
}

std::string MaxOverhangSource(const Process& p) {
  if (p.process == "fdm" && p.material == "tpu")
    return "the 45-degree rule of FDM design guides, kept for TPU: Bambu "
           "Lab's TPU 95A HF data sheet gives about 55 degrees as the most "
           "its profile prints, and a flexible layer sags first";
  return p.process == "fdm"
             ? "the 45-degree rule of FDM design guides: a layer stands on "
               "the one below while it overhangs it by no more than about "
               "half its width"
             : std::string{};
}

double MaxBridgeMm(const Process& p) {
  if (p.process != "fdm")
    return 0.0;
  return p.material == "tpu" ? 5.0 : 10.0;
}

std::string MaxBridgeSource(const Process& p) {
  if (p.process != "fdm")
    return {};
  if (p.material == "tpu")
    return "TO CONFIRM WITH A COUPON: TPU sags and strings across a gap -- "
           "Bambu Lab lists bridging among TPU 95A HF's weaknesses "
           "(bambulab.com, TPU 95A HF) -- so half the 10 mm FDM rule. Its "
           "data sheet's 20 mm (Technical Data Sheet V1.0) is the longest "
           "its tuned profile spans, not a clean ceiling";
  return "community practice: a desktop FDM printer bridges about "
         "10 mm cleanly without tuning, and longer ones sag; to "
         "confirm on your printer";
}

double MinAirMm(const Process& p) {
  if (p.process != "fdm")
    return 0.0;
  if (p.profile && p.profile->zGapMm > 0.0)
    return p.profile->zGapMm;
  // Whole layers, read as they are written: 3 x 0.2 is 0.6, not
  // 0.6000000000000001.
  const auto layers = [](double layer) {
    return std::round(kMinAirLayers * layer * 1e9) / 1e9;
  };
  if (p.profile && p.profile->layerHeightMm > 0.0)
    return layers(p.profile->layerHeightMm);
  return layers(kFdmLayerMm);
}

double SlenderRatio(const Process& p) {
  if (p.profile && p.profile->slenderRatio > 0.0)
    return p.profile->slenderRatio;
  return p.process == "fdm" ? printcheck::kSlenderRatio : 0.0;
}

std::string SlenderFrom(const Process& p) {
  if (p.profile && p.profile->slenderRatio > 0.0)
    return "profile";
  return p.process == "fdm" ? "default" : std::string();
}

std::string SlenderSource(const Process& p) {
  const std::string from = SlenderFrom(p);
  if (from == "profile")
    return "the profile's slender_ratio, " + Mm(p.profile->slenderRatio) +
           ": " + p.profile->slenderSource;
  if (from == "default")
    return "the built-in " + Mm(printcheck::kSlenderRatio) +
           ":1, NOT MEASURED: FDM guides put a free-standing pin's safe "
           "height at a few to ten times its width, and the clock's "
           "pillars, 24.5:1, came off the bed. State slender_ratio (with "
           "its slender_ratio_source) in the print profile once a tower "
           "test says";
  return "the process '" + p.process +
         "' is not FDM: no slender limit is known for it";
}

std::string MinAirFrom(const Process& p) {
  if (p.process != "fdm")
    return {};
  if (p.profile && p.profile->zGapMm > 0.0)
    return "profile";
  if (p.profile && p.profile->layerHeightMm > 0.0)
    return "layers";
  return "process";
}

std::string MinAirSource(const Process& p) {
  const std::string from = MinAirFrom(p);
  const std::string confirm =
      ": to confirm with the coupon's air row (generators/print_coupon.py)";
  if (from == "profile")
    return "the profile's z_gap_mm, " + Mm(p.profile->zGapMm) +
           " mm: the air the coupon measured" +
           (p.profile->layerHeightMm > 0.0
                ? ", " + Mm(p.profile->zGapMm / p.profile->layerHeightMm) +
                      " layers of " + Mm(p.profile->layerHeightMm) + " mm"
                : std::string());
  if (from == "layers")
    return std::to_string(kMinAirLayers) + " layers of the profile's " +
           Mm(p.profile->layerHeightMm) + " mm layer height" + confirm;
  if (from == "process")
    return std::to_string(kMinAirLayers) + " layers of FDM's common " +
           Mm(kFdmLayerMm) + " mm layer height" + confirm;
  return "the process '" + p.process +
         "' is not FDM: no minimum air between stacked parts is known for it";
}

double MinReliableThreadMm(const Process& p) {
  return p.process == "fdm" ? 8.0 : 0.0;
}

const std::string& ConfirmNote() {
  static const std::string s =
      "to confirm with a test coupon: the built-in tables are community "
      "practice and CAD tool defaults, not a measurement of your printer — "
      "print generators/print_coupon.py, exported at the deflection you "
      "export parts at, and keep what fits in a print profile "
      "(Preferences › Print profiles, or print_coupon.py --record)";
  return s;
}

std::string ConfirmNote(const std::string& rowSource) {
  if (rowSource.empty())
    return ConfirmNote();
  for (const Table& t : Tables())
    for (const Row& r : t.rows)
      if (r.source == rowSource)
        return ConfirmNote();
  return "measured: " + rowSource +
         " -- the print profile's own row. Print the coupon again "
         "(generators/print_coupon.py) when the printer, the filament or "
         "the slicer preset changes";
}

} // namespace printfit
