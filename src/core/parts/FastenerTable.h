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

  [[nodiscard]] bool Has(const std::string& key) const {
    return rows.find(key) != rows.end();
  }
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

// Test seam: point the loader at a directory other than the installed one.
// Empty restores the default (RuntimePaths::PartsDir()). Clears the cache.
void SetDataDirForTesting(const std::string& dir);

} // namespace parts
