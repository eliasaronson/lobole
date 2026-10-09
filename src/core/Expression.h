// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <expected>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>

// Expression evaluation for variables & dimensions (docs/variables/README.md).
//
// The document model only ever talks to the ExpressionEngine interface below:
// it hands over expression *text* and gets back values, dependency sets, and
// rewritten text. Nothing outside this module sees tokens or syntax trees, so
// the built-in recursive-descent engine can later be swapped for a heavier
// parser or a full scripting runtime without touching Document or features.

// Values of the document's parameters, as seen during one evaluation.
using SymbolTable = std::unordered_map<std::string, double>;

// A failure from Compile or Evaluate. offset/length span the offending part
// of the expression text (both 0 when the error has no useful location), so
// the UI can underline it and the remote API can report it precisely.
struct ExprError {
  std::string message;
  size_t offset = 0;
  size_t length = 0;
};

// A parsed expression, reusable across evaluations. Produced by an engine
// and only meaningful with the engine that produced it.
class CompiledExpr {
public:
  virtual ~CompiledExpr() = default;

  // Identifiers the expression references. Drives the parameter topo-sort
  // and referenced-delete refusal; function names are not included.
  [[nodiscard]] virtual const std::set<std::string>& Dependencies() const = 0;

  // Evaluates against the given symbols. On failure returns an unexpected
  // ExprError (unknown identifier, division by zero, non-finite result, ...).
  // Never returns NaN/inf — those become errors so bad math can't reach the
  // geometry kernel.
  virtual std::expected<double, ExprError>
  Evaluate(const SymbolTable& symbols) const = 0;
};

class ExpressionEngine {
public:
  virtual ~ExpressionEngine() = default;

  // Parses text into a reusable expression. On failure returns an unexpected
  // ExprError.
  virtual std::expected<std::unique_ptr<CompiledExpr>, ExprError>
  Compile(const std::string& text) const = 0;

  // One-shot convenience: Compile + Evaluate, short-circuiting on the first
  // failure.
  std::expected<double, ExprError>
  EvaluateText(const std::string& text, const SymbolTable& symbols) const;

  // Rewrites references to oldName into newName, leaving every other byte
  // (spacing, parens, case of other identifiers) untouched so stored
  // expressions stay diff-friendly. Text that fails to tokenize is returned
  // unchanged. Backs parameter rename.
  [[nodiscard]] virtual std::string
  RenameIdentifier(const std::string& text, const std::string& oldName,
                   const std::string& newName) const = 0;

  // True if name is usable as a parameter name for this engine: a valid
  // identifier that doesn't collide with built-in functions/constants.
  [[nodiscard]] virtual bool
  IsValidParameterName(const std::string& name) const = 0;

  // The engine used throughout the application (currently the built-in
  // recursive-descent one). Singleton because expressions must evaluate
  // identically everywhere a document is regenerated.
  static const ExpressionEngine& Default();
};

// Project tables in expressions (docs/variables/README.md#tables).
// `lookup("table", key, "column")` reads a cell of a project table. The
// tables reach an evaluation as SYMBOLS, one per cell, under names no
// identifier can spell — so a lookup evaluates wherever a variable does,
// with no second channel into the evaluator. The owner of the tables seeds
// them into the scope (lookuptable::AddCellSymbols); anything that lists
// symbols skips IsCellSymbol ones.
namespace lookuptable {
[[nodiscard]] std::string CellSymbol(const std::string& table,
                                     const std::string& key,
                                     const std::string& column);
[[nodiscard]] bool IsCellSymbol(const std::string& name);
[[nodiscard]] bool SplitCellSymbol(const std::string& name, std::string& table,
                                   std::string& key, std::string& column);
// A numeric key's one spelling: the shortest text that reads back as the
// same double ("6002", "6.35"), so 6002 from an expression, a JSON 6002 and
// the string "6002" are one row.
[[nodiscard]] std::string NumericKey(double value);
} // namespace lookuptable

// Built-in engine: numbers, identifiers, + - * / ^ (right-associative,
// binding tighter than unary minus, so -2^2 = -4), parens, the constant
// `pi`, and functions sin cos tan sqrt abs min max floor ceil — trig in
// degrees, matching the dimension UI — and lookup("table", key, "column"),
// whose table and column are quoted names ("..." or '...') and whose key is
// a quoted name or a numeric expression.
class BuiltinExpressionEngine final : public ExpressionEngine {
public:
  std::expected<std::unique_ptr<CompiledExpr>, ExprError>
  Compile(const std::string& text) const override;
  [[nodiscard]] std::string
  RenameIdentifier(const std::string& text, const std::string& oldName,
                   const std::string& newName) const override;
  [[nodiscard]] bool
  IsValidParameterName(const std::string& name) const override;
};
