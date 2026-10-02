#pragma once

// Feature templates (docs/modeling/templates.md): a composite that recurs --
// a bearing seat, a bolted flange -- saved ONCE as data and placed as one
// feature, a `Template` instance, wherever it is wanted.
//
// A template is a list of features with named, typed parameters. Its
// features are stored as their ordinary JSON, with three rewrites made when
// it is saved:
//   - their ids are LOCAL (1..N, in order), and so are the tags in the face
//     and edge names they reference among themselves;
//   - every expression reads only the template's parameters (a variable the
//     source features read becomes a parameter of that name), so expanding
//     one depends on nothing but the instance's values;
//   - what they stood on outside the selection is a parameter too: a
//     sketch's plane or face is a reference parameter it is ATTACHED to, and
//     any other face or edge outside is one bound at its place in the JSON.
// No code runs. Expanding is substituting values into that JSON.

#include "core/ElementMap.h"
#include "core/Expression.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

class Document;
class Feature;
struct Body;

namespace tmpl {

enum class ParamKind {
  Length,    // mm, an expression
  Angle,     // degrees, an expression
  Count,     // a whole number >= 1, an expression
  String,    // text: a designation, a thread, an end condition
  Reference, // what it stands on: a face, a plane, a frame, Top/Front/Right
};
const char* ParamKindName(ParamKind k); // "length", "angle", ...
[[nodiscard]] bool ParamKindFromString(const std::string& s, ParamKind& out);
std::string ParamKindList();

struct Param {
  std::string name; // an identifier; `on` is the reference it stands on
  ParamKind kind = ParamKind::Length;
  std::string comment;
  // What it was when the template was saved: a number (mm, degrees), a
  // string, or a reference in the `on` form. What the insert tool starts
  // from; never substituted for a value an add_feature left out.
  nlohmann::json example;
  // A String's accepted values; empty = any non-empty text (the feature it
  // lands in still refuses what it does not know, naming what it does).
  std::vector<std::string> choices;
  bool operator==(const Param&) const = default;
};

// Where a String or Reference parameter lands in a template feature.
struct Binding {
  uint64_t feature = 0; // the template-local id
  // A JSON pointer into that feature's JSON ("/designation", "/edges/2"),
  // or kAttach: the sketch stands on the reference (face_ref, plane_ref or
  // a world plane, whichever the value is).
  std::string path;
  std::string param;
  bool operator==(const Binding&) const = default;
};
inline constexpr const char* kAttach = "attach";

struct Template {
  std::string name;
  std::string comment;
  // Bumped every time a template of this name is saved over; an instance
  // records the one it expanded (docs/modeling/templates.md#revisions).
  uint64_t revision = 1;
  std::vector<Param> params;
  // The features, in order, as FeatureToJson writes them, with local ids
  // and with null where a binding puts its value.
  nlohmann::json features = nlohmann::json::array();
  std::vector<Binding> bindings;
  bool operator==(const Template&) const = default;

  const Param* FindParam(const std::string& name) const;
};

// Empty when `t` is well formed; else what is wrong, naming it. Checks the
// name, unique identifier parameters (not ending in `_expr`), known kinds,
// examples of the right type, choices only on strings, local ids 1..N in
// order, every binding naming a feature and a parameter of a matching kind,
// every reference and string parameter bound somewhere, and every
// expression reading nothing but numeric parameters.
[[nodiscard]] std::string Validate(const Template& t);

nlohmann::json ToJson(const Template& t);
// Refuses (throws std::runtime_error, naming it) a template that does not
// validate: never loaded as a different one.
Template FromJson(const nlohmann::json& j);
nlohmann::json LibraryToJson(const std::vector<Template>& library);
// A missing or null value is an empty library. Throws on a bad template or
// two of one name.
std::vector<Template> LibraryFromJson(const nlohmann::json& j);
const Template* Find(const std::vector<Template>& library,
                     const std::string& name);
// "bearing seat (rev 2), post (rev 1)", or "none".
std::string LibraryNames(const std::vector<Template>& library);

// ---- an instance's values ------------------------------------------------

struct Value {
  double number = 0.0; // Length (mm), Angle (degrees), Count
  std::string expr;    // its expression; empty = a literal
  std::string text;    // String
  nlohmann::json ref;  // Reference: "Top" | {feature_id, element} |
                       // {feature_id} (a DatumPlane, Sketch or Frame)
  bool operator==(const Value&) const = default;
};
using Values = std::map<std::string, Value>;

// The instance's values from an add_feature request: `params` an object of
// name -> value (a number, or an expression string, for a number kind; a
// string; a reference), `on` the reference parameter of that name when the
// request gives it at the top level. REFUSES (throws, naming what is
// accepted) an unknown name, a missing one (naming each with its kind and
// saved example: a value is never filled in), a value of the wrong JSON
// type, a reference not in the `on` form, and a string outside `choices`.
// Numbers given as expressions are not evaluated here.
Values ValuesFromRequest(const Template& t, const nlohmann::json& params,
                         const nlohmann::json* on);
// The saved form: {name: number, name_expr?: text, name: "text",
// name: reference}, `on` excluded (the instance writes it at its top
// level). The `_expr` pairing is what check_literals and extract_variables
// read an instance's numbers by.
nlohmann::json ValuesToJson(const Template& t, const Values& v);
// The saved form back (ValuesToJson's, plus `on`); tolerant of nothing it
// does not know, like the request parser.
Values ValuesFromJson(const Template& t, const nlohmann::json& params,
                      const nlohmann::json* on);

// Refuses a value a kind cannot take once evaluated: a count that is not a
// whole number >= 1, a non-finite number. Empty = fine.
[[nodiscard]] std::string CheckEvaluated(const Template& t, const Values& v);

// The parameters as the template's features read them: numbers only.
SymbolTable ParamSymbols(const Template& t, const Values& v);

// ---- expansion -----------------------------------------------------------

using LocalToId = std::function<uint64_t(uint64_t local)>;
// The body (its created_by) that carries the face or edge `name`, for a
// reference the template made on a body it did not create -- a bore cut
// into the face it stands on; 0 when no body has it.
using HostOf = std::function<uint64_t(const std::string& name)>;

// Feature `k`'s JSON as the instance builds it: local ids and the tags in
// names mapped through `ids`, a host reference given its body, and the
// String and Reference values put where they are bound.
std::expected<nlohmann::json, std::string>
ExpandFeatureJson(const Template& t, size_t k, const Values& v,
                  const LocalToId& ids, const HostOf& hostOf);

// ExpandFeatureJson, built (FeatureFromJson, which refuses what a feature
// refuses) and with its expressions evaluated against the parameters. The
// error names the template feature.
std::expected<std::unique_ptr<Feature>, std::string>
BuildFeature(const Template& t, size_t k, const Values& v, const LocalToId& ids,
             const HostOf& hostOf);

// A host lookup over `bodies` as they stand.
HostOf HostIn(const std::vector<Body>& bodies);

// Every local id, in order.
std::vector<uint64_t> LocalIds(const Template& t);

// The parameter identifiers in `text` replaced by what the instance gives
// them -- a number's expression in parentheses, or the number itself -- so
// an unlinked feature reads the document's variables the instance read.
std::string SubstituteParams(const Template& t, const Values& v,
                             const std::string& text);

// ---- saving one -------------------------------------------------------------

struct SaveSpec {
  std::string name, comment;
  std::vector<uint64_t> featureIds; // any order; kept in the document's
  // A literal field made a parameter: the feature, its check_literals path
  // ("distance", "entities/0/r"), and the parameter it becomes. Two fields
  // may share one parameter only when they hold the same number.
  struct Literal {
    uint64_t featureId = 0;
    std::string path;
    std::string param;
  };
  std::vector<Literal> literals;
  // A string field made a parameter: a JSON pointer into the feature
  // ("/designation").
  struct Text {
    uint64_t featureId = 0;
    std::string path;
    std::string param;
    std::vector<std::string> choices;
  };
  std::vector<Text> strings;
  // A variable the features read becomes a parameter of its own name, or
  // of the name given here.
  std::map<std::string, std::string> rename;
  // A parameter's comment.
  std::map<std::string, std::string> comments;
};

// The template `spec` describes, from `doc` as it stands (regenerated).
// Throws std::runtime_error naming what cannot be saved: an unknown id, a
// feature type a template cannot carry (another instance, a Derived, an
// Import, a Frame), a reference to a feature outside the selection that is
// not a face, an edge or a plane, a field that is not a literal or must
// not be driven, an expression reading a table. The result is checked by
// expanding it with its own examples: it must give back exactly the
// features it was saved from.
Template FromFeatures(const Document& doc, const SaveSpec& spec);

// What Save as template would offer for `ids`: the variables the features
// read (each a parameter already), the literal fields that could become
// parameters, and the references outside the selection.
struct Candidate {
  uint64_t featureId = 0;
  std::string feature; // its name
  std::string path;
  std::string label;
  ParamKind kind = ParamKind::Length;
  double value = 0.0;
  std::string suggested; // a parameter name (extract_variables' naming)
};
struct Suggestions {
  std::vector<std::string> variables; // read by the features: parameters
  std::vector<Candidate> literals;
  std::vector<std::string> references; // "on: face '2/cap1' of 'Plate'"
};
Suggestions Suggest(const Document& doc, const std::vector<uint64_t>& ids);

} // namespace tmpl
