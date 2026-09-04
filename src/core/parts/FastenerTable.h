#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

// The vendored standard-parts dimension tables (docs/parts/plan.md, phase 1).
//
// A fastener's geometry is not invented here — it is READ. `assets/parts/`
// carries one CSV per standard, vendored from FreeCAD_FastenersWB with a
// provenance header, and every dimension a generated screw, nut or washer uses
// comes out of the row for its size. The row IS the specification: a test that
// checks a generated M6 against these numbers is checking it against ISO 4762,
// which is the only oracle worth having here.
//
// The rule that shapes the whole interface: a vendored table is a FIXED SCOPE.
// A size that is not in it is REFUSED, naming what is stocked — never
// interpolated, never rounded to a neighbour. A screw at a size nobody makes
// looks perfect in the viewport and cannot be bought, which is exactly the
// plausible-and-wrong answer this codebase refuses everywhere else.
//
// The file format, as the data actually is rather than as the plan assumed:
// one file holds SEVERAL tables separated by blank lines. `iso4762def.csv` is
// three — the dimensions, the nominal length series for that standard, and the
// length RANGE each size is made in. So "is an M6x200 a thing" is answerable
// from the vendored data, which is better than the generic ISO 888 series the
// plan expected to have to use. Some files name each table on a line of their
// own; some go straight to the header. Both shapes occur, so both parse.
namespace parts {

// One table: rows keyed by the first column ("M6", or "T20" for a drive size),
// each row a column-name -> value map.
//
// Values are doubles because every dimension in these files is one. A column
// that is blank for a size is ABSENT from that row rather than zero: zero is a
// real dimension and "this standard does not define that here" is not.
struct Table {
  std::string id; // the file's own name for it, or the file stem when unnamed
  std::vector<std::string> columns; // in file order, for error messages
  std::vector<std::string> order;   // sizes in FILE order (M2 before M10)
  std::map<std::string, std::map<std::string, double>> rows;
  // The same fields as written, before they were read as numbers. A column
  // that is NOT a number lives only here: `iso14579def.csv` names the driver
  // that fits each size ("M6","T30",…) in the middle of a dimension table, and
  // `iso7045def.csv` the Phillips size. Refusing those files as malformed —
  // which is what this parser did — would have meant no torx at all.
  //
  // A non-numeric field is therefore ABSENT from `rows` rather than zero,
  // exactly like a blank one, so nothing reads a driver name as a millimetre.
  std::map<std::string, std::map<std::string, std::string>> text;

  [[nodiscard]] bool Has(const std::string& key) const {
    return Find(key) != nullptr;
  }

  // The row for a size, tolerating the PARENTHESES the ISO tables mark a
  // non-preferred size with: ISO 4032 stocks "(M3.5)", and someone asking for
  // "M3.5" is asking for that size, not for one the standard omits. The
  // parentheses are a recommendation about which sizes to design around, not
  // part of the name — so they are matched through, and printed as-is in a
  // refusal so the recommendation is still visible.
  [[nodiscard]] const std::map<std::string, double>*
  Find(const std::string& key) const;
  [[nodiscard]] const std::string* Key(const std::string& key) const;
};

// Every table in one file, in file order. Table 0 is the dimensions.
struct TableFile {
  std::vector<Table> tables;
  [[nodiscard]] const Table* ById(const std::string& idSuffix) const;
};

// Parse a whole vendored file. Returns nullopt with `error` filled on anything
// malformed — a file that half loads is worse than one that refuses, because
// the sizes it did not read look exactly like sizes the standard omits.
[[nodiscard]] std::optional<TableFile>
ParseFile(const std::string& text, const std::string& stem, std::string* error);

// Load a standard by file stem ("iso4762def"). Cached — the tables are
// immutable and small, and a screw generated per occurrence must not re-read a
// file per body. Returns nullptr with `error` naming the path it looked in: a
// build whose data did not stage has to say so, not quietly make no fasteners.
[[nodiscard]] const TableFile* Load(const std::string& stem,
                                    std::string* error);

// One size's dimensions, or a refusal naming what IS stocked:
//
//   "ISO4762def does not stock M7 — sizes are M1.6, M2, M2.5, … , M64"
[[nodiscard]] std::optional<std::map<std::string, double>>
Dimensions(const std::string& stem, const std::string& size,
           std::string* error);

// The nominal lengths this standard is made in, ascending. Taken from the
// file's own `…length` table where it has one — which is better than a generic
// series, because it is the series for THIS fastener.
[[nodiscard]] std::vector<double> NominalLengths(const std::string& stem,
                                                 std::string* error);

// The length range a given size is made in, from the file's `…range` table:
// {min, max}. nullopt when the file does not carry one (a nut has no length).
[[nodiscard]] std::optional<std::pair<double, double>>
LengthRange(const std::string& stem, const std::string& size,
            std::string* error);

// The shortest nominal length that is at least `required`, for this standard
// and size, or a refusal naming the range.
//
// Snapping is not a nicety. A derived grip of 22.7 mm is not a screw: they are
// made in 20, 25, 30, and a part list asking for 22.7 cannot be ordered. A
// screw drawn at 22.7 looks right, which is why this refuses rather than
// rounding when nothing fits — and why it consults the per-size range, so an
// M6 is not offered at a length only an M20 is made in.
[[nodiscard]] std::optional<double> SnapLength(const std::string& stem,
                                               const std::string& size,
                                               double required,
                                               std::string* error);

// The nominal diameter a metric size names: 6 from "M6", 1.6 from "M1.6".
// nullopt for anything that is not a letter prefix and a number, which is what
// a drive size ("T20") and a typo both look like.
//
// This is the one dimension NOT in the tables: they give head and socket sizes
// and take the thread diameter as read, because it is the size's own name.
[[nodiscard]] std::optional<double> NominalDiameter(const std::string& size);

// Which thread a clearance hole of this diameter is for, and how loose a fit
// it is: {"M6", "Normal"}. nullopt when no stocked clearance matches.
//
// The BACKWARD lookup, and the reason a plain hole is enough to derive a
// fastener from. A hole drilled 6.6 mm is an M6 normal clearance and the model
// need not have said so — the same table that would size the hole answers what
// the hole was for. A diameter matching nothing is refused rather than snapped
// to the nearest, because "6.5 is nearly M6" is exactly the substitution that
// puts an M6 screw in a hole somebody drilled for something else.
//
// A diameter matching SEVERAL is refused the same way, and for the same
// reason: 46 of the table's 111 diameters are a clearance for two threads at
// different fits, so answering with the first in table order is a coin toss
// dressed as a measurement. The refusal names every candidate.
[[nodiscard]] std::optional<std::pair<std::string, std::string>>
ThreadForClearance(double diameter, std::string* error);

// The widest hole this size is ever drilled to — the "Loose" column of the
// clearance table — or nullopt when the size is not in it.
//
// For probing a stack whose hole nobody recorded. A fastener already placed
// knows its own size and axis but not the bore it went through, and a ray
// offset by the THREAD radius runs down the inside of the clearance and finds
// no material at all. Offsetting past the loosest fit instead errs toward a
// false alarm rather than a false pass, which is the right direction for a
// check.
[[nodiscard]] std::optional<double> WidestClearance(const std::string& size,
                                                    std::string* error);

// The LONGEST nominal length that is at most `available`, for a blind hole:
// the screw must not bottom out, so it is bounded from above rather than
// below. nullopt with a refusal when nothing short enough exists.
[[nodiscard]] std::optional<double> SnapLengthDown(const std::string& stem,
                                                   const std::string& size,
                                                   double available,
                                                   std::string* error);

// One field as it was WRITTEN, for a column that is not a dimension: the
// driver an ISO 14579 size takes ("T30"), the Phillips number an ISO 7045
// takes ("2"). nullopt when the file, the size or the column is not there.
[[nodiscard]] std::optional<std::string> TextField(const std::string& stem,
                                                   const std::string& size,
                                                   const std::string& column,
                                                   std::string* error);

// --- what a hole is FOR (docs/modeling/hole-standards.md) -------------------
//
// The other direction from ThreadForClearance: given the intent, the number.
// "A clearance hole for an M6, normal fit" is 6.6, and a person should not
// have to know that, look it up, and type it — which is the same U10 argument
// the fastener's length rests on. The diameter stays the thing the kernel
// cuts; the FIT is what the document remembers.

// How loose a clearance hole is. Spelled the way the vendored table's columns
// are, because those are the words a machinist uses.
enum class Fit { Close, Normal, Loose, Tapped };
[[nodiscard]] std::string FitName(Fit fit);
[[nodiscard]] std::optional<Fit> FitFromName(const std::string& name);

// The COARSE pitch of a metric thread, from the fastener tables' own `P`
// column. Derived rather than tabulated again: "M6" on its own means the
// coarse pitch everywhere in engineering, and the vendored standards already
// say what that is.
[[nodiscard]] std::optional<double> CoarsePitch(const std::string& size,
                                                std::string* error);

// The hole to drill for `thread` at `fit`, in millimetres.
//
// `pitch` is only consulted for Tapped, where the drill depends on it as well
// as on the nominal size (M6 is 1.0 coarse and 0.75 fine, and they tap to
// different holes). 0 means the coarse pitch. A tapped hole is sized for
// roughly 75% thread — the `Soft` column, which is the conventional default;
// the table's `Hard` column exists for the shallower engagement a hard
// material wants and is not offered yet.
//
// Refused, never rounded: a size the table does not carry is named against
// what it does, because a hole drilled to a plausible-but-wrong number looks
// exactly like a correct one.
[[nodiscard]] std::optional<double> HoleDiameter(const std::string& thread,
                                                 Fit fit, double pitch,
                                                 std::string* error);

// Every thread the clearance table carries, in FILE order (M1 before M10).
// For a size picker: the list IS the vendored scope, so a picker built from
// it cannot offer a size the lookup would then refuse.
[[nodiscard]] std::vector<std::string> ClearanceSizes(std::string* error);

// Test seam: point the loader at a directory other than the installed one.
// Empty restores the default (RuntimePaths::PartsDir()). Clears the cache.
void SetDataDirForTesting(const std::string& dir);

} // namespace parts
