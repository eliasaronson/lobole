#pragma once

#include "core/Expression.h"

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <vector>

// Project tables (docs/variables/README.md#tables): named grids of numbers
// keyed by row, read in any expression as lookup("table", key, "column"), so
// that bearing and screw sizes live in the model instead of in a generator's
// Python.
//
// A table is PROJECT data, beside the project variables: one source of truth
// that every part's expressions read. It lives on AppState
// (projectTables), saves in project.json, and rides a single-part file's
// envelope the way the project variables do. Cells are numbers in the
// document's base units (mm, degrees), like a variable's value.
namespace lookuptable {

struct Row {
  // The row's key: text, or a number in its one spelling (NumericKey), so a
  // numeric expression finds it.
  std::string key;
  std::vector<double> values; // one per column
  bool operator==(const Row&) const = default;
};

struct Table {
  std::string name;
  std::string comment;
  std::vector<std::string> columns;
  std::vector<Row> rows;
  bool operator==(const Table&) const = default;
};

// Empty when `t` is well formed; otherwise what is wrong, naming the row or
// column. A name and the columns are identifiers (letters, digits, _; not a
// digit first); keys are non-empty, unique and free of control characters;
// every row has one finite value per column.
[[nodiscard]] std::string Validate(const Table& t);

// A key as given in JSON: a number in its one spelling, a string as is.
// Anything else is refused (nullopt, `why`).
[[nodiscard]] std::optional<std::string> KeyFromJson(const nlohmann::json& j,
                                                     std::string* why);

// {name, comment?, columns: [...], rows: [{key, values: [...]}]}. A key that
// is a number's spelling is written as that number.
[[nodiscard]] nlohmann::json ToJson(const Table& t);
[[nodiscard]] nlohmann::json ToJson(const std::vector<Table>& tables);
// Refused (nullopt, `why`) when malformed or invalid — a table that does
// not validate is not loaded as a different one.
[[nodiscard]] std::optional<Table> FromJson(const nlohmann::json& j,
                                            std::string* why);
// Every table of a list (a missing or null value is no tables). Throws
// std::runtime_error naming the table on the first bad one: a loader
// refuses the file rather than dropping a table expressions read.
[[nodiscard]] std::vector<Table> ListFromJson(const nlohmann::json& j);

// Seeds every cell of `tables` into `out` as a CellSymbol, for evaluation.
void AddCellSymbols(const std::vector<Table>& tables, SymbolTable& out);

[[nodiscard]] const Table* Find(const std::vector<Table>& tables,
                                const std::string& name);

} // namespace lookuptable
