// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// Literals that should be expressions
// (docs/variables/README.md#literals-that-should-be-expressions):
// the shared walk over every expression-capable number a feature carries,
// the advisory lint that reads it (`check_literals`), and the planner the
// `extract_variables` action applies.
//
// The walk is GENERIC on purpose. A feature declares its expression pairs
// through Feature::VisitExprs and writes each as "<field>_expr" in its JSON;
// Fields() puts a sentinel text into every pair of a CLONE, serializes it and
// reads back where each sentinel landed. So a new feature type participates
// the day it gets a VisitExprs and a serializer, with no table here to
// forget, and a pair its serializer does not write (an extrude's second
// distance when it is not two-sided) is no field at all — exactly right,
// since nothing reads it.

#include "core/Document.h"
#include "core/ElementMap.h"
#include "core/Feature.h"

#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace literals {

// What a number measures. A literal is only ever compared with a variable
// of the same kind: 52 teeth are not the 52 mm wall.
enum class Quantity {
  Length, // mm
  Angle,  // degrees
  Count,  // unitless: a pattern count, a helix's turns
  // A variable's only: a force, a stress, a mass -- what a variable's
  // comment says it is ("N, a fingertip pull"). No field is one, so a
  // variable of this kind is never matched.
  Other,
};
const char* QuantityName(Quantity q); // "length", "angle", "count", "other"

// One expression-capable number on a feature.
struct Field {
  // Where it lives, as a path into the feature's JSON with each `_expr`
  // suffix taken off: "distance", "origin/2", "constraints/3" (a sketch
  // dimension), "entities/1/r" (a sketch entity's coordinate). What
  // extract_variables' `fields` names.
  std::string path;
  // The same in words: "distance", "origin z", "diameter dimension",
  // "circle r".
  std::string label;
  double value = 0.0; // in the expression's units: mm, degrees
  std::string expr;   // empty: the number is a literal
  Quantity quantity = Quantity::Length;
  // Why it must not be given an expression, or empty. A sketch entity
  // coordinate a constraint names would fight the solver (drive the
  // dimension instead); a hole's diameter comes from its fit; an extrude's
  // distance means nothing through all.
  std::string notDrivable;
};

// Every expression-capable number `f` carries, in a stable order.
std::vector<Field> Fields(const Feature& f);

// Sets the expression text of the field at `path` (no evaluation: the next
// regeneration evaluates it). False when `f` has no such field.
[[nodiscard]] bool SetFieldExpr(Feature& f, const std::string& path,
                                const std::string& text);

// The numbers in `f`'s JSON that carry NO expression twin, as top-level
// keys ("cb_diameter"): what extract_variables reports leaving alone. Ids,
// references and flags are not numbers anyone would extract.
std::vector<std::string> FieldsWithoutTwin(const Feature& f);

// A variable as the lint compares against it.
struct Variable {
  std::string name;
  std::string scope; // "part" or "project"
  double value = 0.0;
  // The kinds it has been USED as (a bare reference in a field of that
  // kind, or any reference from a length field), plus what its name says
  // ("..._deg", "angle", "count", "teeth") and what its comment's leading
  // unit says ("mm, ...", "N, ...", "MPa", "deg") -- or a ratio, a factor.
  // Empty = never used and nothing said: compared with lengths only.
  std::set<Quantity> kinds;
};

// The variables `doc` can see: its own (part scope), then the project's
// that it does not shadow. `uses` are the documents whose fields teach each
// variable its kind — every part of the project, `doc` included.
std::vector<Variable>
VariablesInScope(const Document& doc,
                 const std::vector<Parameter>* projectParams,
                 const std::vector<const Document*>& uses);

// Whether a literal of quantity `q` and value `v` could mean `var`.
[[nodiscard]] bool Matches(const Variable& var, Quantity q, double v);

struct LiteralFinding {
  uint64_t featureId = 0;
  std::string feature; // its name
  Field field;
  std::vector<std::string> variables; // every variable it equals
};

// A sketch (a primitive is one) or an explicit Frame placed in the WORLD —
// on a base plane, an offset of one, a datum plane, a fixed frame — whose
// plane a planar face of the part already lies on. Attached to the face it
// would follow the part; attached to the world it floats, or is buried, the
// day the part changes.
struct PlacementFinding {
  uint64_t featureId = 0;
  std::string feature;
  std::string on; // "Top", "datum plane 'Deck'", "the plane z = 28.7"
  glm::dvec3 origin{0.0}, normal{0.0, 0.0, 1.0};
  ElementRef face;         // what to attach it to instead
  uint64_t faceMadeBy = 0; // the feature that made that face
  std::string faceFeature; // its name
};

struct Report {
  std::vector<LiteralFinding> literals;
  std::vector<PlacementFinding> placements;
  size_t fieldsChecked = 0;     // literal numbers compared
  size_t placementsChecked = 0; // world-placed sketches and frames
};

// The lint. Advisory: it reports and never refuses. `doc` must have
// regenerated (the placement half reads its bodies).
Report Lint(const Document& doc, const std::vector<Variable>& variables);

// ---- extract_variables ----------------------------------------------------

struct ExtractOptions {
  std::vector<uint64_t> featureIds; // empty: every feature
  std::vector<std::string> paths;   // empty: every field (Field::path)
  std::string prefix;               // prepended to every proposed name
  // Equal literals in DIFFERENT features share one variable. Off by
  // default: equal is not the same as related. Within one feature equal
  // values always share one.
  bool mergeEqual = false;
  // Bind a literal to the ONE existing variable it equals (what the lint
  // found) instead of proposing a new one.
  bool reuse = false;
  // Names already taken anywhere a new variable could collide or be
  // shadowed: every variable of the project and of every part.
  std::set<std::string> taken;
};

struct Use {
  uint64_t featureId = 0;
  std::string feature;
  Field field;
  std::string expression; // what the field gets: "name" or "-name"
};

struct Proposal {
  std::string name;
  std::string expression; // the new variable's; empty when existing
  double value = 0.0;
  Quantity quantity = Quantity::Length;
  bool existing = false; // `reuse`: an existing variable, not a new one
  std::vector<Use> uses;
};

struct LeftAlone {
  uint64_t featureId = 0;
  std::string feature;
  std::string path;
  std::string label;
  double value = 0.0;
  bool hasValue = true;
  std::string reason;
};

struct ExtractPlan {
  std::vector<Proposal> proposals;
  std::vector<LeftAlone> leftAlone;
};

// Plans the extraction; changes nothing. Throws std::runtime_error naming
// what exists for an unknown feature id or field path (refuse, never
// substitute).
ExtractPlan PlanExtract(const Document& doc,
                        const std::vector<Variable>& variables,
                        const ExtractOptions& options);

// The shortest text that reads back as exactly `v`: what a new variable's
// expression is, so the geometry it drives does not move by an ulp.
std::string ExactNumber(double v);

} // namespace literals
