// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// A number that may be an expression of the variables, and stays one
// (docs/variables/README.md#numbers-that-are-expressions).
//
// ExprValue is the evaluated number beside the text that drives it; empty
// text is a literal. ExprInt is a count (an expression that comes out
// fractional is refused, never rounded) and ExprAngle an angle stored in
// radians and spoken in degrees, as every angle at the surface is
// (docs/conventions.md#units-and-angles).
//
// Whatever holds such numbers exposes them through one hook, VisitExprs,
// handing each to an ExprVisitor as an ExprSlot: the text, the value in the
// expression's units, what it measures and where it lives. Evaluation,
// variable rename, the refusal to delete a variable something reads, and
// the literals lint all walk that one hook.

#include "core/Expression.h"

#include <glm/vec3.hpp>
#include <nlohmann/json_fwd.hpp>

#include <array>
#include <functional>
#include <string>
#include <vector>

// What a number measures. A literal is only ever compared with a variable
// of the same kind (the literals lint): 52 teeth are not the 52 mm wall.
enum class ExprQuantity {
  Length, // mm
  Angle,  // degrees at the surface
  Count,  // unitless: a pattern count, a helix's turns
  // Anything else: a force, a ratio, a weight. A variable's comment can say
  // it is one ("N, a fingertip pull"); the lint never matches it.
  Other,
};
const char* ExprQuantityName(ExprQuantity q); // "length", "angle", ...

// The kind a field's NAME says it is: "angle", "rotation", "..._deg" an
// angle, "count" and "turns" a count, anything else a length. What the
// literals lint read off a JSON key before slots carried their quantity,
// and still how a sketch entity's coordinate is told apart.
ExprQuantity QuantityOfFieldName(const std::string& name);

struct ExprValue {
  double value = 0.0;
  std::string expr; // empty: `value` is a literal

  ExprValue() = default;
  explicit ExprValue(double v) : value(v) {} // a literal: `ExprValue d{5.0}`
  // Sets the VALUE; the text is untouched. What evaluation and a computed
  // default write. Unbinding is clearing `expr`, said where it is meant.
  ExprValue& operator=(double v) {
    value = v;
    return *this;
  }
  operator double() const { return value; } // NOLINT: what geometry reads
  [[nodiscard]] bool Bound() const { return !expr.empty(); }

  friend bool operator==(const ExprValue& a, const ExprValue& b) {
    return a.value == b.value && a.expr == b.expr;
  }
  friend bool operator==(const ExprValue& a, double b) { return a.value == b; }
};

struct ExprInt {
  int value = 0;
  std::string expr;

  ExprInt() = default;
  explicit ExprInt(int v) : value(v) {}
  ExprInt& operator=(int v) {
    value = v;
    return *this;
  }
  operator int() const { return value; } // NOLINT
  [[nodiscard]] bool Bound() const { return !expr.empty(); }

  friend bool operator==(const ExprInt& a, const ExprInt& b) {
    return a.value == b.value && a.expr == b.expr;
  }
  friend bool operator==(const ExprInt& a, int b) { return a.value == b; }
};

// Stored in radians, which is what `value` and the conversion hold; written,
// read and evaluated in degrees.
struct ExprAngle {
  double value = 0.0; // radians
  std::string expr;   // evaluates to DEGREES

  ExprAngle() = default;
  static ExprAngle FromDegrees(double deg);
  static ExprAngle FromRadians(double rad) {
    ExprAngle a;
    a.value = rad;
    return a;
  }
  // Sets the value in RADIANS, which is what `value` and the conversion
  // hold; the text is untouched (ExprValue's rule): what a solve writes.
  ExprAngle& operator=(double radians) {
    value = radians;
    return *this;
  }
  [[nodiscard]] double Degrees() const;
  operator double() const { return value; } // NOLINT
  [[nodiscard]] bool Bound() const { return !expr.empty(); }

  friend bool operator==(const ExprAngle& a, const ExprAngle& b) {
    return a.value == b.value && a.expr == b.expr;
  }
};

// One expression-capable number as a visitor sees it.
class ExprSlot {
public:
  ExprSlot(ExprValue& v, ExprQuantity q, std::string path);
  ExprSlot(ExprInt& v, std::string path);
  ExprSlot(ExprAngle& v, std::string path);
  // A pair that is not one of the types: a sketch dimension, one coordinate
  // of a vector the geometry reads whole.
  ExprSlot(std::string& expr, double& value, ExprQuantity q, std::string path);
  // The same with the value stored in radians (a sketch angle dimension).
  static ExprSlot Radians(std::string& expr, double& radians, std::string path);

  [[nodiscard]] std::string& Expr() const { return *m_expr; }
  [[nodiscard]] bool Bound() const { return !m_expr->empty(); }
  // In the expression's units: degrees for an angle stored in radians.
  [[nodiscard]] double Value() const;
  // Stores an evaluated result (in the expression's units). False, with the
  // reason, when the slot refuses it: a count that is not a whole number.
  [[nodiscard]] bool Set(double v, std::string* why = nullptr) const;
  [[nodiscard]] ExprQuantity Quantity() const { return m_quantity; }
  // Where it lives in its owner's JSON, `_expr` left off: "distance",
  // "loads[2].magnitude".
  [[nodiscard]] const std::string& Path() const { return m_path; }
  // The same number under another path: a study's slot as its assembly
  // sees it ("studies/0/loads/1/magnitude").
  [[nodiscard]] ExprSlot WithPath(std::string path) const {
    ExprSlot s = *this;
    s.m_path = std::move(path);
    return s;
  }

private:
  ExprSlot() = default;
  std::string* m_expr = nullptr;
  double* m_double = nullptr;
  int* m_int = nullptr;
  bool m_radians = false;
  ExprQuantity m_quantity = ExprQuantity::Length;
  std::string m_path;
};

// A slot is a handle: const, it still reaches the number it names.
using ExprVisitor = std::function<void(const ExprSlot&)>;

// A vector the geometry reads whole (a box's position, a datum's point): one
// text per coordinate, "" for a literal one, each visited as a slot of its
// own at `<path>/0..2`.
using Vec3Exprs = std::array<std::string, 3>;
void VisitVec3Exprs(const ExprVisitor& visit, glm::dvec3& v, Vec3Exprs& texts,
                    ExprQuantity q, const std::string& path);
[[nodiscard]] inline bool AnyBound(const Vec3Exprs& t) {
  return !t[0].empty() || !t[1].empty() || !t[2].empty();
}

// Evaluates a bound slot over `symbols` into its value; a literal is left
// alone. On failure the last good value stays and `why` gets the engine's
// message ("unknown variable 'bore'") or the slot's refusal.
[[nodiscard]] bool EvaluateSlot(const ExprSlot& slot,
                                const SymbolTable& symbols,
                                std::string* why = nullptr);
// Whether the slot's text names `name` (a variable or a table cell).
[[nodiscard]] bool SlotReads(const ExprSlot& slot, const std::string& name);
// Renames `from` to `to` in the slot's text. True when the text changed.
bool RenameInSlot(const ExprSlot& slot, const std::string& from,
                  const std::string& to);

// ---- JSON: `<key>` and `<key>_expr` ---------------------------------------

// Writes `<key>` (the value) and, when bound, `<key>_expr` (the text): the
// format's standing convention, so a literal writes what it always did. An
// ExprAngle's `<key>` is in degrees.
void WriteExpr(nlohmann::json& j, const std::string& key, const ExprValue& v);
void WriteExpr(nlohmann::json& j, const std::string& key, const ExprInt& v);
void WriteExpr(nlohmann::json& j, const std::string& key, const ExprAngle& v);
// A log line ({fmt}, found by name) prints an ExprValue as its value.
inline double format_as(const ExprValue& v) { return v.value; }
inline int format_as(const ExprInt& v) { return v.value; }

// An ExprValue put straight into JSON is its value: what a report or an RPC
// answer that shows the number writes. A document's file writes both keys,
// through WriteExpr.
void to_json(nlohmann::json& j, const ExprValue& v);

// Only `<key>_expr`, when bound: for a value written under its own condition.
void WriteExprText(nlohmann::json& j, const std::string& key,
                   const std::string& expr);

// Reads `<key>` when present (else the value is left as it is) and
// `<key>_expr` when present (else the text is cleared). An `_expr` that is
// not text is REFUSED (std::runtime_error naming the key). One that is text
// but does not parse is KEPT: a file with one bad expression still opens,
// and the evaluation marks what holds it broken with the parse error, as an
// unknown variable does. The RPC refuses such a text where it arrives
// (ReadExprParam, RefuseUnreadableExpr); a document can hold one, so every
// reader of a document's own JSON (a load, an undo, a copy) must take it.
// Returns whether either key was there.
bool ReadExpr(const nlohmann::json& j, const std::string& key, ExprValue& v);
bool ReadExpr(const nlohmann::json& j, const std::string& key, ExprInt& v);
bool ReadExpr(const nlohmann::json& j, const std::string& key, ExprAngle& v);
// Only `<key>_expr`, read as above; empty when absent.
std::string ReadExprText(const nlohmann::json& j, const std::string& key);
// `t` as an expression text, read as above naming `what` (the key it came
// from): for a text stored under a key of its own (a sketch dimension's
// `expr`, one of a Frame's `origin_expr` triple). Empty is a literal.
std::string ExprTextFromJson(const nlohmann::json& t, const std::string& what);
// Refuses (std::runtime_error naming `what`) a text that does not parse:
// what the RPC applies to every expression it is handed, so a call never
// stores one that can only fail.
void RefuseUnreadableExpr(const std::string& text, const std::string& what);

// An angle written in RADIANS under `<key>`, its text (in degrees, as every
// angle expression is) under `<key>_expr`: the assembly's mate values and
// limits, which the file has always held in radians.
void WriteExprRadians(nlohmann::json& j, const std::string& key,
                      const ExprAngle& v);
bool ReadExprRadians(const nlohmann::json& j, const std::string& key,
                     ExprAngle& v);
// An array of numbers whose entries may each be bound: `<key>_expr` is an
// array of as many texts, "" for a literal entry, written only when one is
// bound. ReadExprTexts gives `n` texts, all empty when the key is absent,
// and refuses one of another length or an entry that is not text.
void WriteExprTexts(nlohmann::json& j, const std::string& key,
                    const std::vector<std::string>& texts);
std::vector<std::string> ReadExprTexts(const nlohmann::json& j,
                                       const std::string& key, size_t n);

// A vector's texts: `<key>_expr` as [x, y, z] texts ("" for a literal
// coordinate), written only when one is bound; read as ReadExprTexts reads.
void WriteVec3Exprs(nlohmann::json& j, const std::string& key,
                    const Vec3Exprs& texts);
Vec3Exprs ReadVec3Exprs(const nlohmann::json& j, const std::string& key);
// A list of points' texts: `<key>_expr` as one [x, y, z] of texts per point,
// written only when one is bound. Read gives `n` entries, all empty when the
// key is absent, and refuses another length or an entry that is not three
// texts.
void WritePointExprs(nlohmann::json& j, const std::string& key,
                     const std::vector<Vec3Exprs>& texts);
std::vector<Vec3Exprs> ReadPointExprs(const nlohmann::json& j,
                                      const std::string& key, size_t n);

// The RPC's reader for a number a method sets: `<key>` as a number, or as a
// string (the older spelling the FEM methods took, kept as an alias), and
// `<key>_expr`. A number alone is a literal (the text is cleared); a text
// keeps `<key>`'s number, if one came, as the value until it is evaluated.
// Refuses a text that does not parse and a `<key>` that is neither, naming
// the key. Returns whether either key was there.
bool ReadExprParam(const nlohmann::json& p, const std::string& key,
                   ExprValue& v);
// The same for a count (a `<key>` that is not whole is refused) and for an
// angle, whose `<key>` is in degrees.
bool ReadExprParam(const nlohmann::json& p, const std::string& key, ExprInt& v);
bool ReadExprParam(const nlohmann::json& p, const std::string& key,
                   ExprAngle& v);
