// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/Template.h"

#include "cad/SketchPlane.h"
#include "core/Document.h"
#include "core/ElementOrigin.h"
#include "core/Feature.h"
#include "core/Literals.h"
#include "core/Primitives.h"
#include "core/io/Serialization.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>

using nlohmann::json;

namespace tmpl {

namespace {

struct KindSpelling {
  ParamKind kind;
  const char* name;
};
constexpr KindSpelling kKinds[] = {
    {ParamKind::Length, "length"},       {ParamKind::Angle, "angle"},
    {ParamKind::Count, "count"},         {ParamKind::String, "string"},
    {ParamKind::Reference, "reference"},
};

bool Numeric(ParamKind k) {
  return k == ParamKind::Length || k == ParamKind::Angle ||
         k == ParamKind::Count;
}

bool EndsWith(const std::string& s, const std::string& tail) {
  return s.size() >= tail.size() &&
         s.compare(s.size() - tail.size(), tail.size(), tail) == 0;
}

// The keys whose integer value is the id of another feature of the same
// history, and the keys holding a list of them. A key this list misses is
// caught where it matters: FromFeatures expands what it saved and compares
// the references (VisitFeatureRefs) with the originals'.
const std::set<std::string> kIdKeys = {
    "feature_id", "sketch_id",     "path_id",   "parent_id",
    "guide_id",   "unfold_id",     "tool_id",   "target_id",
    "source_id",  "profile_id",    "plane_ref", "datum_id",
    "member_id",  "datum_axis_id", "body_id",   "source_sketch_id"};
const std::set<std::string> kIdListKeys = {"sketch_ids", "against", "mitre",
                                           "bends"};

bool IsElementRef(const json& j) {
  return j.is_object() && j.contains("feature_id") && j.contains("element") &&
         j.at("feature_id").is_number_integer() && j.at("element").is_string();
}

// Every feature id `name` mentions (RewriteIds' reading).
std::vector<uint64_t> Mentioned(const std::string& name) {
  std::vector<uint64_t> out;
  (void)topo::RewriteIds(name, [&](uint64_t id) {
    out.push_back(id);
    return id;
  });
  return out;
}

std::string Quoted(const std::string& s) { return "'" + s + "'"; }

std::string ExampleText(const Param& p) {
  if (p.example.is_number())
    return literals::ExactNumber(p.example.get<double>());
  return p.example.dump();
}

// Every expression text a feature's JSON carries: a string under a key
// ending in `_expr` (or an array of them), and a sketch dimension's `expr`.
void CollectExprs(const json& j, std::vector<std::string>& out,
                  bool exprKey = false) {
  if (j.is_object()) {
    for (auto it = j.begin(); it != j.end(); ++it)
      CollectExprs(it.value(), out,
                   EndsWith(it.key(), "_expr") || it.key() == "expr");
  } else if (j.is_array()) {
    for (const json& e : j)
      CollectExprs(e, out, exprKey);
  } else if (j.is_string() && exprKey) {
    const std::string& s = j.get_ref<const std::string&>();
    if (!s.empty())
      out.push_back(s);
  }
}

json PlaneJson(const SketchPlane& p) {
  const auto v = [](const glm::dvec3& x) {
    return json::array({x.x, x.y, x.z});
  };
  return {{"origin", v(p.GetOrigin())},
          {"normal", v(p.GetNormal())},
          {"uAxis", v(p.GetUAxis())}};
}

// The world plane a baked sketch plane IS, exactly (an offset or tilted one
// is no world plane: it stays where it is).
std::optional<std::string> StandardPlaneOf(const json& plane) {
  for (PlaneType t : {PlaneType::XY, PlaneType::XZ, PlaneType::YZ}) {
    if (PlaneJson(SketchPlane(t)) == plane) {
      std::string name = SketchPlane::StandardPlaneName(t);
      return name.substr(0, name.find(' '));
    }
  }
  return std::nullopt;
}

// Maps every local id in a template feature's JSON (the top-level id, the
// id keys, the tags in names) through `ids`, and gives a host reference
// (feature_id 0 on a name of its own) the body that carries it.
std::string Remap(json& j, bool top, size_t n, const LocalToId& ids,
                  const HostOf& hostOf) {
  const auto local = [&](uint64_t v, std::string& err) -> uint64_t {
    if (v == 0)
      return 0;
    if (v > n) {
      err = "names id " + std::to_string(v) +
            ", which is not one of the template's features (1.." +
            std::to_string(n) + ")";
      return 0;
    }
    return ids(v);
  };
  std::string err;
  if (j.is_object()) {
    if (IsElementRef(j) && !j.at("element").get<std::string>().empty()) {
      const std::string name =
          topo::RewriteIds(j.at("element").get<std::string>(),
                           [&](uint64_t v) { return local(v, err); });
      if (!err.empty())
        return "face or edge '" + j.at("element").get<std::string>() + "' " +
               err;
      j["element"] = name;
      const uint64_t fid = j.at("feature_id").get<uint64_t>();
      if (fid == 0) {
        const uint64_t host = hostOf ? hostOf(name) : 0;
        if (host == 0)
          return "the face or edge '" + name +
                 "' it names is on no body: a feature before it in the "
                 "template did not make it";
        j["feature_id"] = host;
      } else {
        j["feature_id"] = local(fid, err);
        if (!err.empty())
          return err;
      }
    }
    for (auto it = j.begin(); it != j.end(); ++it) {
      const std::string& key = it.key();
      json& v = it.value();
      if (key == "id") {
        if (top && v.is_number_integer()) {
          v = local(v.get<uint64_t>(), err);
          if (!err.empty())
            return err;
        }
        continue;
      }
      if (IsElementRef(j) && (key == "feature_id" || key == "element"))
        continue;
      if (kIdKeys.contains(key) && v.is_number_integer()) {
        v = local(v.get<uint64_t>(), err);
        if (!err.empty())
          return "`" + key + "` " + err;
        continue;
      }
      if (kIdListKeys.contains(key) && v.is_array()) {
        for (json& e : v)
          if (e.is_number_integer()) {
            e = local(e.get<uint64_t>(), err);
            if (!err.empty())
              return "`" + key + "` " + err;
          }
        continue;
      }
      if (v.is_object() || v.is_array())
        if (std::string e = Remap(v, false, n, ids, hostOf); !e.empty())
          return e;
    }
  } else if (j.is_array()) {
    for (json& e : j)
      if (std::string x = Remap(e, false, n, ids, hostOf); !x.empty())
        return x;
  }
  return {};
}

// Strips every expression text (they are what a template rewrites) so two
// features can be compared by what they build.
void StripExprs(json& j) {
  if (j.is_object()) {
    for (auto it = j.begin(); it != j.end();) {
      if (EndsWith(it.key(), "_expr") || it.key() == "expr") {
        it = j.erase(it);
        continue;
      }
      StripExprs(it.value());
      ++it;
    }
  } else if (j.is_array()) {
    for (json& e : j)
      StripExprs(e);
  }
}

std::string FeatureLabel(const json& fj) {
  return Quoted(fj.value("name", std::string{})) + " (" +
         fj.value("type", std::string{}) + ")";
}

} // namespace

// ---- kinds and the template itself ------------------------------------------

const char* ParamKindName(ParamKind k) {
  for (const KindSpelling& s : kKinds)
    if (s.kind == k)
      return s.name;
  return "length";
}

bool ParamKindFromString(const std::string& s, ParamKind& out) {
  for (const KindSpelling& k : kKinds)
    if (s == k.name) {
      out = k.kind;
      return true;
    }
  return false;
}

std::string ParamKindList() {
  std::string out;
  for (const KindSpelling& k : kKinds)
    out += (out.empty() ? "" : ", ") + std::string(k.name);
  return out;
}

const Param* Template::FindParam(const std::string& n) const {
  for (const Param& p : params)
    if (p.name == n)
      return &p;
  return nullptr;
}

std::vector<uint64_t> LocalIds(const Template& t) {
  std::vector<uint64_t> out;
  for (const json& f : t.features)
    out.push_back(f.value("id", uint64_t{0}));
  return out;
}

std::string Validate(const Template& t) {
  const ExpressionEngine& engine = ExpressionEngine::Default();
  if (t.name.empty())
    return "a template needs a name";
  for (const char c : t.name)
    if (static_cast<unsigned char>(c) < 0x20)
      return "template name " + Quoted(t.name) + " holds a control character";
  if (t.revision == 0)
    return "template " + Quoted(t.name) + ": revision starts at 1";
  std::set<std::string> names;
  for (const Param& p : t.params) {
    if (!engine.IsValidParameterName(p.name) || EndsWith(p.name, "_expr"))
      return "template " + Quoted(t.name) + ": parameter " + Quoted(p.name) +
             " is not an identifier (letters, digits, _; not a digit "
             "first, not a function or unit name, not ending in _expr)";
    if (!names.insert(p.name).second)
      return "template " + Quoted(t.name) + ": two parameters are named " +
             Quoted(p.name);
    const bool ok = Numeric(p.kind) ? p.example.is_number()
                    : p.kind == ParamKind::String
                        ? p.example.is_string()
                        : (p.example.is_string() || p.example.is_object());
    if (!ok)
      return "template " + Quoted(t.name) + ": parameter " + Quoted(p.name) +
             " (" + ParamKindName(p.kind) +
             ") has an example of the wrong "
             "type: " +
             p.example.dump();
    if (!p.choices.empty()) {
      if (p.kind != ParamKind::String)
        return "template " + Quoted(t.name) +
               ": only a string parameter "
               "has choices (" +
               Quoted(p.name) + " is a " + ParamKindName(p.kind) + ")";
      if (std::find(p.choices.begin(), p.choices.end(),
                    p.example.get<std::string>()) == p.choices.end())
        return "template " + Quoted(t.name) + ": parameter " + Quoted(p.name) +
               "'s example is not one of its choices";
    }
  }
  if (!t.features.is_array() || t.features.empty())
    return "template " + Quoted(t.name) + " has no features";
  for (size_t k = 0; k < t.features.size(); k++) {
    const json& f = t.features[k];
    if (!f.is_object() || !f.contains("type") || !f.at("type").is_string())
      return "template " + Quoted(t.name) + ": feature " +
             std::to_string(k + 1) + " has no type";
    if (f.value("id", uint64_t{0}) != k + 1)
      return "template " + Quoted(t.name) + ": feature " +
             std::to_string(k + 1) + " has id " +
             std::to_string(f.value("id", uint64_t{0})) +
             " (a template's features are numbered 1..N in order)";
    std::vector<std::string> texts;
    CollectExprs(f, texts);
    for (const std::string& text : texts) {
      auto c = engine.Compile(text);
      if (!c)
        return "template " + Quoted(t.name) + ": feature " + FeatureLabel(f) +
               " expression '" + text + "': " + c.error().message;
      for (const std::string& dep : (*c)->Dependencies()) {
        const Param* p = t.FindParam(dep);
        if (p == nullptr || !Numeric(p->kind))
          return "template " + Quoted(t.name) + ": feature " + FeatureLabel(f) +
                 " expression '" + text + "' reads " + Quoted(dep) +
                 ", which is not a number parameter of the template (a "
                 "template sees only its parameters)";
      }
    }
  }
  std::set<std::string> bound;
  for (const Binding& b : t.bindings) {
    const Param* p = t.FindParam(b.param);
    if (p == nullptr || Numeric(p->kind))
      return "template " + Quoted(t.name) + ": a binding names " +
             Quoted(b.param) + ", which is not a string or reference parameter";
    if (b.feature == 0 || b.feature > t.features.size())
      return "template " + Quoted(t.name) + ": a binding of " +
             Quoted(b.param) + " names feature " + std::to_string(b.feature);
    const json& f = t.features[b.feature - 1];
    if (b.path == kAttach) {
      if (p->kind != ParamKind::Reference ||
          f.value("type", std::string{}) != "Sketch")
        return "template " + Quoted(t.name) +
               ": only a Sketch is attached, "
               "and only to a reference parameter (" +
               Quoted(b.param) + " on " + FeatureLabel(f) + ")";
    } else {
      json::json_pointer ptr;
      try {
        ptr = json::json_pointer(b.path);
      } catch (const std::exception&) {
        return "template " + Quoted(t.name) + ": binding path " +
               Quoted(b.path) + " is not a JSON pointer";
      }
      if (!f.contains(ptr))
        return "template " + Quoted(t.name) + ": binding path " +
               Quoted(b.path) + " is not in " + FeatureLabel(f);
      const json& at = f.at(ptr);
      const bool placeholder =
          p->kind == ParamKind::String
              ? at.is_null()
              : (IsElementRef(at) && at.at("feature_id").get<uint64_t>() == 0 &&
                 at.at("element").get<std::string>().empty());
      if (!placeholder)
        return "template " + Quoted(t.name) + ": binding path " +
               Quoted(b.path) + " of " + FeatureLabel(f) +
               " does not hold a placeholder";
    }
    bound.insert(b.param);
  }
  for (const Param& p : t.params)
    if (!Numeric(p.kind) && !bound.contains(p.name))
      return "template " + Quoted(t.name) + ": parameter " + Quoted(p.name) +
             " is bound nowhere";
  return {};
}

json ToJson(const Template& t) {
  json j = {{"name", t.name}, {"revision", t.revision}};
  if (!t.comment.empty())
    j["comment"] = t.comment;
  json params = json::array();
  for (const Param& p : t.params) {
    json pj = {{"name", p.name},
               {"kind", ParamKindName(p.kind)},
               {"example", p.example}};
    if (!p.comment.empty())
      pj["comment"] = p.comment;
    if (!p.choices.empty())
      pj["choices"] = p.choices;
    params.push_back(std::move(pj));
  }
  j["parameters"] = std::move(params);
  j["features"] = t.features;
  json bindings = json::array();
  for (const Binding& b : t.bindings)
    bindings.push_back(
        {{"feature", b.feature}, {"path", b.path}, {"param", b.param}});
  j["bindings"] = std::move(bindings);
  return j;
}

Template FromJson(const json& j) {
  if (!j.is_object())
    throw std::runtime_error("a template is an object: {name, revision, "
                             "parameters, features, bindings}");
  for (auto it = j.begin(); it != j.end(); ++it)
    if (it.key() != "name" && it.key() != "revision" && it.key() != "comment" &&
        it.key() != "parameters" && it.key() != "features" &&
        it.key() != "bindings")
      throw std::runtime_error("template: unknown key '" + it.key() +
                               "' (it has name, revision, comment, "
                               "parameters, features, bindings)");
  Template t;
  try {
    t.name = j.at("name").get<std::string>();
    t.revision = j.value("revision", uint64_t{1});
    t.comment = j.value("comment", std::string{});
    for (const json& pj : j.value("parameters", json::array())) {
      Param p;
      p.name = pj.at("name").get<std::string>();
      const std::string kind = pj.at("kind").get<std::string>();
      if (!ParamKindFromString(kind, p.kind))
        throw std::runtime_error("template " + Quoted(t.name) + ": parameter " +
                                 Quoted(p.name) + " has kind " + Quoted(kind) +
                                 " (kinds: " + ParamKindList() + ")");
      p.comment = pj.value("comment", std::string{});
      p.example = pj.value("example", json());
      p.choices = pj.value("choices", std::vector<std::string>{});
      t.params.push_back(std::move(p));
    }
    t.features = j.value("features", json::array());
    for (const json& bj : j.value("bindings", json::array()))
      t.bindings.push_back({bj.at("feature").get<uint64_t>(),
                            bj.at("path").get<std::string>(),
                            bj.at("param").get<std::string>()});
  } catch (const json::exception& e) {
    throw std::runtime_error("template " + Quoted(j.value("name", "?")) +
                             " is malformed: " + e.what());
  }
  if (std::string why = Validate(t); !why.empty())
    throw std::runtime_error(why);
  return t;
}

json LibraryToJson(const std::vector<Template>& library) {
  json out = json::array();
  for (const Template& t : library)
    out.push_back(ToJson(t));
  return out;
}

std::vector<Template> LibraryFromJson(const json& j) {
  std::vector<Template> out;
  if (j.is_null())
    return out;
  if (!j.is_array())
    throw std::runtime_error("templates: a list of templates");
  for (const json& tj : j) {
    Template t = FromJson(tj);
    if (Find(out, t.name) != nullptr)
      throw std::runtime_error("templates: two are named " + Quoted(t.name));
    out.push_back(std::move(t));
  }
  return out;
}

const Template* Find(const std::vector<Template>& library,
                     const std::string& name) {
  for (const Template& t : library)
    if (t.name == name)
      return &t;
  return nullptr;
}

std::string LibraryNames(const std::vector<Template>& library) {
  std::string out;
  for (const Template& t : library)
    out += (out.empty() ? "" : ", ") + Quoted(t.name) + " (rev " +
           std::to_string(t.revision) + ")";
  return out.empty() ? "none" : out;
}

// ---- values
// ---------------------------------------------------------------------

namespace {

std::string ParamList(const Template& t) {
  std::string out;
  for (const Param& p : t.params)
    out +=
        (out.empty() ? "" : ", ") + p.name + " (" + ParamKindName(p.kind) + ")";
  return out.empty() ? "none" : out;
}

void SetValue(const Template& t, const Param& p, const json& v, Value& out,
              bool fromSaved) {
  const std::string where =
      "template " + Quoted(t.name) + " parameter " + Quoted(p.name);
  switch (p.kind) {
  case ParamKind::Length:
  case ParamKind::Angle:
  case ParamKind::Count:
    if (v.is_number()) {
      out.number = v.get<double>();
      if (!std::isfinite(out.number))
        throw std::runtime_error(where + " is not a finite number");
      if (!fromSaved && p.kind == ParamKind::Count &&
          (out.number < 1.0 || out.number != std::floor(out.number)))
        throw std::runtime_error(where +
                                 " is a count: a whole number >= 1, "
                                 "not " +
                                 literals::ExactNumber(out.number));
    } else if (v.is_string()) {
      out.expr = v.get<std::string>();
      if (out.expr.empty())
        throw std::runtime_error(where + ": an empty expression");
    } else {
      throw std::runtime_error(where + " is a " + ParamKindName(p.kind) +
                               ": a number (" +
                               (p.kind == ParamKind::Angle ? "degrees" : "mm") +
                               ") or an expression string, not " + v.dump());
    }
    return;
  case ParamKind::String:
    if (!v.is_string() || v.get<std::string>().empty())
      throw std::runtime_error(where + " is a string (saved as " +
                               p.example.dump() + "), not " + v.dump());
    out.text = v.get<std::string>();
    if (!p.choices.empty() && std::find(p.choices.begin(), p.choices.end(),
                                        out.text) == p.choices.end()) {
      std::string list;
      for (const std::string& c : p.choices)
        list += (list.empty() ? "" : ", ") + c;
      throw std::runtime_error(where + " is one of: " + list + " (not " +
                               Quoted(out.text) + ")");
    }
    return;
  case ParamKind::Reference: {
    // The `on` form, refused naming it (primitive::OnFromJson).
    try {
      out.ref = primitive::OnToJson(primitive::OnFromJson(v));
    } catch (const std::exception& e) {
      throw std::runtime_error(where + ": " + e.what());
    }
    return;
  }
  }
}

} // namespace

Values ValuesFromRequest(const Template& t, const json& params,
                         const json* on) {
  if (!params.is_null() && !params.is_object())
    throw std::runtime_error("parameters is an object of name: value (the "
                             "template's: " +
                             ParamList(t) + ")");
  Values out;
  const auto take = [&](const std::string& key, const json& v) {
    std::string name = key;
    bool exprKey = false;
    if (EndsWith(key, "_expr") && t.FindParam(key) == nullptr) {
      name = key.substr(0, key.size() - 5);
      exprKey = true;
    }
    const Param* p = t.FindParam(name);
    if (p == nullptr)
      throw std::runtime_error("template " + Quoted(t.name) +
                               " has no parameter " + Quoted(name) +
                               " (it has: " + ParamList(t) + ")");
    if (exprKey && !Numeric(p->kind))
      throw std::runtime_error("parameter " + Quoted(name) + " is a " +
                               ParamKindName(p->kind) +
                               "; only a number takes an expression");
    if (exprKey && !v.is_string())
      throw std::runtime_error(key + " is an expression string");
    Value& val = out[name];
    if (exprKey) {
      val.expr = v.get<std::string>();
      return;
    }
    const std::string keep = val.expr;
    SetValue(t, *p, v, val, false);
    if (!keep.empty() && val.expr.empty())
      val.expr = keep; // {d: 47, d_expr: "x"}: the expression drives
  };
  if (params.is_object())
    for (auto it = params.begin(); it != params.end(); ++it)
      take(it.key(), it.value());
  if (on != nullptr && !on->is_null()) {
    if (out.contains("on"))
      throw std::runtime_error("`on` is given twice: at the top level and in "
                               "parameters");
    take("on", *on);
  }
  std::string missing;
  for (const Param& p : t.params)
    if (!out.contains(p.name))
      missing += (missing.empty() ? "" : ", ") + p.name + " (" +
                 ParamKindName(p.kind) + ", saved as " + ExampleText(p) + ")";
  if (!missing.empty())
    throw std::runtime_error("template " + Quoted(t.name) +
                             " needs every parameter; missing: " + missing);
  return out;
}

json ValuesToJson(const Template& t, const Values& v) {
  json out = json::object();
  for (const Param& p : t.params) {
    if (p.name == "on" && p.kind == ParamKind::Reference)
      continue;
    const auto it = v.find(p.name);
    if (it == v.end())
      continue;
    switch (p.kind) {
    case ParamKind::Length:
    case ParamKind::Angle:
    case ParamKind::Count:
      out[p.name] = it->second.number;
      if (!it->second.expr.empty())
        out[p.name + "_expr"] = it->second.expr;
      break;
    case ParamKind::String:
      out[p.name] = it->second.text;
      break;
    case ParamKind::Reference:
      out[p.name] = it->second.ref;
      break;
    }
  }
  return out;
}

Values ValuesFromJson(const Template& t, const json& params, const json* on) {
  Values out;
  for (const Param& p : t.params) {
    const json* v = nullptr;
    if (p.name == "on" && p.kind == ParamKind::Reference && on != nullptr &&
        !on->is_null())
      v = on;
    else if (params.is_object() && params.contains(p.name))
      v = &params.at(p.name);
    if (v == nullptr)
      continue; // WhyIncomplete names it
    Value& val = out[p.name];
    SetValue(t, p, *v, val, true);
    if (Numeric(p.kind) && params.is_object() &&
        params.contains(p.name + "_expr"))
      val.expr = params.at(p.name + "_expr").get<std::string>();
  }
  return out;
}

std::string CheckEvaluated(const Template& t, const Values& v) {
  for (const Param& p : t.params) {
    const auto it = v.find(p.name);
    if (it == v.end())
      return "parameter " + Quoted(p.name) + " has no value";
    if (!Numeric(p.kind))
      continue;
    const double x = it->second.number;
    if (!std::isfinite(x))
      return "parameter " + Quoted(p.name) + " is not a finite number";
    if (p.kind == ParamKind::Count && (x < 1.0 || x != std::floor(x)))
      return "parameter " + Quoted(p.name) +
             " is a count: a whole number >= 1, not " +
             literals::ExactNumber(x);
  }
  return {};
}

SymbolTable ParamSymbols(const Template& t, const Values& v) {
  SymbolTable s;
  for (const Param& p : t.params)
    if (Numeric(p.kind))
      if (const auto it = v.find(p.name); it != v.end())
        s[p.name] = it->second.number;
  return s;
}

// ---- expansion
// -----------------------------------------------------------------

std::expected<json, std::string> ExpandFeatureJson(const Template& t, size_t k,
                                                   const Values& v,
                                                   const LocalToId& ids,
                                                   const HostOf& hostOf) {
  if (k >= t.features.size())
    return std::unexpected("no template feature " + std::to_string(k + 1));
  json j = t.features[k];
  if (std::string e = Remap(j, true, t.features.size(), ids, hostOf);
      !e.empty())
    return std::unexpected(FeatureLabel(j) + ": " + e);
  const uint64_t local = k + 1;
  for (const Binding& b : t.bindings) {
    if (b.feature != local)
      continue;
    const Param* p = t.FindParam(b.param);
    const auto it = v.find(b.param);
    if (p == nullptr || it == v.end())
      return std::unexpected("parameter " + Quoted(b.param) + " has no value");
    if (p->kind == ParamKind::String) {
      j[json::json_pointer(b.path)] = it->second.text;
      continue;
    }
    const json& ref = it->second.ref;
    if (b.path == kAttach) {
      j.erase("face_ref");
      j.erase("plane_ref");
      const primitive::On on = primitive::OnFromJson(ref);
      switch (on.kind) {
      case primitive::OnKind::WorldPlane:
        j["plane"] = PlaneJson(SketchPlane(on.world));
        break;
      case primitive::OnKind::Face:
        j["face_ref"] = {{"feature_id", on.face.featureId},
                         {"element", on.face.name}};
        break;
      case primitive::OnKind::PlaneFeature:
        j["plane_ref"] = on.featureId;
        break;
      }
      continue;
    }
    if (!ref.is_object() || !ref.contains("element"))
      return std::unexpected(
          "parameter " + Quoted(b.param) + " lands on a face or an edge of " +
          FeatureLabel(j) + ": give it {feature_id, element}, not " +
          ref.dump());
    json& at = j[json::json_pointer(b.path)];
    at["feature_id"] = ref.at("feature_id");
    at["element"] = ref.at("element");
  }
  return j;
}

std::expected<std::unique_ptr<Feature>, std::string>
BuildFeature(const Template& t, size_t k, const Values& v, const LocalToId& ids,
             const HostOf& hostOf) {
  auto j = ExpandFeatureJson(t, k, v, ids, hostOf);
  if (!j)
    return std::unexpected(j.error());
  std::unique_ptr<Feature> f;
  try {
    f = DocumentIO::FeatureFromJson(*j);
  } catch (const std::exception& e) {
    return std::unexpected(FeatureLabel(*j) + ": " + e.what());
  }
  const SymbolTable symbols = ParamSymbols(t, v);
  std::string err;
  f->VisitExprs([&](std::string& text, double& value) {
    if (text.empty() || !err.empty())
      return;
    const auto r = ExpressionEngine::Default().EvaluateText(text, symbols);
    if (r)
      value = *r;
    else
      err = "'" + text + "': " + r.error().message;
  });
  if (!err.empty())
    return std::unexpected(FeatureLabel(*j) + ": " + err);
  return f;
}

HostOf HostIn(const std::vector<Body>& bodies) {
  return [&bodies](const std::string& name) -> uint64_t {
    for (const Body& b : bodies) {
      for (const auto& [i, n] : b.elementMap.names)
        if (n == name)
          return b.createdBy;
      for (const auto& [i, n] : b.elementMap.edgeNames)
        if (n == name)
          return b.createdBy;
    }
    return 0;
  };
}

std::string SubstituteParams(const Template& t, const Values& v,
                             const std::string& text) {
  if (text.empty())
    return text;
  const ExpressionEngine& engine = ExpressionEngine::Default();
  // Through sentinels, so a value that names another parameter is not
  // substituted twice.
  std::string out = text;
  std::vector<std::pair<std::string, std::string>> swaps;
  for (const Param& p : t.params) {
    if (!Numeric(p.kind))
      continue;
    const auto it = v.find(p.name);
    if (it == v.end())
      continue;
    const std::string sentinel =
        "__template_param_" + std::to_string(swaps.size()) + "__";
    const std::string next = engine.RenameIdentifier(out, p.name, sentinel);
    if (next == out)
      continue;
    out = next;
    swaps.emplace_back(sentinel, it->second.expr.empty()
                                     ? literals::ExactNumber(it->second.number)
                                     : "(" + it->second.expr + ")");
  }
  for (const auto& [sentinel, value] : swaps)
    out = engine.RenameIdentifier(out, sentinel, value);
  return out;
}

// ---- saving one
// ---------------------------------------------------------------------

namespace {

void RefuseType(const Feature& f) {
  const std::string type = f.TypeName();
  std::string why;
  if (type == "Template")
    why = "it is a template instance itself: unlink it first, or save "
          "the template it expands";
  else if (type == "Derived")
    why = "it is a snapshot of another part, held as assets of this one";
  else if (type == "Import")
    why = "it reads a file on disk";
  else if (type == "Frame")
    why = "a mate anchors to a frame by name, and nothing outside an "
          "instance can see the features inside it";
  if (!why.empty())
    throw std::runtime_error("a template cannot carry " + Quoted(f.GetName()) +
                             " (" + type + "): " + why);
  if (f.IsSuppressed())
    throw std::runtime_error(Quoted(f.GetName()) +
                             " is suppressed: unsuppress it or leave it out");
}

std::string FeatureNameOf(const Document& doc, uint64_t id) {
  const Feature* f = doc.FindFeatureById(id);
  return f ? Quoted(f->GetName()) : "feature " + std::to_string(id);
}

std::string Escape(const std::string& key);

// What the walk over a feature's JSON found outside the selection.
struct External {
  json example; // the `on` form
  std::string param;
};

struct Saver {
  const Document& doc;
  const SaveSpec& spec;
  std::map<uint64_t, uint64_t> local; // source id -> local id
  Template t;
  std::vector<External> externals;
  // A host reference's body, by the name it had in the source: the check
  // expands with it, since the source's bodies have moved on since.
  std::map<std::string, uint64_t> hostAt;

  Param& AddParam(const std::string& name, ParamKind kind,
                  const json& example) {
    for (Param& p : t.params)
      if (p.name == name) {
        if (p.kind != kind)
          throw std::runtime_error("parameter " + Quoted(name) +
                                   " would be both a " + ParamKindName(p.kind) +
                                   " and a " + ParamKindName(kind));
        if (p.example != example &&
            !(p.example.is_number() && example.is_number() &&
              std::abs(p.example.get<double>() - example.get<double>()) <=
                  1e-12 * std::max(1.0, std::abs(example.get<double>()))))
          throw std::runtime_error(
              "parameter " + Quoted(name) +
              " would stand for two different values (" + p.example.dump() +
              " and " + example.dump() +
              "): give them two parameters, or make them equal first");
        return p;
      }
    Param p;
    p.name = name;
    p.kind = kind;
    p.example = example;
    if (const auto c = spec.comments.find(name); c != spec.comments.end())
      p.comment = c->second;
    t.params.push_back(std::move(p));
    return t.params.back();
  }

  std::string ExternalParam(const json& example, bool attach) {
    for (const External& e : externals)
      if (e.example == example)
        return e.param;
    size_t n = 0;
    for (const External& e : externals)
      if ((e.param.rfind("on", 0) == 0) == attach)
        n++;
    const std::string base = attach ? "on" : "ref";
    std::string name = n == 0 ? base : base + std::to_string(n + 1);
    while (t.FindParam(name) != nullptr)
      name += "_";
    externals.push_back({example, name});
    AddParam(name, ParamKind::Reference, example);
    return name;
  }

  uint64_t Local(uint64_t id) const {
    const auto it = local.find(id);
    return it == local.end() ? 0 : it->second;
  }

  // The walk: references outside the selection become parameters, ids and
  // names inside it become local.
  void Walk(json& j, const std::string& ptr, const json& feature) {
    if (j.is_object()) {
      if (IsElementRef(j) && !j.at("element").get<std::string>().empty()) {
        const uint64_t fid = j.at("feature_id").get<uint64_t>();
        const std::string name = j.at("element").get<std::string>();
        const std::vector<uint64_t> ids = Mentioned(name);
        size_t inside = 0;
        for (uint64_t id : ids)
          inside += local.contains(id) ? 1 : 0;
        if (inside == 0 && !local.contains(fid)) {
          // Outside: a reference parameter, bound here.
          const std::string param =
              ExternalParam({{"feature_id", fid}, {"element", name}}, false);
          j["feature_id"] = 0;
          j["element"] = "";
          t.bindings.push_back(
              {Local(feature.at("id").get<uint64_t>()), ptr, param});
        } else if (inside == ids.size() && !ids.empty()) {
          j["element"] =
              topo::RewriteIds(name, [&](uint64_t id) { return Local(id); });
          if (local.contains(fid)) {
            j["feature_id"] = Local(fid);
          } else {
            // On a body it did not make: found by its name when expanded.
            hostAt[name] = fid;
            j["feature_id"] = 0;
          }
        } else {
          throw std::runtime_error(
              FeatureLabel(feature) + " names " + Quoted(name) +
              ", a face or edge made partly inside the selection and partly "
              "outside it (" +
              FeatureNameOf(doc, fid) +
              "): a template follows its own faces or the ones it stands "
              "on, not one both changed");
        }
      }
      for (auto it = j.begin(); it != j.end(); ++it) {
        const std::string& key = it.key();
        json& v = it.value();
        if (ptr.empty() && key == "id")
          continue;
        if (IsElementRef(j) && (key == "feature_id" || key == "element"))
          continue;
        if (kIdKeys.contains(key) && v.is_number_integer()) {
          const uint64_t id = v.get<uint64_t>();
          if (id == 0)
            continue;
          if (!local.contains(id))
            throw std::runtime_error(
                FeatureLabel(feature) + " names " + FeatureNameOf(doc, id) +
                " by `" + key +
                "`, outside the selection: select it too. A template stands "
                "on faces, edges, planes and frames, not on other features");
          v = Local(id);
          continue;
        }
        if (kIdListKeys.contains(key) && v.is_array()) {
          for (json& e : v)
            if (e.is_number_integer() && e.get<uint64_t>() != 0) {
              if (!local.contains(e.get<uint64_t>()))
                throw std::runtime_error(
                    FeatureLabel(feature) + " names " +
                    FeatureNameOf(doc, e.get<uint64_t>()) + " by `" + key +
                    "`, outside the selection: select it too");
              e = Local(e.get<uint64_t>());
            }
          continue;
        }
        if (v.is_object() || v.is_array())
          Walk(v, ptr + "/" + Escape(key), feature);
      }
    } else if (j.is_array()) {
      for (size_t i = 0; i < j.size(); i++)
        Walk(j[i], ptr + "/" + std::to_string(i), feature);
    }
  }
};

// A JSON pointer's escaping of one key (RFC 6901).
std::string Escape(const std::string& key) {
  std::string out;
  for (const char c : key)
    out += c == '~'   ? std::string("~0")
           : c == '/' ? std::string("~1")
                      : std::string(1, c);
  return out;
}

std::multiset<uint64_t> RefsOf(const Feature& f) {
  std::multiset<uint64_t> out;
  f.VisitFeatureRefs([&](uint64_t id) {
    if (id != 0)
      out.insert(id);
  });
  return out;
}

std::string FirstDifference(const json& a, const json& b) {
  const json patch = json::diff(a, b);
  if (patch.empty())
    return {};
  const json& op = patch.front();
  return op.value("path", std::string("/")) + " (" +
         (a.contains(json::json_pointer(op.value("path", std::string(""))))
              ? a.at(json::json_pointer(op.value("path", std::string(""))))
                    .dump()
              : std::string("absent")) +
         " became " + op.value("value", json()).dump() + ")";
}

} // namespace

Template FromFeatures(const Document& doc, const SaveSpec& spec) {
  if (spec.name.empty())
    throw std::runtime_error("a template needs a name");
  if (spec.featureIds.empty())
    throw std::runtime_error("save_template: name the features to save "
                             "(feature_ids)");
  // The features, in the document's order.
  std::vector<std::pair<size_t, const Feature*>> chosen;
  for (uint64_t id : spec.featureIds) {
    size_t index = 0;
    const Feature* f = doc.FindFeatureById(id, &index);
    if (f == nullptr) {
      std::string known;
      for (const auto& g : doc.GetFeatures())
        known += (known.empty() ? "" : ", ") + std::to_string(g->GetId()) +
                 " " + Quoted(g->GetName());
      throw std::runtime_error("no feature with id " + std::to_string(id) +
                               " in this part (it has: " +
                               (known.empty() ? std::string("none") : known) +
                               ")");
    }
    if (std::none_of(chosen.begin(), chosen.end(),
                     [&](const auto& c) { return c.second == f; }))
      chosen.emplace_back(index, f);
  }
  std::sort(chosen.begin(), chosen.end());
  for (const auto& [i, f] : chosen) {
    RefuseType(*f);
    if (!f->GetError().empty())
      throw std::runtime_error(Quoted(f->GetName()) + " is in error (" +
                               f->GetError() +
                               "): a template is saved from features that "
                               "build");
  }

  Saver s{doc, spec, {}, {}, {}, {}};
  s.t.name = spec.name;
  s.t.comment = spec.comment;
  for (size_t k = 0; k < chosen.size(); k++)
    s.local[chosen[k].second->GetId()] = k + 1;

  // Clones, so the literals and renames can be written into them.
  std::vector<std::unique_ptr<Feature>> clones;
  for (const auto& [i, f] : chosen)
    clones.push_back(f->Clone());

  // Literal fields made parameters.
  for (const SaveSpec::Literal& lit : spec.literals) {
    size_t k = 0;
    while (k < chosen.size() && chosen[k].second->GetId() != lit.featureId)
      k++;
    if (k == chosen.size())
      throw std::runtime_error("literal " + Quoted(lit.path) + ": feature " +
                               std::to_string(lit.featureId) +
                               " is not one of the features saved");
    const std::vector<literals::Field> fields = literals::Fields(*clones[k]);
    const literals::Field* field = nullptr;
    std::string known;
    for (const literals::Field& fd : fields) {
      known += (known.empty() ? "" : ", ") + fd.path;
      if (fd.path == lit.path)
        field = &fd;
    }
    if (field == nullptr)
      throw std::runtime_error(
          Quoted(chosen[k].second->GetName()) + " has no field " +
          Quoted(lit.path) +
          " (fields: " + (known.empty() ? std::string("none") : known) + ")");
    if (!field->expr.empty())
      throw std::runtime_error(
          Quoted(chosen[k].second->GetName()) + " " + lit.path +
          " is the expression '" + field->expr +
          "', not a literal: the variables it reads are parameters already");
    if (!field->notDrivable.empty())
      throw std::runtime_error(Quoted(chosen[k].second->GetName()) + " " +
                               lit.path +
                               " cannot be a parameter: " + field->notDrivable);
    if (!ExpressionEngine::Default().IsValidParameterName(lit.param))
      throw std::runtime_error(Quoted(lit.param) +
                               " is not a parameter name (an identifier, "
                               "not a function or unit)");
    const ParamKind kind =
        field->quantity == literals::Quantity::Angle   ? ParamKind::Angle
        : field->quantity == literals::Quantity::Count ? ParamKind::Count
                                                       : ParamKind::Length;
    s.AddParam(lit.param, kind, field->value);
    if (!literals::SetFieldExpr(*clones[k], lit.path, lit.param))
      throw std::runtime_error(Quoted(chosen[k].second->GetName()) + " " +
                               lit.path + " takes no expression");
  }

  // The variables the features read: parameters of their own name (or the
  // one `rename` gives), their example what they are now.
  const SymbolTable symbols = primitive::Symbols(doc);
  const ExpressionEngine& engine = ExpressionEngine::Default();
  for (size_t k = 0; k < clones.size(); k++) {
    std::map<std::string, literals::Quantity> kindOf;
    for (const literals::Field& fd : literals::Fields(*clones[k]))
      if (!fd.expr.empty())
        kindOf[fd.expr] = fd.quantity;
    std::string failure;
    clones[k]->VisitExprs([&](std::string& text, double&) {
      if (text.empty() || !failure.empty())
        return;
      if (text.find("lookup") != std::string::npos) {
        failure = "'" + text +
                  "' reads a table: a template sees only its parameters, so "
                  "make the value a variable the feature reads, which then "
                  "becomes one";
        return;
      }
      auto c = engine.Compile(text);
      if (!c) {
        failure = "'" + text + "': " + c.error().message;
        return;
      }
      const literals::Quantity q =
          kindOf.contains(text) ? kindOf.at(text) : literals::Quantity::Length;
      std::string rewritten = text;
      for (const std::string& dep : (*c)->Dependencies()) {
        const Param* lit = nullptr;
        for (const SaveSpec::Literal& l : spec.literals)
          if (l.param == dep)
            lit = s.t.FindParam(dep);
        if (lit != nullptr)
          continue; // a parameter this save made from a literal
        const auto sym = symbols.find(dep);
        if (sym == symbols.end()) {
          failure = "'" + text + "' reads " + Quoted(dep) +
                    ", which is no variable of this part";
          return;
        }
        std::string name = dep;
        if (const auto r = spec.rename.find(dep); r != spec.rename.end())
          name = r->second;
        if (!engine.IsValidParameterName(name)) {
          failure = Quoted(name) + " is not a parameter name";
          return;
        }
        try {
          s.AddParam(name,
                     q == literals::Quantity::Angle   ? ParamKind::Angle
                     : q == literals::Quantity::Count ? ParamKind::Count
                                                      : ParamKind::Length,
                     sym->second);
        } catch (const std::exception& e) {
          failure = e.what();
          return;
        }
        if (name != dep)
          rewritten = engine.RenameIdentifier(rewritten, dep, name);
      }
      text = rewritten;
    });
    if (!failure.empty())
      throw std::runtime_error(Quoted(chosen[k].second->GetName()) + ": " +
                               failure);
  }

  // The JSON, its strings made parameters, its references and ids walked.
  for (size_t k = 0; k < clones.size(); k++) {
    json fj = DocumentIO::FeatureToJson(*clones[k]);
    for (const SaveSpec::Text& st : spec.strings) {
      if (st.featureId != chosen[k].second->GetId())
        continue;
      json::json_pointer ptr;
      try {
        ptr = json::json_pointer(st.path);
      } catch (const std::exception&) {
        throw std::runtime_error(Quoted(st.path) +
                                 " is not a JSON pointer (\"/designation\")");
      }
      if (!fj.contains(ptr) || !fj.at(ptr).is_string())
        throw std::runtime_error(
            FeatureLabel(fj) + " has no string at " + Quoted(st.path) +
            " (a field at its default is not written: set it to something "
            "else first, or name a path get_state shows)");
      if (!engine.IsValidParameterName(st.param))
        throw std::runtime_error(Quoted(st.param) + " is not a parameter name");
      Param& p = s.AddParam(st.param, ParamKind::String, fj.at(ptr));
      p.choices = st.choices;
      fj[ptr] = nullptr;
      s.t.bindings.push_back({k + 1, st.path, st.param});
    }
    // A sketch stands on what it is attached to: a parameter.
    if (fj.value("type", std::string{}) == "Sketch") {
      json example;
      if (fj.contains("face_ref")) {
        const json fr = fj.at("face_ref");
        const uint64_t fid = fr.at("feature_id").get<uint64_t>();
        const std::vector<uint64_t> ids =
            Mentioned(fr.at("element").get<std::string>());
        const bool inside =
            s.local.contains(fid) ||
            std::any_of(ids.begin(), ids.end(),
                        [&](uint64_t id) { return s.local.contains(id); });
        if (!inside) {
          example = fr;
          fj.erase("face_ref");
        }
      } else if (fj.contains("plane_ref")) {
        const uint64_t id = fj.at("plane_ref").get<uint64_t>();
        if (!s.local.contains(id)) {
          example = {{"feature_id", id}};
          fj.erase("plane_ref");
        }
      } else if (const auto world = StandardPlaneOf(fj.at("plane"))) {
        example = *world;
      }
      if (!example.is_null()) {
        const std::string param = s.ExternalParam(example, true);
        s.t.bindings.push_back({k + 1, kAttach, param});
      }
    }
    s.Walk(fj, "", fj);
    fj["id"] = k + 1;
    s.t.features.push_back(std::move(fj));
  }
  if (std::string why = Validate(s.t); !why.empty())
    throw std::runtime_error(why);

  // The check: expanded with its own examples, the template gives back the
  // features it was saved from -- the same JSON (expressions aside), and
  // the same references.
  Values examples;
  for (const Param& p : s.t.params) {
    Value v;
    if (Numeric(p.kind))
      v.number = p.example.get<double>();
    else if (p.kind == ParamKind::String)
      v.text = p.example.get<std::string>();
    else
      v.ref = p.example;
    examples[p.name] = v;
  }
  std::vector<uint64_t> sourceIds;
  for (const auto& [i, f] : chosen)
    sourceIds.push_back(f->GetId());
  for (size_t k = 0; k < chosen.size(); k++) {
    const HostOf hostByName = [&](const std::string& name) -> uint64_t {
      const auto it = s.hostAt.find(name);
      return it == s.hostAt.end() ? 0 : it->second;
    };
    auto same = BuildFeature(
        s.t, k, examples, [&](uint64_t l) { return sourceIds[l - 1]; },
        hostByName);
    if (!same)
      throw std::runtime_error("the template does not rebuild " +
                               Quoted(chosen[k].second->GetName()) + ": " +
                               same.error());
    json a = DocumentIO::FeatureToJson(*chosen[k].second);
    json b = DocumentIO::FeatureToJson(**same);
    StripExprs(a);
    StripExprs(b);
    if (a != b)
      throw std::runtime_error("the template does not give back " +
                               Quoted(chosen[k].second->GetName()) + " at " +
                               FirstDifference(a, b));
    // The references, through ids no feature has, so one the walk missed
    // shows up as a stray local id.
    constexpr uint64_t kProbe = uint64_t{1} << 50;
    const HostOf probeHost = [&](const std::string& name) {
      return hostByName(topo::RewriteIds(name, [&](uint64_t id) {
        return id > kProbe && id <= kProbe + sourceIds.size()
                   ? sourceIds[id - kProbe - 1]
                   : id;
      }));
    };
    auto probe = BuildFeature(
        s.t, k, examples, [&](uint64_t l) { return kProbe + l; }, probeHost);
    if (!probe)
      throw std::runtime_error("the template does not rebuild " +
                               Quoted(chosen[k].second->GetName()) + ": " +
                               probe.error());
    std::multiset<uint64_t> want;
    for (uint64_t id : RefsOf(*chosen[k].second))
      want.insert(s.local.contains(id) ? kProbe + s.local.at(id) : id);
    if (RefsOf(**probe) != want)
      throw std::runtime_error(
          Quoted(chosen[k].second->GetName()) + " (" +
          chosen[k].second->TypeName() +
          ") carries a reference a template cannot follow yet");
  }
  return s.t;
}

Suggestions Suggest(const Document& doc, const std::vector<uint64_t>& ids) {
  Suggestions out;
  std::set<uint64_t> chosen(ids.begin(), ids.end());
  std::set<std::string> vars;
  std::set<std::string> refs;
  // Names in the extract_variables style, for the literals.
  std::map<std::pair<uint64_t, std::string>, std::string> names;
  try {
    literals::ExtractOptions opts;
    opts.featureIds = ids;
    for (const Parameter& p : doc.GetParameters())
      opts.taken.insert(p.name);
    const literals::ExtractPlan plan = literals::PlanExtract(doc, {}, opts);
    for (const literals::Proposal& p : plan.proposals)
      for (const literals::Use& u : p.uses)
        names[{u.featureId, u.field.path}] = p.name;
  } catch (const std::exception&) {
  }
  const ExpressionEngine& engine = ExpressionEngine::Default();
  for (uint64_t id : ids) {
    const Feature* f = doc.FindFeatureById(id);
    if (f == nullptr)
      continue;
    for (const literals::Field& fd : literals::Fields(*f)) {
      if (!fd.expr.empty()) {
        if (auto c = engine.Compile(fd.expr))
          for (const std::string& d : (*c)->Dependencies())
            vars.insert(d);
        continue;
      }
      if (!fd.notDrivable.empty() || std::abs(fd.value) < 1e-12)
        continue;
      Candidate c;
      c.featureId = id;
      c.feature = f->GetName();
      c.path = fd.path;
      c.label = fd.label;
      c.kind = fd.quantity == literals::Quantity::Angle   ? ParamKind::Angle
               : fd.quantity == literals::Quantity::Count ? ParamKind::Count
                                                          : ParamKind::Length;
      c.value = fd.value;
      const auto it = names.find({id, fd.path});
      c.suggested = it != names.end() ? it->second : "p_" + fd.path;
      for (char& ch : c.suggested)
        if (!std::isalnum(static_cast<unsigned char>(ch)) && ch != '_')
          ch = '_';
      out.literals.push_back(std::move(c));
    }
    f->VisitFeatureRefs([&](uint64_t r) {
      if (r != 0 && !chosen.contains(r))
        refs.insert(Quoted(f->GetName()) + " stands on " +
                    FeatureNameOf(doc, r));
    });
  }
  out.variables.assign(vars.begin(), vars.end());
  out.references.assign(refs.begin(), refs.end());
  return out;
}

} // namespace tmpl
