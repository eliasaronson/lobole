#pragma once

#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>

// Expression evaluation for variables & dimensions (docs/variables/plan.md).
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

  // Evaluates against the given symbols. On failure returns nullopt and
  // fills outError (unknown identifier, division by zero, non-finite
  // result, ...). Never returns NaN/inf — those become errors so bad math
  // can't reach the geometry kernel.
  virtual std::optional<double> Evaluate(const SymbolTable& symbols,
                                         ExprError* outError) const = 0;
};

class ExpressionEngine {
public:
  virtual ~ExpressionEngine() = default;

  // Parses text into a reusable expression. On failure returns nullptr and
  // fills outError.
  virtual std::unique_ptr<CompiledExpr> Compile(const std::string& text,
                                                ExprError* outError) const = 0;

  // One-shot convenience: Compile + Evaluate.
  std::optional<double> EvaluateText(const std::string& text,
                                     const SymbolTable& symbols,
                                     ExprError* outError) const;

  // Rewrites references to oldName into newName, leaving every other byte
  // (spacing, parens, case of other identifiers) untouched so stored
  // expressions stay diff-friendly. Text that fails to tokenize is returned
  // unchanged. Backs parameter rename.
  [[nodiscard]] virtual std::string RenameIdentifier(
      const std::string& text, const std::string& oldName,
      const std::string& newName) const = 0;

  // True if name is usable as a parameter name for this engine: a valid
  // identifier that doesn't collide with built-in functions/constants.
  [[nodiscard]] virtual bool IsValidParameterName(
      const std::string& name) const = 0;

  // The engine used throughout the application (currently the built-in
  // recursive-descent one). Singleton because expressions must evaluate
  // identically everywhere a document is regenerated.
  static const ExpressionEngine& Default();
};

// Built-in engine: numbers, identifiers, + - * / ^ (right-associative,
// binding tighter than unary minus, so -2^2 = -4), parens, the constant
// `pi`, and functions sin cos tan sqrt abs min max floor ceil — trig in
// degrees, matching the dimension UI.
class BuiltinExpressionEngine final : public ExpressionEngine {
public:
  std::unique_ptr<CompiledExpr> Compile(const std::string& text,
                                        ExprError* outError) const override;
  [[nodiscard]] std::string RenameIdentifier(
      const std::string& text, const std::string& oldName,
      const std::string& newName) const override;
  [[nodiscard]] bool IsValidParameterName(
      const std::string& name) const override;
};
