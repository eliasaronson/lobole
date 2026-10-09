// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <optional>
#include <string>
#include <vector>

// The process allowance a PRINTED part needs
// (docs/tolerances/README.md#printed-parts): an FDM printer makes a hole
// smaller than its model and a shaft larger, by amounts that depend on the
// process and the material. A fit or a thread on a printed part is therefore
// MODELLED at its size plus an allowance, and this is the table that says by
// how much.
//
// Like a tolerance it drives NO geometry. It is what check_fit, check_fits,
// print_allowance and the Thread editor REPORT — "H7 on a printed Ø8 bore:
// model Ø8.2" — and a generator applies it where it models the part.
//
// Pure static data, no I/O, every number MILLIMETRES ON THE DIAMETER and
// signed so that model = basic + allowance: a hole's is positive (model it
// larger), a shaft's negative (model it smaller). Each row carries its
// source, and every one of them is to be confirmed with a test coupon
// (generators/print_coupon.py): the sources are community practice and CAD
// tool defaults, not a measurement of this printer.
namespace printfit {

// How a part is made, as a document declares it: the process and the
// material it is made of in that process. Only the pairs the table carries
// are accepted (Validate); anything else is refused naming what is.
struct Process {
  std::string process;  // "fdm"
  std::string material; // "petg" | "pla"
  bool operator==(const Process&) const = default;
};

// What a diameter is, as far as a printer is concerned: which way the
// process moves it. A Gap is the round clearance between two surfaces
// PRINTED TOGETHER that must come apart (a print-in-place joint): its basic
// is the inner surface's diameter, its model the outer's, and the allowance
// is the whole diametral gap — what a print needs so the two do not fuse.
enum class Printed { Hole, Shaft, InternalThread, ExternalThread, Gap };
[[nodiscard]] std::string PrintedName(Printed kind);
// "hole" | "shaft" | "internal_thread" | "external_thread" | "gap"; nullopt
// otherwise
// (the caller refuses naming PrintedNames()).
[[nodiscard]] std::optional<Printed> PrintedFromName(const std::string& name);
[[nodiscard]] const std::string& PrintedNames();

// One row: the allowance for one kind of diameter over a size range
// [fromMm, toMm) of the BASIC (as-printed) size.
struct Row {
  Printed kind = Printed::Hole;
  double fromMm = 0.0;
  double toMm = 0.0;        // exclusive; a large number for "and above"
  double allowanceMm = 0.0; // signed, on the diameter: model = basic + this
  std::string source;
};

struct Table {
  std::string process;
  std::string material;
  std::string conditions; // what the numbers assume (nozzle, export)
  std::vector<Row> rows;
};

// Every process/material pair carried, in display order.
[[nodiscard]] const std::vector<Table>& Tables();
// "fdm" ...
[[nodiscard]] std::vector<std::string> ProcessNames();
// The materials carried for `process` (empty for an unknown process).
[[nodiscard]] std::vector<std::string> MaterialsFor(const std::string& process);

// Empty when `p` is a carried pair; otherwise the refusal, naming the
// processes (for an unknown process) or that process's materials (for a
// material it has no data for).
[[nodiscard]] std::string Validate(const Process& p);
// The table for `p`, or nullptr (Validate says why).
[[nodiscard]] const Table* Find(const Process& p);

// The answer for one diameter.
struct Allowance {
  Printed kind = Printed::Hole;
  double basicMm = 0.0;     // the size the print should come out at
  double allowanceMm = 0.0; // signed, on the diameter
  double modelMm = 0.0;     // basic + allowance: what to MODEL
  const Row* row = nullptr;
};

// What to model for a printed diameter of `basicMm`. Refused (nullopt, `why`
// set) for a process/material not carried or a size outside the rows.
[[nodiscard]] std::optional<Allowance>
ForBasic(const Process& p, Printed kind, double basicMm, std::string* why);

// The inverse: a diameter MODELLED at `modelledMm` prints at what? The row is
// the one whose model range holds `modelledMm`. The table steps at a size, so
// the model ranges of two neighbouring rows can leave a gap (a hole row of
// +0.2 below Ø10 and +0.3 from Ø10 leaves Ø10.2..Ø10.3 unclaimed); a
// modelled size in the gap prints at the step itself.
[[nodiscard]] std::optional<Allowance> ForModelled(const Process& p,
                                                   Printed kind,
                                                   double modelledMm,
                                                   std::string* why);

// A desktop FDM printer holds about ±0.2 mm in practice; an ISO band
// narrower than that is a statement the print will not meet on its own.
// Returned with its source, for the note the checks attach.
[[nodiscard]] double ProcessRepeatabilityMm(const Process& p);
[[nodiscard]] std::string ProcessRepeatabilitySource(const Process& p);

// Below this major diameter a printed thread is not reliable in this
// process (the community's reading: printed threads work from M8 up); the
// thread report says so and names the alternatives. 0 = no floor known.
[[nodiscard]] double MinReliableThreadMm(const Process& p);

// A hole exported as chords loses the sagitta: every chord of an STL
// tessellated at chordal deviation `deflectionMm` may sit that far inside
// the surface, so a hole's diameter across two chords can come out up to
// twice the deflection small (and a shaft's the same amount large). The
// table's numbers were measured through somebody's export; the coupon,
// exported at the same deflection as the parts, calibrates both at once.
[[nodiscard]] inline double ChordLossMm(double deflectionMm) {
  return 2.0 * deflectionMm;
}

// The sentence every report ends with.
[[nodiscard]] const std::string& ConfirmNote();

} // namespace printfit
