// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/Literals.h"

#include "core/ElementOrigin.h"
#include "core/Expression.h"
#include "core/features/DatumPlaneFeature.h"
#include "core/features/FrameFeature.h"
#include "core/features/SketchFeature.h"
#include "core/io/Serialization.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <functional>
#include <map>
#include <optional>
#include <stdexcept>

using nlohmann::json;

namespace literals {

const char* QuantityName(Quantity q) {
  switch (q) {
  case Quantity::Length:
    return "length";
  case Quantity::Angle:
    return "angle";
  case Quantity::Count:
    return "count";
  case Quantity::Other:
    return "other";
  }
  return "length";
}

std::string ExactNumber(double v) {
  char buf[64];
  const auto r = std::to_chars(buf, buf + sizeof(buf), v);
  return std::string(buf, r.ptr);
}

namespace {

// A text no expression can be: a control character first. Each probe slot
// gets its own, so where it lands in the JSON says which slot it is.
const std::string kSentinel = "\x1fliteral-probe#";

std::vector<std::string> Split(const std::string& path) {
  std::vector<std::string> out;
  std::string seg;
  for (char c : path) {
    if (c == '/') {
      out.push_back(seg);
      seg.clear();
    } else {
      seg += c;
    }
  }
  out.push_back(seg);
  return out;
}

std::string Join(const std::vector<std::string>& segs) {
  std::string out;
  for (size_t i = 0; i < segs.size(); i++)
    out += (i ? "/" : "") + segs[i];
  return out;
}

bool IsIndex(const std::string& s) {
  return !s.empty() && std::all_of(s.begin(), s.end(),
                                   [](char c) { return c >= '0' && c <= '9'; });
}

bool EndsWith(const std::string& s, const std::string& tail) {
  return s.size() >= tail.size() &&
         s.compare(s.size() - tail.size(), tail.size(), tail) == 0;
}

// "/distance_expr" -> "distance", "/origin_expr/2" -> "origin/2",
// "/constraints/3/expr" -> "constraints/3". The number an `_expr` key
// drives is its sibling of the same name -- or of that name plus "_deg"
// where the number is spelled in degrees (a revolve's `angle_deg` beside
// `angle_expr`), which is the key a path must name.
std::string FieldPath(const json& root, const std::string& jsonPath) {
  std::vector<std::string> segs = Split(jsonPath.substr(1));
  if (!segs.empty() && segs.back() == "expr")
    segs.pop_back();
  const json* node = &root;
  for (std::string& s : segs) {
    const json* parent = node;
    const std::string key = s;
    if (EndsWith(s, "_expr")) {
      s.resize(s.size() - 5);
      if (parent->is_object() && !parent->contains(s) &&
          parent->contains(s + "_deg"))
        s += "_deg";
    }
    if (parent->is_object() && parent->contains(key))
      node = &(*parent)[key];
    else if (parent->is_array() && IsIndex(key) &&
             std::stoul(key) < parent->size())
      node = &(*parent)[std::stoul(key)];
  }
  return Join(segs);
}

void FindSentinels(const json& j, const std::string& path,
                   std::vector<std::pair<size_t, std::string>>& out) {
  if (j.is_object()) {
    for (auto it = j.begin(); it != j.end(); ++it)
      FindSentinels(it.value(), path + "/" + it.key(), out);
  } else if (j.is_array()) {
    for (size_t i = 0; i < j.size(); i++)
      FindSentinels(j[i], path + "/" + std::to_string(i), out);
  } else if (j.is_string()) {
    const std::string& s = j.get_ref<const std::string&>();
    if (s.rfind(kSentinel, 0) == 0)
      out.emplace_back(std::stoul(s.substr(kSentinel.size())), path);
  }
}

// Every VisitExprs slot of `f` that its JSON writes, as {slot, field path}.
std::vector<std::pair<size_t, std::string>> ProbeSlots(const Feature& f) {
  std::unique_ptr<Feature> probe = f.Clone();
  size_t k = 0;
  probe->VisitExprs([&](std::string& text, double&) {
    text = kSentinel + std::to_string(k++);
  });
  std::vector<std::pair<size_t, std::string>> found;
  const json probed = DocumentIO::FeatureToJson(*probe);
  FindSentinels(probed, "", found);
  for (auto& [slot, path] : found)
    path = FieldPath(probed, path);
  std::sort(found.begin(), found.end());
  return found;
}

Quantity QuantityOfName(const std::string& name) {
  if (name.find("angle") != std::string::npos ||
      name.find("rotation") != std::string::npos || EndsWith(name, "_deg") ||
      name == "deg")
    return Quantity::Angle;
  if (name == "count" || name == "turns")
    return Quantity::Count;
  return Quantity::Length;
}

const char* Axis(size_t i) {
  static const char* kAxes[] = {"x", "y", "z"};
  return i < 3 ? kAxes[i] : "";
}

// The words and the kind of a probe path, read off the feature's own JSON.
void Describe(const json& fj, const std::string& path, Field& out) {
  const std::vector<std::string> segs = Split(path);
  if (segs.size() == 2 && segs[0] == "constraints" && IsIndex(segs[1])) {
    const json& c = fj.at("constraints").at(std::stoul(segs[1]));
    const std::string kind = c.value("kind", std::string("dimension"));
    out.label = kind + " dimension";
    out.quantity = kind == "angle" ? Quantity::Angle : Quantity::Length;
    return;
  }
  if (segs.size() == 3 && segs[0] == "entities" && IsIndex(segs[1])) {
    const json& e = fj.at("entities").at(std::stoul(segs[1]));
    out.label = e.value("type", std::string("entity")) + " " + segs[2];
    out.quantity = QuantityOfName(segs[2]);
    return;
  }
  std::string name;
  std::string label;
  for (size_t i = 0; i < segs.size(); i++) {
    if (IsIndex(segs[i])) {
      label += std::string(" ") + Axis(std::stoul(segs[i]));
    } else {
      name = segs[i];
      label += (label.empty() ? "" : " ") + segs[i];
    }
  }
  out.label = label;
  out.quantity = QuantityOfName(name);
}

// Why a field must not carry an expression, read off the feature's JSON.
std::string NotDrivable(const json& fj, const std::string& path) {
  const std::string type = fj.value("type", std::string{});
  if (type == "Hole" && path == "diameter" && fj.contains("fit"))
    return "the hole's diameter comes from its fit (" +
           fj.at("fit").value("thread", std::string{}) + ")";
  if (type == "Hole" && path == "depth" && fj.value("through", false))
    return "a through hole has no depth";
  if (type == "Extrude" && path == "distance") {
    const std::string end = fj.value("end_mode", std::string("blind"));
    if (end == "through_all" || end == "up_to_face")
      return "the extrude ends " + end + ", so its distance is not used";
  }
  return {};
}

} // namespace

std::vector<Field> Fields(const Feature& f) {
  std::vector<Field> out;
  const json fj = DocumentIO::FeatureToJson(f);

  // The slots' current texts and values, read from a clone (VisitExprs is
  // non-const, and a sketch's angle round-trips through degrees).
  std::vector<std::pair<std::string, double>> slots;
  {
    std::unique_ptr<Feature> reader = f.Clone();
    reader->VisitExprs([&](std::string& text, double& value) {
      slots.emplace_back(text, value);
    });
  }
  for (const auto& [slot, path] : ProbeSlots(f)) {
    if (slot >= slots.size())
      continue;
    Field field;
    field.path = path;
    field.expr = slots[slot].first;
    field.value = slots[slot].second;
    Describe(fj, path, field);
    field.notDrivable = NotDrivable(fj, path);
    out.push_back(std::move(field));
  }

  // A sketch's entity coordinates can each take an expression; the ones
  // that do not have one yet are no VisitExprs slot, so list them here.
  if (const auto* sk = dynamic_cast<const SketchFeature*>(&f)) {
    std::set<uint32_t> constrained, generated;
    for (const sketch::Constraint& c : sk->constraints) {
      constrained.insert(c.a.entity);
      constrained.insert(c.b.entity);
      if (c.kind == sketch::ConstraintKind::Symmetric)
        constrained.insert(c.c.entity);
    }
    for (const sketch::DerivedOp& op : sk->derived)
      for (const sketch::DerivedOp::Inst& inst : op.instances)
        generated.insert(inst.genId);
    for (const sketch::ExternalGeom& e : sk->external)
      for (uint32_t id : e.genIds)
        generated.insert(id);
    for (size_t k = 0; k < sk->entities.size(); k++) {
      const sketch::Entity& e = sk->entities[k];
      const uint32_t id = sketch::EntityId(e);
      const auto exprs = sk->entityExprs.find(id);
      for (const std::string& name : sketch::EntityExprFields(e)) {
        if (exprs != sk->entityExprs.end()) {
          const auto it = exprs->second.find(name);
          if (it != exprs->second.end() && !it->second.empty())
            continue; // a probe slot, listed above
        }
        const std::optional<double> v = sketch::GetEntityField(e, name);
        if (!v)
          continue;
        Field field;
        field.path = "entities/" + std::to_string(k) + "/" + name;
        field.value = *v;
        Describe(fj, field.path, field);
        if (generated.contains(id))
          field.notDrivable = "generated by a pattern, mirror, copy or "
                              "projection in this sketch";
        else if (constrained.contains(id))
          field.notDrivable = "a constraint in this sketch fixes it; drive "
                              "the dimension instead";
        out.push_back(std::move(field));
      }
    }
  }
  return out;
}

bool SetFieldExpr(Feature& f, const std::string& path,
                  const std::string& text) {
  std::optional<size_t> slot;
  for (const auto& [k, p] : ProbeSlots(f))
    if (p == path)
      slot = k;
  if (slot) {
    size_t k = 0;
    f.VisitExprs([&](std::string& t, double&) {
      if (k++ == *slot)
        t = text;
    });
    return true;
  }
  auto* sk = dynamic_cast<SketchFeature*>(&f);
  const std::vector<std::string> segs = Split(path);
  if (sk == nullptr || segs.size() != 3 || segs[0] != "entities" ||
      !IsIndex(segs[1]))
    return false;
  const size_t k = std::stoul(segs[1]);
  if (k >= sk->entities.size())
    return false;
  const std::vector<std::string> names =
      sketch::EntityExprFields(sk->entities[k]);
  if (std::find(names.begin(), names.end(), segs[2]) == names.end())
    return false;
  sk->entityExprs[sketch::EntityId(sk->entities[k])][segs[2]] = text;
  return true;
}

std::vector<std::string> FieldsWithoutTwin(const Feature& f) {
  const json fj = DocumentIO::FeatureToJson(f);
  std::set<std::string> twinned;
  for (const Field& field : Fields(f))
    twinned.insert(Split(field.path).front());
  std::vector<std::string> out;
  for (auto it = fj.begin(); it != fj.end(); ++it) {
    const std::string& key = it.key();
    if (twinned.contains(key) || key == "id" || EndsWith(key, "_id") ||
        EndsWith(key, "_ids") || EndsWith(key, "_ref"))
      continue;
    const json& v = it.value();
    const bool number = v.is_number() && !v.is_boolean();
    const bool vector = v.is_array() && !v.empty() &&
                        std::all_of(v.begin(), v.end(), [](const json& e) {
                          return e.is_number();
                        });
    if (number || vector)
      out.push_back(key);
  }
  return out;
}

// ---- variables ------------------------------------------------------------

namespace {

// Whether `text` is `name` alone, up to parentheses, spaces and a sign.
bool IsBare(const std::string& text, const std::string& name) {
  std::string stripped;
  for (char c : text)
    if (c != ' ' && c != '(' && c != ')' && c != '-' && c != '+')
      stripped += c;
  return stripped == name;
}

// Whether the expression multiplies or divides two NAMES — `teeth *
// module` — which says nothing about either one's kind on its own.
bool MultipliesNames(const std::string& text, size_t names) {
  return names > 1 && (text.find('*') != std::string::npos ||
                       text.find('/') != std::string::npos ||
                       text.find('^') != std::string::npos);
}

std::string Lower(std::string s) {
  for (char& c : s)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

// What a variable's comment says it measures. Generators and people write
// the unit first ("mm, the socket in front of the axis", "N, a fingertip
// pull"); a ratio or a factor says so in words.
std::optional<Quantity> CommentKind(const std::string& comment) {
  size_t end = 0;
  while (end < comment.size() &&
         std::string(" ,:;(").find(comment[end]) == std::string::npos)
    end++;
  const std::string unit = comment.substr(0, end);
  static const std::set<std::string> kLength = {"mm",   "cm", "m", "in",
                                                "inch", "ft", "um"};
  static const std::set<std::string> kAngle = {"deg", "degrees", "degree",
                                               "rad"};
  static const std::set<std::string> kOther = {
      "N",     "kN",  "N.m", "N.mm", "Nm",  "Nmm",   "N/mm",  "MPa",
      "GPa",   "kPa", "Pa",  "kg",   "g",   "g/cm3", "kg/m3", "s",
      "ms",    "Hz",  "rpm", "W",    "V",   "A",     "J",     "%",
      "N/mm2", "N/m", "lb",  "lbf",  "psi", "K",     "C",     "degC"};
  if (kLength.contains(unit))
    return Quantity::Length;
  if (kAngle.contains(unit))
    return Quantity::Angle;
  if (kOther.contains(unit))
    return Quantity::Other;
  const std::string c = Lower(comment);
  for (const char* word : {"ratio", "factor", "fraction", "coefficient",
                           "in modules", "per cent", "percent"})
    if (c.find(word) != std::string::npos)
      return Quantity::Count;
  return std::nullopt;
}

} // namespace

std::vector<Variable>
VariablesInScope(const Document& doc,
                 const std::vector<Parameter>* projectParams,
                 const std::vector<const Document*>& uses) {
  std::vector<Variable> out;
  const auto add = [&](const Parameter& p, const char* scope) {
    Variable v{p.name, scope, p.value, {}};
    if (const std::optional<Quantity> q = CommentKind(p.comment))
      v.kinds.insert(*q);
    out.push_back(std::move(v));
  };
  for (const Parameter& p : doc.GetParameters())
    add(p, "part");
  if (projectParams != nullptr)
    for (const Parameter& p : *projectParams)
      if (doc.FindParameter(p.name) == nullptr)
        add(p, "project");

  std::map<std::string, Variable*> byName;
  for (Variable& v : out)
    byName[v.name] = &v;
  const ExpressionEngine& engine = ExpressionEngine::Default();
  for (const Document* user : uses) {
    if (user == nullptr)
      continue;
    for (const auto& f : user->GetFeatures()) {
      for (const Field& field : Fields(*f)) {
        if (field.expr.empty())
          continue;
        const auto compiled = engine.Compile(field.expr);
        if (!compiled)
          continue;
        const std::set<std::string>& deps = (*compiled)->Dependencies();
        for (const std::string& dep : deps) {
          const auto it = byName.find(dep);
          if (it == byName.end())
            continue;
          // A part variable is only THIS part's: another part's same name
          // is its own variable.
          if (it->second->scope == "part" && user != &doc)
            continue;
          if (user != &doc && user->FindParameter(dep) != nullptr)
            continue; // shadowed there by that part's own
          const bool bare = IsBare(field.expr, dep);
          if (bare || (field.quantity == Quantity::Length &&
                       !MultipliesNames(field.expr, deps.size())))
            it->second->kinds.insert(field.quantity);
        }
      }
    }
  }
  for (Variable& v : out) {
    const std::string n = Lower(v.name);
    if (n.find("angle") != std::string::npos || EndsWith(n, "_deg") ||
        n.find("deg_") != std::string::npos)
      v.kinds.insert(Quantity::Angle);
    if (n.find("count") != std::string::npos ||
        n.find("teeth") != std::string::npos || n.rfind("n_", 0) == 0 ||
        n.rfind("num_", 0) == 0)
      v.kinds.insert(Quantity::Count);
  }
  return out;
}

bool Matches(const Variable& var, Quantity q, double v) {
  if (std::abs(v) < 1e-9 || std::abs(var.value) < 1e-9)
    return false; // a zero means nothing in particular
  if (var.kinds.empty() ? q != Quantity::Length : !var.kinds.contains(q))
    return false;
  const double tol =
      q == Quantity::Count ? 1e-9 : 1e-6 + 1e-9 * std::abs(var.value);
  return std::abs(std::abs(v) - std::abs(var.value)) <= tol;
}

// ---- placement ------------------------------------------------------------

namespace {

struct World {
  bool world = false;
  bool exprDriven = false; // its offset follows a variable: not floating
  std::string on;
};

std::string Quoted(const std::string& kind, const Feature& f) {
  return kind + " '" + f.GetName() + "'";
}

World FramePlacement(const Document& doc, const FrameFeature& fr, int depth);

World DatumPlacement(const Document& doc, const DatumPlaneFeature& dp,
                     int depth) {
  World w{true, false, Quoted("datum plane", dp)};
  if (!dp.offsetExpr.empty() || !dp.angleExpr.empty())
    w.exprDriven = true;
  if (dp.kind != DatumPlaneKind::ThroughPoints && dp.parentId != 0 &&
      depth < 32)
    if (const auto* parent = dynamic_cast<const DatumPlaneFeature*>(
            doc.FindFeatureById(dp.parentId)))
      w.exprDriven =
          w.exprDriven || DatumPlacement(doc, *parent, depth + 1).exprDriven;
  return w;
}

World FramePlacement(const Document& doc, const FrameFeature& fr, int depth) {
  World w;
  if (fr.kind == FrameKind::FromElement)
    return w; // attached to the model
  w.on = Quoted("frame", fr);
  if (fr.kind == FrameKind::Explicit) {
    w.world = true;
    for (const std::string& e : fr.originExpr)
      w.exprDriven = w.exprDriven || !e.empty();
    return w;
  }
  // Offset: wherever its parent is, moved by its own numbers.
  const auto* parent =
      dynamic_cast<const FrameFeature*>(doc.FindFeatureById(fr.parentId));
  if (parent == nullptr || depth >= 32)
    return World{};
  World pw = FramePlacement(doc, *parent, depth + 1);
  if (!pw.world)
    return World{};
  w.world = true;
  w.exprDriven = pw.exprDriven;
  for (int i = 0; i < 3; i++)
    w.exprDriven = w.exprDriven || !fr.offsetExpr[i].empty() ||
                   !fr.rotationExpr[i].empty();
  return w;
}

// "Top", or "the plane z = 28.7" for a baked plane parallel to one.
std::string BakedPlaneName(const SketchPlane& plane) {
  const glm::dvec3 n = plane.GetNormal();
  const glm::dvec3 o = plane.GetOrigin();
  static const char* kNames[] = {"Right", "Front", "Top"};
  static const char* kAxes[] = {"x", "y", "z"};
  for (int a = 0; a < 3; a++) {
    if (std::abs(std::abs(n[a]) - 1.0) > 1e-9)
      continue;
    const double at = o[a];
    if (std::abs(at) < 1e-9)
      return kNames[a];
    return std::string("the plane ") + kAxes[a] + " = " + ExactNumber(at);
  }
  return "a fixed plane";
}

World SketchPlacement(const Document& doc, const SketchFeature& sk, int depth) {
  if (sk.faceRef.IsSet())
    return World{};
  if (sk.planeRef == 0)
    return World{true, false, BakedPlaneName(sk.plane)};
  const Feature* ref = doc.FindFeatureById(sk.planeRef);
  if (const auto* dp = dynamic_cast<const DatumPlaneFeature*>(ref))
    return DatumPlacement(doc, *dp, 0);
  if (const auto* fr = dynamic_cast<const FrameFeature*>(ref))
    return FramePlacement(doc, *fr, 0);
  if (const auto* other = dynamic_cast<const SketchFeature*>(ref);
      other != nullptr && depth < 32) {
    World w = SketchPlacement(doc, *other, depth + 1);
    if (w.world)
      w.on = Quoted("the plane of sketch", *other);
    return w;
  }
  return World{};
}

// The UV box a sketch's geometry spans (conservative for curves).
bool SketchFootprint(const SketchFeature& sk, glm::dvec2& lo, glm::dvec2& hi) {
  bool any = false;
  const auto add = [&](glm::dvec2 p, double r = 0.0) {
    const glm::dvec2 a = p - glm::dvec2(r), b = p + glm::dvec2(r);
    lo = any ? glm::min(lo, a) : a;
    hi = any ? glm::max(hi, b) : b;
    any = true;
  };
  for (const sketch::Entity& e : sk.entities)
    std::visit(
        [&](const auto& g) {
          using T = std::decay_t<decltype(g)>;
          if (g.construction)
            return;
          if constexpr (std::is_same_v<T, sketch::Point>)
            add(g.p);
          else if constexpr (std::is_same_v<T, sketch::Line>) {
            add(g.a);
            add(g.b);
          } else if constexpr (std::is_same_v<T, sketch::Circle> ||
                               std::is_same_v<T, sketch::Arc>)
            add(g.center, g.radius);
          else if constexpr (std::is_same_v<T, sketch::Ellipse> ||
                             std::is_same_v<T, sketch::EllipseArc>)
            add(g.center, g.rMajor);
          else if constexpr (std::is_same_v<T, sketch::BSpline>) {
            for (const glm::dvec2& p : g.poles)
              add(p);
          } else if constexpr (std::is_same_v<T, sketch::Text>)
            add(g.anchor);
        },
        e);
  return any;
}

bool Parallel(const glm::dvec3& a, const glm::dvec3& b) {
  return glm::length(glm::cross(glm::normalize(a), glm::normalize(b))) < 1e-7;
}

// A face lying in a base plane THROUGH THE ORIGIN: the part's own primary
// datum, which whatever made the face measured from — as the subject does.
bool OnOriginPlane(const glm::dvec3& point, const glm::dvec3& normal) {
  for (int a = 0; a < 3; a++)
    if (std::abs(std::abs(normal[a]) - 1.0) < 1e-9 && std::abs(point[a]) < 1e-6)
      return true;
  return false;
}

constexpr double kOnPlane = 1e-5; // mm

// The name a face of the FINAL bodies had when feature `limit` ran, or
// nullopt when it did not exist yet: its seed and every G/S step (which make
// a face) must come from earlier features, and the M steps of later ones
// (which only modify it) are dropped
// (docs/modeling/toponaming.md#naming-grammar). A pattern instance or a
// multi-part name is not unpicked: nullopt.
std::optional<std::string>
NameBefore(const std::string& name,
           const std::function<bool(uint64_t)>& earlier) {
  // A piece qualifier's neighbours carry '#' and ',' of their own.
  const std::string bare = topo::WithoutQualifiers(name);
  if (bare.find('#') != std::string::npos ||
      bare.find(',') != std::string::npos)
    return std::nullopt;
  const auto tagOf = [](const std::string& s, size_t from) -> uint64_t {
    uint64_t v = 0;
    size_t i = from;
    for (; i < s.size() && s[i] >= '0' && s[i] <= '9'; i++)
      v = v * 10 + static_cast<uint64_t>(s[i] - '0');
    return i == from ? 0 : v;
  };
  const std::vector<std::string> steps = topo::NameSteps(name);
  const std::string& seed = steps.front();
  const uint64_t seedTag = tagOf(seed, 0);
  if (seedTag == 0 || !earlier(seedTag))
    return std::nullopt;
  std::string out = seed;
  for (size_t s = 1; s < steps.size(); s++) {
    const std::string& step = steps[s];
    if (step.empty())
      continue;
    const char kind = step[0];
    if (kind == 'M' || kind == 'G' || kind == 'S') {
      const uint64_t tag = tagOf(step, 1);
      if (!earlier(tag)) {
        if (kind != 'M')
          return std::nullopt; // made after it
        break;                 // modified after it: the name it had then
      }
    }
    out += ";" + step;
  }
  return out;
}

} // namespace

Report Lint(const Document& doc, const std::vector<Variable>& variables) {
  Report report;
  const auto& features = doc.GetFeatures();

  // Literals equal to a variable in scope.
  for (const auto& f : features) {
    for (Field& field : Fields(*f)) {
      if (!field.expr.empty() || !field.notDrivable.empty())
        continue;
      report.fieldsChecked++;
      std::vector<std::string> names;
      for (const Variable& v : variables)
        if (Matches(v, field.quantity, field.value))
          names.push_back((field.value < 0) != (v.value < 0) ? "-" + v.name
                                                             : v.name);
      if (!names.empty())
        report.literals.push_back(
            {f->GetId(), f->GetName(), std::move(field), std::move(names)});
    }
  }

  // Placements in the world where a face of the part already lies.
  std::map<uint64_t, size_t> indexOf;
  for (size_t i = 0; i < features.size(); i++)
    indexOf[features[i]->GetId()] = i;
  const auto before = [&](uint64_t id, size_t limit) {
    const auto it = indexOf.find(id);
    return it != indexOf.end() && it->second < limit;
  };
  for (size_t i = 1; i < features.size(); i++) {
    const Feature* f = features[i].get();
    if (doc.EffectiveSuppressed(i) || !f->GetError().empty())
      continue;
    World world;
    glm::dvec3 origin, normal, uAxis, vAxis;
    glm::dvec2 lo{0.0}, hi{0.0};
    if (const auto* sk = dynamic_cast<const SketchFeature*>(f)) {
      world = SketchPlacement(doc, *sk, 0);
      if (!world.world || world.exprDriven || !SketchFootprint(*sk, lo, hi))
        continue;
      origin = sk->plane.GetOrigin();
      normal = sk->plane.GetNormal();
      uAxis = sk->plane.GetUAxis();
      vAxis = sk->plane.GetVAxis();
    } else if (const auto* fr = dynamic_cast<const FrameFeature*>(f)) {
      world = FramePlacement(doc, *fr, 0);
      if (!world.world || world.exprDriven)
        continue;
      origin = fr->Origin();
      normal = fr->ZAxis();
      uAxis = fr->XAxis();
      vAxis = fr->YAxis();
    } else {
      continue;
    }
    if (glm::length(normal) < 1e-12 || OnOriginPlane(origin, normal))
      continue;
    report.placementsChecked++;

    // The faces it could have been attached to: the bodies as they stood
    // when it ran. The regen cache has them unless the part was opened
    // from baked geometry, which carries only the tip; then the final
    // bodies, keeping only faces every feature named in had run by then.
    const std::vector<Body>* cached = doc.CachedBodiesAfter(i - 1);
    const bool useCache = cached != nullptr && !cached->empty();
    const std::vector<Body>& bodies = useCache ? *cached : doc.GetBodies();

    std::optional<PlacementFinding> hit;
    for (const Body& body : bodies) {
      if (body.reference || hit)
        continue;
      const int faces = body.shape.FaceCount();
      for (int fi = 0; fi < faces && !hit; fi++) {
        const std::string* name = body.elementMap.NameOf(fi);
        if (name == nullptr)
          continue;
        glm::dvec3 c, n, u;
        if (!body.shape.FacePlane(fi, c, n, u) || !Parallel(n, normal) ||
            std::abs(glm::dot(c - origin, glm::normalize(normal))) > kOnPlane)
          continue;
        std::string faceName = *name;
        if (!useCache) {
          if (!before(body.createdBy, i))
            continue;
          const std::optional<std::string> then =
              NameBefore(*name, [&](uint64_t id) { return before(id, i); });
          if (!then)
            continue;
          faceName = *then;
        }
        // In the plane its own sketch was drawn on (an extrude's start cap
        // over a sketch on this same plane): there by construction.
        const topo::ElementOrigin eo = topo::DecodeElementName(faceName);
        bool byConstruction = false;
        if (const Feature* maker =
                eo.derived.empty() ? doc.FindFeatureById(eo.madeBy) : nullptr)
          maker->VisitFeatureRefs([&](uint64_t ref) {
            const auto* s =
                dynamic_cast<const SketchFeature*>(doc.FindFeatureById(ref));
            if (s != nullptr && SketchPlacement(doc, *s, 0).world &&
                Parallel(s->plane.GetNormal(), n) &&
                std::abs(glm::dot(c - s->plane.GetOrigin(),
                                  glm::normalize(s->plane.GetNormal()))) <=
                    kOnPlane)
              byConstruction = true;
          });
        if (byConstruction)
          continue;
        // Over the face, not merely in its plane.
        glm::dvec2 flo, fhi;
        if (!body.shape.FaceExtent(fi, origin, uAxis, vAxis, flo, fhi))
          continue;
        if (hi.x < flo.x - kOnPlane || lo.x > fhi.x + kOnPlane ||
            hi.y < flo.y - kOnPlane || lo.y > fhi.y + kOnPlane)
          continue;
        PlacementFinding p;
        p.featureId = f->GetId();
        p.feature = f->GetName();
        p.on = world.on;
        p.origin = origin;
        p.normal = normal;
        p.face = {body.createdBy, faceName};
        p.faceMadeBy = eo.madeBy;
        if (const Feature* maker = doc.FindFeatureById(eo.madeBy))
          p.faceFeature = maker->GetName();
        hit = std::move(p);
      }
    }
    if (hit)
      report.placements.push_back(std::move(*hit));
  }
  return report;
}

// ---- extract_variables ----------------------------------------------------

namespace {

std::string Sanitize(const std::string& s) {
  std::string out;
  bool sep = false;
  for (char ch : s) {
    const unsigned char c = static_cast<unsigned char>(ch);
    if (std::isalnum(c)) {
      if (sep && !out.empty())
        out += '_';
      out += static_cast<char>(std::tolower(c));
      sep = false;
    } else {
      sep = true;
    }
  }
  return out;
}

// The part of a field a NAME is made from: "distance", "origin_z",
// "diameter" (a sketch dimension's kind), "r" (an entity's).
std::string NamePart(const Field& field) {
  const std::vector<std::string> segs = Split(field.path);
  if (segs.front() == "constraints") {
    std::string kind = field.label;
    const size_t sp = kind.rfind(" dimension");
    if (sp != std::string::npos)
      kind.resize(sp);
    return Sanitize(kind);
  }
  if (segs.front() == "entities")
    return Sanitize(segs.back());
  return Sanitize(field.label);
}

} // namespace

ExtractPlan PlanExtract(const Document& doc,
                        const std::vector<Variable>& variables,
                        const ExtractOptions& options) {
  const ExpressionEngine& engine = ExpressionEngine::Default();
  std::vector<const Feature*> chosen;
  if (options.featureIds.empty()) {
    for (const auto& f : doc.GetFeatures())
      chosen.push_back(f.get());
  } else {
    for (uint64_t id : options.featureIds) {
      const Feature* f = doc.FindFeatureById(id);
      if (f == nullptr) {
        std::string known;
        for (const auto& g : doc.GetFeatures())
          known += (known.empty() ? "" : ", ") + std::to_string(g->GetId()) +
                   " '" + g->GetName() + "'";
        throw std::runtime_error("no feature with id " + std::to_string(id) +
                                 " in this part (it "
                                 "has: " +
                                 (known.empty() ? std::string("none") : known) +
                                 ")");
      }
      chosen.push_back(f);
    }
  }

  // Every named path must exist on one of the chosen features.
  std::vector<std::vector<Field>> fields;
  std::set<std::string> allPaths;
  for (const Feature* f : chosen) {
    fields.push_back(Fields(*f));
    for (const Field& fd : fields.back())
      allPaths.insert(fd.path);
  }
  const std::set<std::string> wanted(options.paths.begin(),
                                     options.paths.end());
  for (const std::string& p : wanted)
    if (!allPaths.contains(p)) {
      std::string known;
      for (const std::string& k : allPaths)
        known += (known.empty() ? "" : ", ") + k;
      throw std::runtime_error(
          "no field '" + p + "' on " +
          (options.featureIds.empty() ? std::string("any feature")
                                      : std::string("the features named")) +
          " (fields: " + (known.empty() ? std::string("none") : known) +
          "; check_literals and a dry run list them)");
    }

  // A prefix no variable name can start with is refused here: the loop
  // below appended _2, _3, ... to a name that stays invalid whatever the
  // suffix, and `prefix: "1/0"` hung the app (hostile_params_smoke).
  if (!options.prefix.empty() &&
      !engine.IsValidParameterName(options.prefix + "x") &&
      !engine.IsValidParameterName("v_" + options.prefix + "x"))
    throw std::runtime_error(
        "prefix '" + options.prefix +
        "' cannot begin a variable name: use letters, digits and _ (a name "
        "starts with a letter or _)");
  ExtractPlan plan;
  std::set<std::string> taken = options.taken;
  const auto freshName = [&](std::string base) {
    if (base.empty())
      base = "value";
    base = options.prefix + base;
    if (!engine.IsValidParameterName(base))
      base = "v_" + base;
    std::string name = base;
    for (int n = 2; taken.contains(name) || !engine.IsValidParameterName(name);
         n++)
      name = base + "_" + std::to_string(n);
    taken.insert(name);
    return name;
  };

  struct Group {
    size_t proposal;
    uint64_t scope; // the feature, or 0 when merging across features
  };
  std::vector<Group> groups;
  std::map<std::string, size_t> existingProposal;

  for (size_t fi = 0; fi < chosen.size(); fi++) {
    const Feature* f = chosen[fi];
    for (const Field& field : fields[fi]) {
      if (!wanted.empty() && !wanted.contains(field.path))
        continue;
      if (!field.expr.empty())
        continue; // already an expression
      LeftAlone skip{f->GetId(),  f->GetName(), field.path, field.label,
                     field.value, true,         {}};
      if (!field.notDrivable.empty()) {
        skip.reason = field.notDrivable;
      } else if (field.quantity == Quantity::Count) {
        skip.reason = "a unitless count";
      } else if (std::abs(field.value) < 1e-12) {
        skip.reason = "zero: there is nothing to name";
      } else if (field.quantity == Quantity::Angle &&
                 std::abs(std::abs(field.value) - 360.0) < 1e-9) {
        skip.reason = "a full turn";
      }
      if (!skip.reason.empty()) {
        plan.leftAlone.push_back(std::move(skip));
        continue;
      }
      Use use{f->GetId(), f->GetName(), field, {}};

      if (options.reuse) {
        std::vector<const Variable*> same;
        for (const Variable& v : variables)
          if (Matches(v, field.quantity, field.value))
            same.push_back(&v);
        if (same.size() > 1) {
          std::string names;
          for (const Variable* v : same)
            names += (names.empty() ? "" : ", ") + v->name;
          skip.reason = "equals several variables (" + names +
                        "): bind one by hand with modify_feature";
          plan.leftAlone.push_back(std::move(skip));
          continue;
        }
        if (same.size() == 1) {
          const Variable& v = *same.front();
          use.expression =
              (field.value < 0) != (v.value < 0) ? "-" + v.name : v.name;
          auto it = existingProposal.find(v.name);
          if (it == existingProposal.end()) {
            Proposal p;
            p.name = v.name;
            p.value = v.value;
            p.quantity = field.quantity;
            p.existing = true;
            plan.proposals.push_back(std::move(p));
            it = existingProposal.emplace(v.name, plan.proposals.size() - 1)
                     .first;
          }
          plan.proposals[it->second].uses.push_back(std::move(use));
          continue;
        }
      }

      const uint64_t scope = options.mergeEqual ? 0 : f->GetId();
      std::optional<size_t> joined;
      for (const Group& g : groups) {
        const Proposal& p = plan.proposals[g.proposal];
        if (g.scope == scope && p.quantity == field.quantity &&
            std::abs(std::abs(p.value) - std::abs(field.value)) <=
                1e-12 * std::max(1.0, std::abs(p.value)))
          joined = g.proposal;
      }
      if (joined) {
        Proposal& p = plan.proposals[*joined];
        use.expression =
            (field.value < 0) != (p.value < 0) ? "-" + p.name : p.name;
        p.uses.push_back(std::move(use));
        continue;
      }
      Proposal p;
      p.name = freshName(Sanitize(f->GetName()) + "_" + NamePart(field));
      p.value = field.value;
      p.expression = ExactNumber(field.value);
      p.quantity = field.quantity;
      use.expression = p.name;
      p.uses.push_back(std::move(use));
      plan.proposals.push_back(std::move(p));
      groups.push_back({plan.proposals.size() - 1, scope});
    }
    if (wanted.empty())
      for (const std::string& key : FieldsWithoutTwin(*f)) {
        LeftAlone skip{f->GetId(), f->GetName(),        key, key, 0.0,
                       false,      "no expression twin"};
        plan.leftAlone.push_back(std::move(skip));
      }
  }
  return plan;
}

} // namespace literals
