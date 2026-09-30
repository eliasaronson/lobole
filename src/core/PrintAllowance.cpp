#include "core/PrintAllowance.h"

#include <cstdio>

namespace printfit {
namespace {

constexpr double kAbove = 1e9; // "and above": the last row's open end

// The sources, once. docs/plans/pan-tilt-survey.md section 5 read them; the
// numbers are that section's proposed table.
constexpr const char* kHoleSource =
    "FDM holes print 0.1-0.4 mm undersize on the diameter and the fix is to "
    "model them about 0.2 mm oversize (3dprintbounty.com, 3D printing "
    "tolerances guide); PETG holes read 0.2-0.3 mm under on Ø10, and PETG "
    "shrinks 0.5-0.8 % against PLA's 0.3-0.5 % (pan-tilt survey, section 5)";
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
  };
  out.push_back(std::move(petg));
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
  }
  return "hole";
}

std::optional<Printed> PrintedFromName(const std::string& name) {
  for (Printed k : {Printed::Hole, Printed::Shaft, Printed::InternalThread,
                    Printed::ExternalThread})
    if (PrintedName(k) == name)
      return k;
  return std::nullopt;
}

const std::string& PrintedNames() {
  static const std::string s = "hole, shaft, internal_thread, external_thread";
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

std::string Validate(const Process& p) {
  const std::vector<std::string> materials = MaterialsFor(p.process);
  if (materials.empty())
    return "unknown process '" + p.process +
           "'. Accepted: " + Join(ProcessNames());
  if (Find(p) == nullptr)
    return "the " + p.process + " allowance has no data for '" + p.material +
           "'. Accepted for " + p.process + ": " + Join(materials) +
           ". A material is added with sourced numbers, or measured off a "
           "print_coupon — never borrowed from a neighbour";
  return {};
}

const Table* Find(const Process& p) {
  for (const Table& t : Tables())
    if (t.process == p.process && t.material == p.material)
      return &t;
  return nullptr;
}

std::optional<Allowance> ForBasic(const Process& p, Printed kind,
                                  double basicMm, std::string* why) {
  const Table* t = Find(p);
  if (t == nullptr) {
    if (why)
      *why = Validate(p);
    return std::nullopt;
  }
  if (!(basicMm > 0.0)) {
    if (why)
      *why = "a printed diameter must be positive (got " + Mm(basicMm) + ")";
    return std::nullopt;
  }
  for (const Row& r : t->rows)
    if (r.kind == kind && basicMm >= r.fromMm && basicMm < r.toMm)
      return Allowance{kind, basicMm, r.allowanceMm, basicMm + r.allowanceMm,
                       &r};
  if (why)
    *why = "the " + p.process + " " + p.material + " table has no " +
           PrintedName(kind) + " row for Ø" + Mm(basicMm);
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
  // Rows are in ascending size, so the first whose basic range holds the
  // inverse is the answer; failing that, the gap between two rows' model
  // ranges prints at the step between them.
  const Row* prev = nullptr;
  for (const Row& r : t->rows) {
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
           " is outside the " + p.process + " " + p.material + " table";
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

double MinReliableThreadMm(const Process& p) {
  return p.process == "fdm" ? 8.0 : 0.0;
}

const std::string& ConfirmNote() {
  static const std::string s =
      "to confirm with a test coupon: the table is community practice and CAD "
      "tool defaults, not a measurement of your printer — print "
      "generators/print_coupon.py, exported at the deflection you export "
      "parts at, and correct the table from what fits";
  return s;
}

} // namespace printfit
