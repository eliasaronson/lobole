// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/Requirements.h"

#include "core/InputHash.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>
#include <stdexcept>

namespace req {

using json = nlohmann::json;

namespace {

struct KindSpec {
  const char* kind;
  bool numeric;
  const char* document; // the document kind it measures
  std::vector<std::string> keys;
  std::vector<std::string> required;
};

const std::vector<KindSpec>& Specs() {
  static const std::vector<KindSpec> specs = {
      {"study_probe",
       true,
       "part",
       {"document", "study", "case", "face", "at", "quantity", "readings",
        "expression", "unit"},
       {"document", "study"}},
      {"study_result",
       true,
       "part",
       {"document", "study", "case", "quantity"},
       {"document", "study", "quantity"}},
      {"clearance",
       true,
       "assembly",
       {"document", "a", "b", "mode", "motion_study"},
       {"document", "a", "b"}},
      {"contact",
       false,
       "assembly",
       {"document", "a", "b", "expect"},
       {"document", "a", "b", "expect"}},
      {"seal",
       true,
       "assembly",
       {"document", "a", "b", "motion_study"},
       {"document", "a", "b"}},
      {"insertion",
       false,
       "assembly",
       {"document", "a", "b", "entry", "mate", "allowance_mm", "samples",
        "rotations", "lead_in_mm"},
       {"document", "a", "b", "entry"}},
      {"mass", true, "part|assembly", {"document"}, {"document"}},
      {"volume", true, "part|assembly", {"document"}, {"document"}},
      {"fit_class",
       false,
       "assembly",
       {"document", "mate", "class"},
       {"document", "mate", "class"}},
      {"check_fits",
       false,
       "assembly",
       {"document", "allow_unverified"},
       {"document"}},
      {"check_seat",
       false,
       "assembly",
       {"document", "occurrences", "allow_unverified"},
       {"document", "occurrences"}},
      {"bearing_bore",
       true,
       "assembly",
       {"document", "occurrences"},
       {"document", "occurrences"}},
      {"section_area",
       true,
       "part",
       {"document", "of", "base_plane", "offset", "origin", "normal", "bodies"},
       {"document", "of"}},
      {"check_fasteners",
       false,
       "assembly",
       {"document", "allow", "allow_skipped"},
       {"document"}},
      {"check_literals", false, "part", {"document", "documents"}, {}},
      {"motion_clean",
       false,
       "assembly",
       {"document", "study"},
       {"document", "study"}},
      {"cam_verify",
       false,
       "job",
       {"document", "setup", "resolution", "allow_uncut"},
       {"document", "setup"}},
  };
  return specs;
}

const KindSpec* SpecFor(const std::string& kind) {
  for (const KindSpec& s : Specs())
    if (kind == s.kind)
      return &s;
  return nullptr;
}

std::string Join(const std::vector<std::string>& v) {
  std::string out;
  for (const std::string& s : v)
    out += (out.empty() ? "" : ", ") + s;
  return out;
}

// A bound side: a number, or an expression string over the project's
// variables. Anything else is refused by name.
void ReadSide(const json& j, const char* key, double& value,
              std::string& expr) {
  const json& v = j.at(key);
  if (v.is_number()) {
    value = v.get<double>();
    expr.clear();
  } else if (v.is_string() && !v.get<std::string>().empty()) {
    expr = v.get<std::string>();
    value = 0.0;
  } else {
    throw std::runtime_error(std::string("bound.") + key +
                             " is a number or an expression over the "
                             "project's variables, not " +
                             v.dump());
  }
}

void WriteSide(json& j, const char* key, double value,
               const std::string& expr) {
  if (expr.empty())
    j[key] = value;
  else
    j[key] = expr;
}

// The type of every key that is read as an id, a flag, a number or text,
// refused by the key's own name before anything reads it. Reading one
// directly let nlohmann answer: `document: "Garden shed"` came back as "a
// parameter has the wrong type -- type must be number, but is string", which
// names neither the field nor what goes in it.
void ValidateTypes(const std::string& kind, const char* document,
                   const json& m) {
  const auto refuse = [&](const std::string& key, const std::string& takes) {
    throw std::runtime_error("measure " + kind + ": `measure." + key + "` is " +
                             takes + ", not " + m.at(key).dump());
  };
  const auto id = [](const json& v) {
    return v.is_number_integer() && v.get<int64_t>() >= 0;
  };
  struct IdKey {
    const char* key;
    const char* what;
  };
  const std::string doc = std::string("the id of the ") + document +
                          " it measures (list_parts), not its name: a name "
                          "repoints silently on a rename";
  const IdKey ids[] = {
      {"study", "a study id (list_studies)"},
      {"mate", "a mate id (list_mates)"},
      {"motion_study", "a motion study id (list_motion_studies)"},
      {"setup", "a CAM setup id (list_cam_setups)"}};
  if (m.contains("document") && !id(m.at("document")))
    refuse("document", doc);
  for (const IdKey& k : ids)
    if (m.contains(k.key) && !id(m.at(k.key)))
      refuse(k.key, k.what);
  if (m.contains("documents") && m.at("documents").is_array())
    for (const json& e : m.at("documents"))
      if (!id(e))
        throw std::runtime_error("measure " + kind +
                                 ": `measure.documents` lists part ids "
                                 "(list_parts), not " +
                                 e.dump());
  for (const char* k : {"allow_unverified", "allow_skipped"})
    if (m.contains(k) && !m.at(k).is_boolean())
      refuse(k, "true or false");
  for (const char* k : {"offset", "resolution", "allowance_mm", "lead_in_mm"})
    if (m.contains(k) && !m.at(k).is_number())
      refuse(k, "a number");
  for (const char* k : {"samples", "rotations"})
    if (m.contains(k) &&
        !(m.at(k).is_number_integer() && m.at(k).get<int64_t>() >= 1))
      refuse(k, "a whole number");
  for (const char* k : {"expression", "unit"})
    if (m.contains(k) && !m.at(k).is_string())
      refuse(k, "text");
}

// Each kind's own refusals for values the vocabulary fixes.
void ValidateValues(const std::string& kind, const json& m) {
  const auto oneOf = [&](const char* key,
                         std::initializer_list<const char*> ok) {
    if (!m.contains(key))
      return;
    const std::string v =
        m.at(key).is_string() ? m.at(key).get<std::string>() : m.at(key).dump();
    for (const char* o : ok)
      if (v == o)
        return;
    std::string list;
    for (const char* o : ok)
      list += (list.empty() ? "" : ", ") + std::string(o);
    throw std::runtime_error(kind + ": unknown " + key + " '" + v +
                             "' (accepted: " + list + ")");
  };
  if (kind == "study_probe") {
    oneOf("quantity", {"translation", "rotation", "displacement_at",
                       "von_mises_max", "von_mises_mean"});
    const bool face = m.contains("face");
    const bool formula = m.contains("readings") || m.contains("expression");
    if (face == formula)
      throw std::runtime_error(
          "study_probe measures ONE face ({face, at?, quantity?}) or a "
          "formula over several ({readings: {name: {face, at?}}, "
          "expression}); pass exactly one of the two");
    if (formula && !(m.contains("readings") && m.contains("expression")))
      throw std::runtime_error(
          "study_probe's formula needs both `readings` (name -> {face, at?}) "
          "and `expression` (over <name>_tx/_ty/_tz, _rx/_ry/_rz, "
          "_dx/_dy/_dz, _cx/_cy/_cz, _vmmax/_vmmean and the project's "
          "variables)");
    if (formula) {
      if (!m.at("readings").is_object() || m.at("readings").empty())
        throw std::runtime_error(
            "study_probe readings is an object: name -> {face, at?}");
      for (auto it = m.at("readings").begin(); it != m.at("readings").end();
           ++it) {
        if (!it.value().is_object() || !it.value().contains("face"))
          throw std::runtime_error("study_probe reading '" + it.key() +
                                   "' is {face, at?}");
        for (auto k = it.value().begin(); k != it.value().end(); ++k)
          if (k.key() != "face" && k.key() != "at")
            throw std::runtime_error("study_probe reading '" + it.key() +
                                     "': unknown key '" + k.key() +
                                     "' (accepted: face, at)");
      }
    }
    if (m.value("quantity", std::string()) == "displacement_at" &&
        !m.contains("at"))
      throw std::runtime_error(
          "study_probe quantity displacement_at needs `at`, the point "
          "[x, y, z] mm the face's motion is carried to");
  } else if (kind == "study_result") {
    oneOf("quantity",
          {"max_displacement", "max_von_mises", "factor_of_safety"});
  } else if (kind == "clearance" || kind == "contact" || kind == "seal" ||
             kind == "insertion") {
    if (kind == "clearance")
      oneOf("mode", {"mesh", "exact"});
    else if (kind == "insertion")
      oneOf("entry", {"positive", "negative", "either"});
    else if (kind == "contact")
      oneOf("expect", {"clear", "touching", "not_clashing"});
    if (!m.at("a").is_number_integer() || !m.at("b").is_number_integer() ||
        m.at("a").get<int64_t>() < 0 || m.at("b").get<int64_t>() < 0)
      throw std::runtime_error(kind + ": `a` and `b` are top-level occurrence "
                                      "ids (list_occurrences), not names: a "
                                      "name repoints silently on a rename");
    if (m.at("a") == m.at("b"))
      throw std::runtime_error(kind + ": an occurrence cannot clear itself "
                                      "-- `a` and `b` are the same");
  } else if (kind == "fit_class") {
    oneOf("class", {"clearance", "transition", "interference"});
  } else if (kind == "check_seat" || kind == "bearing_bore") {
    const json& o = m.at("occurrences");
    if (!o.is_array() || o.empty())
      throw std::runtime_error(
          kind + " occurrences is a non-empty list of bearing occurrence ids");
    for (const json& id : o)
      if (!id.is_number_integer() || id.get<int64_t>() < 0)
        throw std::runtime_error(kind +
                                 " occurrences are occurrence ids "
                                 "(list_occurrences), not " +
                                 id.dump());
  } else if (kind == "section_area") {
    oneOf("of", {"material", "enclosed"});
    oneOf("base_plane", {"xy", "xz", "yz"});
    const bool byBase = m.contains("base_plane");
    const bool byPoint = m.contains("origin") || m.contains("normal");
    if (byBase == byPoint)
      throw std::runtime_error(
          "section_area cuts on ONE plane: {base_plane: xy|xz|yz, offset} or "
          "{origin: [x, y, z], normal: [x, y, z]}; pass exactly one of the "
          "two");
    // A requirement's plane is stated, never "the middle of what is shown":
    // that moves with the model, and the area with it.
    if (byBase && !(m.contains("offset") && m.at("offset").is_number()))
      throw std::runtime_error("section_area base_plane needs `offset`, mm "
                               "along the plane's normal");
    const auto vec3 = [&](const char* key) {
      if (!m.contains(key))
        throw std::runtime_error(std::string("section_area needs both "
                                             "`origin` and `normal`; `") +
                                 key + "` is missing");
      const json& v = m.at(key);
      if (!v.is_array() || v.size() != 3 ||
          !std::all_of(v.begin(), v.end(),
                       [](const json& c) { return c.is_number(); }))
        throw std::runtime_error(std::string("section_area ") + key +
                                 " is [x, y, z], not " + v.dump());
    };
    if (byPoint) {
      vec3("origin");
      vec3("normal");
      const json& n = m.at("normal");
      if (std::hypot(n[0].get<double>(), n[1].get<double>(),
                     n[2].get<double>()) < 1e-12)
        throw std::runtime_error("section_area normal is a direction, not "
                                 "zero");
    }
    if (m.contains("bodies")) {
      const json& b = m.at("bodies");
      if (!b.is_array() || b.empty())
        throw std::runtime_error("section_area bodies is a non-empty list of "
                                 "body names or the feature ids that created "
                                 "them");
      for (const json& e : b)
        if (!e.is_string() && !(e.is_number_integer() && e.get<int64_t>() > 0))
          throw std::runtime_error("section_area bodies are body names or "
                                   "feature ids, not " +
                                   e.dump());
    }
  } else if (kind == "check_fasteners") {
    if (m.contains("allow")) {
      const json& a = m.at("allow");
      if (!a.is_array())
        throw std::runtime_error(
            "check_fasteners allow is a list of {part?, why?}: a finding is "
            "allowed when its part starts with `part` and its reason "
            "contains `why`");
      for (const json& e : a) {
        if (!e.is_object() || e.empty())
          throw std::runtime_error(
              "check_fasteners allow entry is {part?, why?, max?}, not " +
              e.dump());
        for (auto k = e.begin(); k != e.end(); ++k)
          if (k.key() != "part" && k.key() != "why" && k.key() != "max")
            throw std::runtime_error("check_fasteners allow: unknown key '" +
                                     k.key() + "' (accepted: part, why, max)");
        if (e.contains("max") && !(e.at("max").is_number_integer() &&
                                   e.at("max").get<int64_t>() >= 0))
          throw std::runtime_error("check_fasteners allow: `max` is how many "
                                   "findings the entry may match, a count");
      }
    }
  } else if (kind == "check_literals") {
    if (m.contains("document") && m.contains("documents"))
      throw std::runtime_error("check_literals reads one part (`document`), "
                               "several (`documents`), or every part "
                               "(neither); not both");
    if (m.contains("documents") &&
        (!m.at("documents").is_array() || m.at("documents").empty()))
      throw std::runtime_error(
          "check_literals documents is a non-empty list of part ids");
  } else if (kind == "cam_verify") {
    if (m.contains("allow_uncut") && !m.at("allow_uncut").is_array())
      throw std::runtime_error(
          "cam_verify allow_uncut is a list of what-prefixes ('M4') the job "
          "may leave uncut");
  }
}

} // namespace

std::string MeasureKindList() {
  std::vector<std::string> v;
  for (const char* k : kMeasureKinds)
    v.push_back(k);
  return Join(v);
}

std::string BoundKindList() {
  std::vector<std::string> v;
  for (const char* k : kBoundKinds)
    v.push_back(k);
  return Join(v);
}

bool IsNumericKind(const std::string& kind) {
  const KindSpec* s = SpecFor(kind);
  return s != nullptr && s->numeric;
}

std::vector<std::string> MeasureKeys(const std::string& kind) {
  const KindSpec* s = SpecFor(kind);
  return s ? s->keys : std::vector<std::string>{};
}

std::vector<std::string> RequiredMeasureKeys(const std::string& kind) {
  const KindSpec* s = SpecFor(kind);
  return s ? s->required : std::vector<std::string>{};
}

std::string DocumentKindFor(const std::string& kind) {
  const KindSpec* s = SpecFor(kind);
  return s ? s->document : "";
}

void Validate(const json& measure, const Bound& bound) {
  if (!measure.is_object() || !measure.contains("kind") ||
      !measure.at("kind").is_string())
    throw std::runtime_error("a requirement's measure is {kind, ...}; kinds: " +
                             MeasureKindList());
  const std::string kind = measure.at("kind").get<std::string>();
  const KindSpec* spec = SpecFor(kind);
  if (spec == nullptr)
    throw std::runtime_error("unknown measure kind '" + kind +
                             "' (accepted: " + MeasureKindList() + ")");
  for (auto it = measure.begin(); it != measure.end(); ++it) {
    if (it.key() == "kind")
      continue;
    if (std::find(spec->keys.begin(), spec->keys.end(), it.key()) ==
        spec->keys.end())
      throw std::runtime_error("measure " + kind + ": unknown key '" +
                               it.key() + "' (accepted: " + Join(spec->keys) +
                               ")");
  }
  for (const std::string& k : spec->required)
    if (!measure.contains(k))
      throw std::runtime_error("measure " + kind + " needs `" + k +
                               "` (it takes: " + Join(spec->keys) + ")");
  ValidateTypes(kind, spec->document, measure);
  ValidateValues(kind, measure);

  bool knownBound = false;
  for (const char* b : kBoundKinds)
    knownBound |= bound.kind == b;
  if (!knownBound)
    throw std::runtime_error("unknown bound kind '" + bound.kind +
                             "' (accepted: " + BoundKindList() + ")");
  if (spec->numeric && bound.kind == "pass")
    throw std::runtime_error(
        "measure " + kind +
        " is a number: bound it at_most, at_least or within, not pass");
  if (!spec->numeric && bound.kind != "pass")
    throw std::runtime_error("measure " + kind +
                             " is a verdict: its bound is pass, not " +
                             bound.kind);
  if (bound.kind == "within" && bound.minExpr.empty() &&
      bound.maxExpr.empty() && bound.min > bound.max)
    throw std::runtime_error("bound within: min is above max");
}

Bound BoundFromJson(const json& j) {
  if (!j.is_object() || !j.contains("kind") || !j.at("kind").is_string())
    throw std::runtime_error("a bound is {kind, value?, min?, max?}; kinds: " +
                             BoundKindList());
  Bound b;
  b.kind = j.at("kind").get<std::string>();
  const auto only = [&](std::initializer_list<const char*> keys) {
    for (auto it = j.begin(); it != j.end(); ++it) {
      if (it.key() == "kind")
        continue;
      bool ok = false;
      for (const char* k : keys)
        ok |= it.key() == k;
      if (!ok) {
        std::string list;
        for (const char* k : keys)
          list += (list.empty() ? "" : ", ") + std::string(k);
        throw std::runtime_error(
            "bound " + b.kind + ": unknown key '" + it.key() +
            "' (accepted: " + (list.empty() ? "none" : list) + ")");
      }
    }
  };
  if (b.kind == "at_most" || b.kind == "at_least") {
    only({"value"});
    if (!j.contains("value"))
      throw std::runtime_error("bound " + b.kind + " needs `value`");
    ReadSide(j, "value", b.value, b.valueExpr);
  } else if (b.kind == "within") {
    only({"min", "max"});
    if (!j.contains("min") || !j.contains("max"))
      throw std::runtime_error("bound within needs `min` and `max`");
    ReadSide(j, "min", b.min, b.minExpr);
    ReadSide(j, "max", b.max, b.maxExpr);
  } else if (b.kind == "pass") {
    only({});
  } else {
    throw std::runtime_error("unknown bound kind '" + b.kind +
                             "' (accepted: " + BoundKindList() + ")");
  }
  return b;
}

json BoundToJson(const Bound& b) {
  json j{{"kind", b.kind}};
  if (b.kind == "at_most" || b.kind == "at_least")
    WriteSide(j, "value", b.value, b.valueExpr);
  else if (b.kind == "within") {
    WriteSide(j, "min", b.min, b.minExpr);
    WriteSide(j, "max", b.max, b.maxExpr);
  }
  return j;
}

json EvidenceToJson(const Evidence& e) {
  json j{{"pass", e.pass},
         {"input_hash", inputhash::Hex(e.inputHash)},
         {"revision", e.revision}};
  if (e.value)
    j["value"] = *e.value;
  if (!e.unit.empty())
    j["unit"] = e.unit;
  if (!e.error.empty())
    j["error"] = e.error;
  if (e.limit)
    j["limit"] = *e.limit;
  if (e.limitMin)
    j["limit_min"] = *e.limitMin;
  if (e.limitMax)
    j["limit_max"] = *e.limitMax;
  if (!e.detail.is_null() && !e.detail.empty())
    j["detail"] = e.detail;
  if (!e.checkedAt.empty())
    j["checked_at"] = e.checkedAt;
  return j;
}

Evidence EvidenceFromJson(const json& j) {
  Evidence e;
  e.taken = true;
  e.pass = j.value("pass", false);
  const std::string hex = j.value("input_hash", std::string());
  if (!inputhash::FromHex(hex, e.inputHash))
    throw std::runtime_error("requirement evidence: input_hash '" + hex +
                             "' is not 0x + 16 hex digits");
  e.revision = j.value("revision", 0);
  if (j.contains("value") && j.at("value").is_number())
    e.value = j.at("value").get<double>();
  e.unit = j.value("unit", std::string());
  e.error = j.value("error", std::string());
  if (j.contains("limit"))
    e.limit = j.at("limit").get<double>();
  if (j.contains("limit_min"))
    e.limitMin = j.at("limit_min").get<double>();
  if (j.contains("limit_max"))
    e.limitMax = j.at("limit_max").get<double>();
  if (j.contains("detail"))
    e.detail = j.at("detail");
  e.checkedAt = j.value("checked_at", std::string());
  return e;
}

json ToJson(const Requirement& r) {
  json j{{"id", r.id},
         {"name", r.name},
         {"revision", r.revision},
         {"measure", r.measure},
         {"bound", BoundToJson(r.bound)}};
  if (!r.source.empty())
    j["source"] = r.source;
  if (r.evidence.taken)
    j["evidence"] = EvidenceToJson(r.evidence);
  return j;
}

Requirement FromJson(const json& j) {
  Requirement r;
  try {
    r.id = j.at("id").get<uint64_t>();
    r.name = j.at("name").get<std::string>();
    r.source = j.value("source", std::string());
    r.revision = j.value("revision", 1);
    r.measure = j.at("measure");
    r.bound = BoundFromJson(j.at("bound"));
    Validate(r.measure, r.bound);
    if (j.contains("evidence"))
      r.evidence = EvidenceFromJson(j.at("evidence"));
  } catch (const std::exception& e) {
    throw std::runtime_error("requirement " +
                             (j.is_object() && j.contains("id")
                                  ? j.at("id").dump()
                                  : std::string("?")) +
                             ": " + e.what());
  }
  return r;
}

std::vector<Requirement> ListFromJson(const json& j) {
  std::vector<Requirement> out;
  if (j.is_null())
    return out;
  if (!j.is_array())
    throw std::runtime_error("`requirements` is a list");
  std::set<uint64_t> ids;
  for (const json& e : j) {
    out.push_back(FromJson(e));
    if (!ids.insert(out.back().id).second)
      throw std::runtime_error("two requirements share id " +
                               std::to_string(out.back().id));
  }
  return out;
}

json ListToJson(const std::vector<Requirement>& list) {
  json a = json::array();
  for (const Requirement& r : list)
    a.push_back(ToJson(r));
  return a;
}

bool Holds(const Bound& b, double value, double limit, double limitMin,
           double limitMax) {
  if (!std::isfinite(value))
    return false;
  if (b.kind == "at_most")
    return value <= limit;
  if (b.kind == "at_least")
    return value >= limit;
  if (b.kind == "within")
    return value >= limitMin && value <= limitMax;
  return false; // `pass` is a verdict, not a comparison
}

std::string StateOf(const Requirement& r, const std::string& unresolvedWhy,
                    uint64_t currentHash) {
  if (!unresolvedWhy.empty())
    return "unresolved";
  if (!r.evidence.taken || r.evidence.revision != r.revision)
    return "unchecked";
  if (r.evidence.inputHash != currentHash)
    return "stale";
  if (!r.evidence.error.empty())
    return "unresolved";
  return r.evidence.pass ? "pass" : "fail";
}

std::string Slug(const std::string& name) {
  std::string s;
  for (char c : name) {
    const unsigned char u = static_cast<unsigned char>(c);
    if (std::isalnum(u))
      s += static_cast<char>(std::tolower(u));
    else if (!s.empty() && s.back() != '-')
      s += '-';
  }
  while (!s.empty() && s.back() == '-')
    s.pop_back();
  if (s.size() > 40)
    s.resize(40);
  return s.empty() ? "requirement" : s;
}

namespace {

std::string Num(double v) {
  char buf[64];
  std::snprintf(buf, sizeof buf, "%.4g", v);
  return buf;
}

// One finding as a line: a string as it is, an object as its fields
// ("mate: shaft fit, status: fail, reason: ..."), names first.
std::string FindingText(const json& f) {
  if (f.is_string())
    return f.get<std::string>();
  if (!f.is_object())
    return f.dump();
  std::string out;
  const auto add = [&](const std::string& k, const json& v) {
    if (v.is_array() && v.empty())
      return;
    out += (out.empty() ? "" : ", ") + k + ": " +
           (v.is_string()         ? v.get<std::string>()
            : v.is_number_float() ? Num(v.get<double>())
                                  : v.dump());
  };
  for (const char* k : {"part", "mate", "name", "designation"})
    if (f.contains(k))
      add(k, f.at(k));
  for (const auto& [k, v] : f.items())
    if (k != "part" && k != "mate" && k != "name" && k != "designation")
      add(k, v);
  return out;
}

} // namespace

std::string BoundText(const json& row) {
  const json& b = row.at("bound");
  const std::string kind = b.value("kind", std::string());
  const std::string unit = row.value("unit", std::string());
  const auto side = [&](const char* key, const char* evaluated) {
    if (row.contains(evaluated))
      return Num(row.at(evaluated).get<double>());
    const json& v = b.at(key);
    return v.is_number() ? Num(v.get<double>()) : v.get<std::string>();
  };
  const std::string u = unit.empty() ? "" : " " + unit;
  if (kind == "at_most")
    return "<= " + side("value", "limit") + u;
  if (kind == "at_least")
    return ">= " + side("value", "limit") + u;
  if (kind == "within")
    return side("min", "limit_min") + " .. " + side("max", "limit_max") + u;
  return "passes";
}

std::vector<std::string> FindingLines(const json& row) {
  std::vector<std::string> lines;
  const std::string st = row.value("state", std::string());
  if (st == "unresolved") {
    lines.push_back(row.value("note", std::string("could not be measured")));
    return lines;
  }
  if (st != "fail")
    return lines;
  const std::string unit = row.value("unit", std::string());
  if (row.contains("value"))
    lines.push_back("measured " + Num(row.at("value").get<double>()) +
                    (unit.empty() ? "" : " " + unit) + ", must be " +
                    BoundText(row));
  const json detail = row.value("detail", json::object());
  if (!detail.is_object())
    return lines;
  for (const json& f : detail.value("findings", json::array()))
    lines.push_back(FindingText(f));
  // check_seat: per bearing.
  for (const json& b : detail.value("bearings", json::array()))
    for (const json& f : b.value("findings", json::array()))
      lines.push_back(b.value("designation", std::string("bearing")) + ": " +
                      FindingText(f));
  for (const char* k : {"reason", "note"})
    if (detail.contains(k) && detail.at(k).is_string())
      lines.push_back(detail.at(k).get<std::string>());
  // fit_class: the kind it came out as.
  if (detail.contains("kind") && detail.contains("status") &&
      !row.contains("value"))
    lines.push_back("the fit is " + FindingText(detail.at("kind")) + ", " +
                    FindingText(detail.at("status")));
  if (lines.empty())
    lines.push_back("the verdict failed: " + detail.dump());
  return lines;
}

} // namespace req
