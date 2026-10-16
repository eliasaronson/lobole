// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/PrintAllowance.h"

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
  };
  out.push_back(std::move(petg));
  out.push_back(std::move(pla));
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
  }
  return "hole";
}

std::optional<Printed> PrintedFromName(const std::string& name) {
  for (Printed k : {Printed::Hole, Printed::Shaft, Printed::InternalThread,
                    Printed::ExternalThread, Printed::Gap})
    if (PrintedName(k) == name)
      return k;
  return std::nullopt;
}

const std::string& PrintedNames() {
  static const std::string s =
      "hole, shaft, internal_thread, external_thread, gap";
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
// closed (not positive), 2 strictly opened (a gap of zero fuses).
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
  return Profile{name, *t, 0.0};
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

double SmallestGapSideMm(const Table& t) {
  double gap = 0.0, any = 0.0;
  for (const Row& r : t.rows) {
    const double a = std::abs(r.allowanceMm);
    if (!(a > 0.0))
      continue;
    if (r.kind == Printed::Gap)
      gap = gap > 0.0 ? std::min(gap, a) : a;
    any = any > 0.0 ? std::min(any, a) : a;
  }
  return (gap > 0.0 ? gap : any) / 2.0;
}

double StlDeflectionMm(const Process& p) {
  if (p.profile && p.profile->stlDeflectionMm > 0.0)
    return p.profile->stlDeflectionMm;
  const Table* t = Find(p);
  return t == nullptr ? 0.0 : SmallestGapSideMm(*t) / 10.0;
}

std::optional<Allowance> ForBasic(const Process& p, Printed kind,
                                  double basicMm, std::string* why) {
  const Table* t = Find(p);
  if (t == nullptr) {
    if (why)
      *why = Validate(p);
    return std::nullopt;
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
  return p.process == "fdm"
             ? "the 45-degree rule of FDM design guides: a layer stands on "
               "the one below while it overhangs it by no more than about "
               "half its width"
             : std::string{};
}

double MaxBridgeMm(const Process& p) { return p.process == "fdm" ? 10.0 : 0.0; }

std::string MaxBridgeSource(const Process& p) {
  return p.process == "fdm"
             ? "community practice: a desktop FDM printer bridges about "
               "10 mm cleanly without tuning, and longer ones sag; to "
               "confirm on your printer"
             : std::string{};
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

} // namespace printfit
