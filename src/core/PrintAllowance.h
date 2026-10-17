// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <array>
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
// No I/O, every number MILLIMETRES ON THE DIAMETER and signed so that
// model = basic + allowance: a hole's is positive (model it larger), a
// shaft's negative (model it smaller). Each row carries its source. The
// BUILT-IN tables are community practice and CAD tool defaults, the
// defaults a PRINT PROFILE starts from; a profile is the user's own copy,
// corrected from a test coupon (generators/print_coupon.py) for one printer,
// filament and slicer preset (docs/tolerances/README.md#print-profiles).
namespace printfit {

// What a diameter is, as far as a printer is concerned: which way the
// process moves it. A Gap is the round clearance between two surfaces
// PRINTED TOGETHER that must come apart (a print-in-place joint): its basic
// is the inner surface's diameter, its model the outer's, and the allowance
// is the whole diametral gap — what a print needs so the two do not fuse.
//
// A Press is the other way round (docs/tolerances/README.md#press-allowance):
// two parts printed APART and pressed together to stay -- a printed cap on a
// printed pin. Its basic is the pin's MODELLED diameter, its model the
// cap's bore, and the allowance the bore less the pin on the diameter:
// negative is a bore modelled under its pin. Either sign is a press, since
// FDM already prints a bore small and a pin large; the coupon's press row
// says where this printer holds without splitting. Quoted a side as the
// INTERFERENCE, -allowance / 2, as a gap is quoted a side.
enum class Printed { Hole, Shaft, InternalThread, ExternalThread, Gap, Press };
// Every kind, in display order: the one list the parser, the Settings
// window's kind column and the names below are drawn from.
[[nodiscard]] const std::vector<Printed>& AllPrinted();
[[nodiscard]] std::string PrintedName(Printed kind);
// "hole" | "shaft" | "internal_thread" | "external_thread" | "gap" |
// "press"; nullopt otherwise (the caller refuses naming PrintedNames()).
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
  bool operator==(const Row&) const = default;
};

// "And above": the open end of a row's size range.
inline constexpr double kAboveMm = 1e9;

struct Table {
  std::string process;
  std::string material;
  std::string conditions; // what the numbers assume (nozzle, export)
  std::vector<Row> rows;
  bool operator==(const Table&) const = default;
};

// A PRINT PROFILE (docs/tolerances/README.md#print-profiles): a named,
// user-owned copy of a table -- one printer, one filament, one slicer
// preset. Stored one file per profile in ~/.config/lobole/print_profiles
// (DocumentIO::LoadPrintProfiles), and EMBEDDED in a part that is printed
// with it, the way a CAM setup embeds its machine.
struct Profile {
  std::string name;
  Table table; // its base process and material, conditions and rows
  // The STL chord deviation the profile was calibrated at, mm; 0 = not
  // stated, and an export takes a tenth of the smallest gap a side.
  double stlDeflectionMm = 0.0;
  // The slicer preset's layer height, mm; 0 = not stated.
  double layerHeightMm = 0.0;
  // The vertical AIR a face needs over another part to come away from it,
  // mm, whole layers of layerHeightMm (the slicer ends air at a layer); 0 =
  // not measured (print_coupon's air row,
  // docs/tolerances/README.md#print-profiles). Not a gap a part may START
  // in: a part printed one empty layer over another bonds to it, and one
  // with more under it is printed into the air -- every piece starts on the
  // bed. The sideways counterpart is the `gap` rows. Drives no geometry:
  // check_print's stacking item judges by it (MinAirMm), and a
  // print-in-place generator reads it.
  double zGapMm = 0.0;
  // v179: the printer's build volume, mm along its bed's x and y and up
  // its z: what check_print's bed_fit item fits a plate into. Unset = not
  // stated, and bed_fit skips saying so: nothing guesses a printer.
  std::optional<std::array<double, 3>> bedMm;
  // v181: the bed's keep-out rectangles, [x0, y0, x1, y1] mm in the bed's
  // own coordinates (origin at its front-left corner, as the slicer's
  // printable area): a P1S keeps 18 x 28 mm at the front left clear for its
  // nozzle wiper. bed_fit fails a piece whose footprint enters one, and the
  // positions a plate's pieces have are where the job puts them.
  std::vector<std::array<double, 4>> bedExcludeMm;
  // v181: the slicer preset's wall line width, mm; 0 = not stated. A wall
  // loop's share of the shell the printed mass weighs solid
  // (core/PrintedMass.h).
  double lineWidthMm = 0.0;
  bool operator==(const Profile&) const = default;
};

// How a part is made, as a document declares it: the process and the
// material it is made of in that process, and, when it is printed with one
// of the user's profiles, an embedded COPY of that profile (v169). The copy
// is what every lookup reads: a part keeps what it was modelled with,
// whichever machine opens it. Without one, the built-in table of the pair.
// Only the pairs the built-in tables carry are accepted (Validate).
struct Process {
  std::string process;  // "fdm"
  std::string material; // "petg" | "pla" | "tpu"
  std::optional<Profile> profile;
  bool operator==(const Process&) const = default;
};
// A part printed with `p`: its pair and the embedded copy.
[[nodiscard]] Process FromProfile(const Profile& p);
// "fdm pla", or "'K5 PLA' (fdm pla)" with a profile: how a report names it.
[[nodiscard]] std::string Label(const Process& p);

// Every BUILT-IN process/material pair carried, in display order: the
// defaults, read-only, that a profile starts from.
[[nodiscard]] const std::vector<Table>& Tables();
// "fdm" ...
[[nodiscard]] std::vector<std::string> ProcessNames();
// The materials carried for `process` (empty for an unknown process).
[[nodiscard]] std::vector<std::string> MaterialsFor(const std::string& process);

// Empty when `p` is a carried pair (and its profile, when it has one, is
// one ValidateProfile accepts); otherwise the refusal, naming the processes
// (for an unknown process) or that process's materials (for a material it
// has no data for).
[[nodiscard]] std::string Validate(const Process& p);
// The table `p` reads: its embedded profile's, else the built-in one of its
// pair; nullptr when neither (Validate says why).
[[nodiscard]] const Table* Find(const Process& p);
// The built-in table of a pair, or nullptr.
[[nodiscard]] const Table* FindBuiltin(const std::string& process,
                                       const std::string& material);

// Empty when `p` may be stored; otherwise what is wrong with it: no name, a
// base process/material the built-in tables do not carry, a negative or
// non-finite chord deviation or layer height, a bed whose sides are not all
// finite and positive, a Z gap without a layer
// height or not whole layers of it, no rows, a size range that is empty,
// overlapping or out of order within its kind, or an allowance of the wrong
// sign for its kind (a hole, internal thread or gap is opened, so not
// negative -- and a gap of zero fuses; a shaft or external thread is not
// positive; a press may be either).
[[nodiscard]] std::string ValidateProfile(const Profile& p);

// How a part's embedded profile stands against the user's store, by name.
enum class ProfileMatch {
  Builtin, // no profile: the built-in table, nothing to compare
  Same,    // named, and equal to the store's profile of that name
  Differs, // named, and the store's says something else
  Missing, // named, and the store has no profile of that name
};
[[nodiscard]] const Profile* FindProfile(const std::vector<Profile>& store,
                                         const std::string& name);
[[nodiscard]] ProfileMatch MatchProfile(const std::vector<Profile>& store,
                                        const Process& p);
// "builtin" | "same" | "differs" | "missing"
[[nodiscard]] const char* ProfileMatchName(ProfileMatch m);
// The file a profile of this name is stored in: the name lower-cased with
// every run of non-alphanumerics folded to one '-', plus ".json".
[[nodiscard]] std::string ProfileFileName(const std::string& name);
// A profile that starts as the built-in table of a pair ("the defaults,
// corrected"); nullopt for a pair the tables do not carry.
[[nodiscard]] std::optional<Profile>
ProfileFromBuiltin(const std::string& name, const std::string& process,
                   const std::string& material);

// Everything about `p` that moves geometry: the pair, the profile's name and
// every row's kind, range and allowance. The regeneration and requirements
// hashes fold this in, so editing a gap rebuilds what was modelled from it.
[[nodiscard]] std::string Fingerprint(const Process& p);

// The smallest clearance a side the table opens -- half its smallest gap
// row, or, without one, half its smallest non-zero allowance that is not a
// press (an interference opens nothing); 0 for none.
[[nodiscard]] double SmallestGapSideMm(const Table& t);
// The chord deviation a printed part's STL is exported at: the profile's
// stated one, else a tenth of the smallest gap a side (0 when the table has
// nothing to keep).
[[nodiscard]] double StlDeflectionMm(const Process& p);
// A built-in material's own chord deviation, where a tenth of its gap a side
// would let the chords eat its holes (TPU's); 0 = none of its own.
[[nodiscard]] double BuiltinStlDeflectionMm(const std::string& process,
                                            const std::string& material);

// A PRINTED PRESS's size band (docs/tolerances/README.md#printed-press-fits):
// the band a printed cap's bore and the printed pin it is pressed on carry,
// so the contact checks relieve the press as they relieve an ISO press fit.
// The bore's is the profile's press allowance on the diameter, the pin's
// zero, each give or take kPressToleranceShare of the profile's gap at that
// size -- the share check_print's clearance item allows a gap. Deviations
// on the diameter from `basicMm`, the PIN's modelled diameter: both faces
// are modelled at what the press needs, so neither carries a process
// allowance on top.
inline constexpr double kPressToleranceShare = 0.1;
struct PressBand {
  double basicMm = 0.0;     // the pin's modelled diameter
  double pressMm = 0.0;     // the press allowance (bore - pin), signed
  double toleranceMm = 0.0; // either way, on the diameter
  double lowerMm = 0.0, upperMm = 0.0;
  bool builtinFallback = false; // the press row is the built-in default
};
// The band of the face modelled at `modelledMm`, a bore when `bore`.
// Refused (nullopt, `why` set) for a process that is not FDM -- a printed
// press is the allowance an FDM printer's bore and pin need -- or a size
// the profile's press or gap rows do not cover.
[[nodiscard]] std::optional<PressBand>
PrintedPress(const Process& p, bool bore, double modelledMm, std::string* why);

// The answer for one diameter.
struct Allowance {
  Printed kind = Printed::Hole;
  double basicMm = 0.0;     // the size the print should come out at
  double allowanceMm = 0.0; // signed, on the diameter
  double modelMm = 0.0;     // basic + allowance: what to MODEL
  const Row* row = nullptr;
  // A press asked of a profile with no press row of its own: a profile
  // saved before the kind existed, or one whose coupon has not been pressed
  // yet. It reads the BUILT-IN press row of its pair -- a default to confirm
  // with the coupon's press row, said so in `row->source` -- rather than
  // refusing every press on every profile there is (the way an unmeasured
  // z_gap_mm falls back to whole layers, MinAirFrom). No other kind falls
  // back: a profile states those from the start.
  bool builtinFallback = false;
};

// What to model for a printed diameter of `basicMm`. Refused (nullopt, `why`
// set) for a process/material not carried or a size outside the rows.
[[nodiscard]] std::optional<Allowance>
ForBasic(const Process& p, Printed kind, double basicMm, std::string* why);
// The same on a resolved table (`label` names it in a refusal).
[[nodiscard]] std::optional<Allowance> ForBasic(const Table& t,
                                                const std::string& label,
                                                Printed kind, double basicMm,
                                                std::string* why);

// The inverse: a diameter MODELLED at `modelledMm` prints at what? The row is
// the one whose model range holds `modelledMm`. The table steps at a size, so
// the model ranges of two neighbouring rows can leave a gap (a hole row of
// +0.2 below Ø10 and +0.3 from Ø10 leaves Ø10.2..Ø10.3 unclaimed); a
// modelled size in the gap prints at the step itself.
[[nodiscard]] std::optional<Allowance> ForModelled(const Process& p,
                                                   Printed kind,
                                                   double modelledMm,
                                                   std::string* why);
[[nodiscard]] std::optional<Allowance>
ForModelled(const Table& t, const std::string& label, Printed kind,
            double modelledMm, std::string* why);

// A desktop FDM printer holds about ±0.2 mm in practice; an ISO band
// narrower than that is a statement the print will not meet on its own.
// Returned with its source, for the note the checks attach.
[[nodiscard]] double ProcessRepeatabilityMm(const Process& p);
[[nodiscard]] std::string ProcessRepeatabilitySource(const Process& p);

// What the process prints without support
// (docs/tolerances/README.md#checking-a-print), each with its source: the
// steepest downward face, in degrees FROM VERTICAL (0 a wall, 90 a ceiling),
// and the widest ceiling it bridges between two anchored edges, in mm. 0 = not
// known for the process. check_print's defaults; a caller may state its own.
[[nodiscard]] double MaxOverhangDeg(const Process& p);
[[nodiscard]] std::string MaxOverhangSource(const Process& p);
[[nodiscard]] double MaxBridgeMm(const Process& p);
[[nodiscard]] std::string MaxBridgeSource(const Process& p);

// The vertical air a face needs over ANOTHER part printed under it, mm:
// the profile's z_gap_mm when it states one, else kMinAirLayers of its
// layer_height_mm, else kMinAirLayers of FDM's common kFdmLayerMm; 0 for a
// process that is not FDM (check_print's stacking item skips, saying why).
inline constexpr int kMinAirLayers = 3;
inline constexpr double kFdmLayerMm = 0.2;
[[nodiscard]] double MinAirMm(const Process& p);
// Where it came from: "profile" (its z_gap_mm), "layers" (its layer
// height) or "process"; "" when there is none.
[[nodiscard]] std::string MinAirFrom(const Process& p);
// The sentence saying so.
[[nodiscard]] std::string MinAirSource(const Process& p);

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

// The sentence a report on the built-in tables ends with: confirm with a
// coupon.
[[nodiscard]] const std::string& ConfirmNote();
// The one a report on ONE row ends with, from that row's `source`: a row
// of a built-in table (or a profile row still carrying a built-in row's
// source: copied, never measured) gets ConfirmNote(); a profile's own
// measured row names its source instead, since the coupon that sentence
// asks for is the one the row came from. Empty = ConfirmNote().
[[nodiscard]] std::string ConfirmNote(const std::string& rowSource);

} // namespace printfit
