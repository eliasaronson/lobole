// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/ExprValue.h"

#include <algorithm>

#include <glm/trigonometric.hpp>
#include <nlohmann/json.hpp>

#include <cmath>
#include <stdexcept>

const char* ExprQuantityName(ExprQuantity q) {
  switch (q) {
  case ExprQuantity::Length:
    return "length";
  case ExprQuantity::Angle:
    return "angle";
  case ExprQuantity::Count:
    return "count";
  case ExprQuantity::Other:
    return "other";
  }
  return "other";
}

ExprQuantity QuantityOfFieldName(const std::string& name) {
  const auto endsWith = [&](const std::string& tail) {
    return name.size() >= tail.size() &&
           name.compare(name.size() - tail.size(), tail.size(), tail) == 0;
  };
  if (name.find("angle") != std::string::npos ||
      name.find("rotation") != std::string::npos || endsWith("_deg") ||
      name == "deg")
    return ExprQuantity::Angle;
  if (name == "count" || name == "turns")
    return ExprQuantity::Count;
  return ExprQuantity::Length;
}

ExprAngle ExprAngle::FromDegrees(double deg) {
  ExprAngle a;
  a.value = glm::radians(deg);
  return a;
}

double ExprAngle::Degrees() const { return glm::degrees(value); }

ExprSlot::ExprSlot(ExprValue& v, ExprQuantity q, std::string path)
    : m_expr(&v.expr), m_double(&v.value), m_quantity(q),
      m_path(std::move(path)) {}

ExprSlot::ExprSlot(ExprInt& v, std::string path)
    : m_expr(&v.expr), m_int(&v.value), m_quantity(ExprQuantity::Count),
      m_path(std::move(path)) {}

ExprSlot::ExprSlot(ExprAngle& v, std::string path)
    : m_expr(&v.expr), m_double(&v.value), m_radians(true),
      m_quantity(ExprQuantity::Angle), m_path(std::move(path)) {}

ExprSlot::ExprSlot(std::string& expr, double& value, ExprQuantity q,
                   std::string path)
    : m_expr(&expr), m_double(&value), m_quantity(q), m_path(std::move(path)) {}

ExprSlot ExprSlot::Radians(std::string& expr, double& radians,
                           std::string path) {
  ExprSlot s;
  s.m_expr = &expr;
  s.m_double = &radians;
  s.m_radians = true;
  s.m_quantity = ExprQuantity::Angle;
  s.m_path = std::move(path);
  return s;
}

double ExprSlot::Value() const {
  if (m_int != nullptr)
    return static_cast<double>(*m_int);
  return m_radians ? glm::degrees(*m_double) : *m_double;
}

bool ExprSlot::Set(double v, std::string* why) const {
  if (m_int != nullptr) {
    // A count is whole. Rounding 2.5 would give a pattern a number of copies
    // nobody wrote, so a fractional result is the expression's error.
    const double r = std::round(v);
    if (!std::isfinite(v) || std::abs(v - r) > 1e-9 ||
        std::abs(r) > 2147483647.0) {
      if (why != nullptr)
        *why = "a count is a whole number, and '" + *m_expr + "' is " +
               std::to_string(v);
      return false;
    }
    *m_int = static_cast<int>(r);
    return true;
  }
  *m_double = m_radians ? glm::radians(v) : v;
  return true;
}

bool EvaluateSlot(const ExprSlot& slot, const SymbolTable& symbols,
                  std::string* why) {
  if (!slot.Bound())
    return true;
  const auto v = ExpressionEngine::Default().EvaluateText(slot.Expr(), symbols);
  if (!v.has_value()) {
    if (why != nullptr)
      *why = v.error().message;
    return false;
  }
  return slot.Set(*v, why);
}

bool SlotReads(const ExprSlot& slot, const std::string& name) {
  if (!slot.Bound())
    return false;
  const auto compiled = ExpressionEngine::Default().Compile(slot.Expr());
  return compiled.has_value() && (*compiled)->Dependencies().contains(name);
}

bool RenameInSlot(const ExprSlot& slot, const std::string& from,
                  const std::string& to) {
  if (!slot.Bound())
    return false;
  std::string next =
      ExpressionEngine::Default().RenameIdentifier(slot.Expr(), from, to);
  if (next == slot.Expr())
    return false;
  slot.Expr() = std::move(next);
  return true;
}

// ---- JSON -------------------------------------------------------------------

void to_json(nlohmann::json& j, const ExprValue& v) { j = v.value; }
void to_json(nlohmann::json& j, const ExprInt& v) { j = v.value; }

void WriteExprText(nlohmann::json& j, const std::string& key,
                   const std::string& expr) {
  if (!expr.empty())
    j[key + "_expr"] = expr;
}

void WriteExpr(nlohmann::json& j, const std::string& key, const ExprValue& v) {
  j[key] = v.value;
  WriteExprText(j, key, v.expr);
}

void WriteExpr(nlohmann::json& j, const std::string& key, const ExprInt& v) {
  j[key] = v.value;
  WriteExprText(j, key, v.expr);
}

void WriteExpr(nlohmann::json& j, const std::string& key, const ExprAngle& v) {
  j[key] = v.Degrees();
  WriteExprText(j, key, v.expr);
}

std::string ExprTextFromJson(const nlohmann::json& t, const std::string& what) {
  if (!t.is_string())
    throw std::runtime_error(what + " is an expression (text), not " +
                             t.dump());
  // Kept even when it does not parse: the evaluation says so, on whatever
  // holds it, and the rest of the file opens (ReadExpr).
  return t.get<std::string>();
}

void RefuseUnreadableExpr(const std::string& text, const std::string& what) {
  if (text.empty())
    return;
  const auto compiled = ExpressionEngine::Default().Compile(text);
  if (!compiled)
    throw std::runtime_error(what + " '" + text +
                             "' does not read: " + compiled.error().message);
}

std::string ReadExprText(const nlohmann::json& j, const std::string& key) {
  const std::string k = key + "_expr";
  if (!j.is_object() || !j.contains(k))
    return {};
  return ExprTextFromJson(j.at(k), k);
}

namespace {

double NumberAt(const nlohmann::json& j, const std::string& key) {
  const nlohmann::json& n = j.at(key);
  if (!n.is_number())
    throw std::runtime_error(key + " is a number, not " + n.dump());
  return n.get<double>();
}

} // namespace

bool ReadExpr(const nlohmann::json& j, const std::string& key, ExprValue& v) {
  const bool has = j.is_object() && j.contains(key);
  if (has)
    v.value = NumberAt(j, key);
  v.expr = ReadExprText(j, key);
  return has || !v.expr.empty();
}

bool ReadExpr(const nlohmann::json& j, const std::string& key, ExprInt& v) {
  const bool has = j.is_object() && j.contains(key);
  if (has) {
    const nlohmann::json& n = j.at(key);
    if (!n.is_number_integer())
      throw std::runtime_error(key + " is a whole number, not " + n.dump());
    v.value = n.get<int>();
  }
  v.expr = ReadExprText(j, key);
  return has || !v.expr.empty();
}

bool ReadExpr(const nlohmann::json& j, const std::string& key, ExprAngle& v) {
  const bool has = j.is_object() && j.contains(key);
  if (has)
    v.value = glm::radians(NumberAt(j, key));
  v.expr = ReadExprText(j, key);
  return has || !v.expr.empty();
}

bool ReadExprParam(const nlohmann::json& p, const std::string& key,
                   ExprValue& v) {
  const bool hasKey = p.is_object() && p.contains(key);
  const bool hasExpr = p.is_object() && p.contains(key + "_expr");
  if (!hasKey && !hasExpr)
    return false;
  std::string text;
  if (hasKey) {
    const nlohmann::json& n = p.at(key);
    if (n.is_string()) {
      // The alias: an expression where the number goes.
      text = ExprTextFromJson(n, key);
      if (hasExpr)
        throw std::runtime_error(key + " is given twice: as the expression '" +
                                 n.get<std::string>() + "' and as " + key +
                                 "_expr");
    } else if (n.is_number()) {
      v.value = n.get<double>();
    } else {
      throw std::runtime_error(key + " is a number, or an expression (" + key +
                               "_expr), not " + n.dump());
    }
  }
  if (hasExpr)
    text = ReadExprText(p, key);
  RefuseUnreadableExpr(text, hasExpr ? key + "_expr" : key);
  v.expr = std::move(text);
  return true;
}

bool ReadExprParam(const nlohmann::json& p, const std::string& key,
                   ExprInt& v) {
  if (p.is_object() && p.contains(key) && p.at(key).is_number() &&
      !p.at(key).is_number_integer()) {
    const double d = p.at(key).get<double>();
    if (d != std::round(d))
      throw std::runtime_error(key + " is a whole number, not " +
                               p.at(key).dump());
  }
  ExprValue x(static_cast<double>(v.value));
  if (!ReadExprParam(p, key, x))
    return false;
  if (!x.Bound() || (p.is_object() && p.contains(key)))
    v.value = static_cast<int>(std::lround(x.value)); // whole, checked above
  v.expr = std::move(x.expr);
  return true;
}

bool ReadExprParam(const nlohmann::json& p, const std::string& key,
                   ExprAngle& v) {
  ExprValue x(v.Degrees());
  if (!ReadExprParam(p, key, x))
    return false;
  v.value = glm::radians(x.value);
  v.expr = std::move(x.expr);
  return true;
}

void WriteExprRadians(nlohmann::json& j, const std::string& key,
                      const ExprAngle& v) {
  j[key] = v.value;
  WriteExprText(j, key, v.expr);
}

bool ReadExprRadians(const nlohmann::json& j, const std::string& key,
                     ExprAngle& v) {
  const bool has = j.is_object() && j.contains(key);
  if (has)
    v.value = NumberAt(j, key);
  v.expr = ReadExprText(j, key);
  return has || !v.expr.empty();
}

void WriteExprTexts(nlohmann::json& j, const std::string& key,
                    const std::vector<std::string>& texts) {
  for (const std::string& t : texts)
    if (!t.empty()) {
      j[key + "_expr"] = texts;
      return;
    }
}

std::vector<std::string> ReadExprTexts(const nlohmann::json& j,
                                       const std::string& key, size_t n) {
  std::vector<std::string> out(n);
  const std::string k = key + "_expr";
  if (!j.is_object() || !j.contains(k))
    return out;
  const nlohmann::json& a = j.at(k);
  if (!a.is_array() || a.size() != n)
    throw std::runtime_error(k + " is " + std::to_string(n) +
                             " expression texts (\"\" for a number), not " +
                             a.dump());
  for (size_t i = 0; i < n; i++)
    out[i] = ExprTextFromJson(a[i], k);
  return out;
}

void VisitVec3Exprs(const ExprVisitor& visit, glm::dvec3& v, Vec3Exprs& texts,
                    ExprQuantity q, const std::string& path) {
  for (int k = 0; k < 3; k++)
    visit(ExprSlot(texts[static_cast<size_t>(k)], v[k], q,
                   path + "/" + std::to_string(k)));
}

void WriteVec3Exprs(nlohmann::json& j, const std::string& key,
                    const Vec3Exprs& texts) {
  if (AnyBound(texts))
    j[key + "_expr"] = {texts[0], texts[1], texts[2]};
}

Vec3Exprs ReadVec3Exprs(const nlohmann::json& j, const std::string& key) {
  const std::vector<std::string> t = ReadExprTexts(j, key, 3);
  return {t[0], t[1], t[2]};
}

namespace {

std::vector<std::string> SplitPath(const std::string& path) {
  std::vector<std::string> out;
  size_t from = 0;
  while (from <= path.size()) {
    const size_t slash = path.find('/', from);
    out.push_back(path.substr(from, slash - from));
    if (slash == std::string::npos)
      break;
    from = slash + 1;
  }
  return out;
}

bool IsIndex(const std::string& s) {
  return !s.empty() && std::all_of(s.begin(), s.end(),
                                   [](char c) { return c >= '0' && c <= '9'; });
}

// The object holding the slot's number, its key there, and the index into
// that key's array when the number is an entry of one (-1 when not).
template <class J>
J* Place(J& root, const std::vector<std::string>& parts, std::string& key,
         int& index) {
  size_t n = parts.size();
  index = -1;
  if (n >= 2 && IsIndex(parts[n - 1])) {
    index = std::stoi(parts[n - 1]);
    n--;
  }
  key = parts[n - 1];
  J* at = &root;
  for (size_t i = 0; i + 1 < n; i++) {
    if (IsIndex(parts[i])) {
      const size_t k = std::stoul(parts[i]);
      if (!at->is_array() || k >= at->size())
        return nullptr;
      at = &(*at)[k];
    } else {
      if (!at->is_object() || !at->contains(parts[i]))
        return nullptr;
      at = &(*at)[parts[i]];
    }
  }
  return at->is_object() ? at : nullptr;
}

} // namespace

void WriteSlotText(nlohmann::json& root, const ExprSlot& slot) {
  if (!slot.Bound())
    return;
  std::string key;
  int index = 0;
  nlohmann::json* obj = Place(root, SplitPath(slot.Path()), key, index);
  if (obj == nullptr || !obj->contains(key))
    return;
  if (index < 0) {
    (*obj)[key + "_expr"] = slot.Expr();
    return;
  }
  const nlohmann::json& numbers = obj->at(key);
  if (!numbers.is_array() || static_cast<size_t>(index) >= numbers.size())
    return;
  nlohmann::json& texts = (*obj)[key + "_expr"];
  if (!texts.is_array())
    texts = nlohmann::json::array();
  while (texts.size() < numbers.size())
    texts.push_back("");
  texts[static_cast<size_t>(index)] = slot.Expr();
}

void ReadSlotText(const nlohmann::json& root, const ExprSlot& slot) {
  std::string key;
  int index = 0;
  const nlohmann::json* obj = Place(root, SplitPath(slot.Path()), key, index);
  slot.Expr().clear();
  if (obj == nullptr || !obj->contains(key + "_expr"))
    return;
  const nlohmann::json& t = obj->at(key + "_expr");
  const std::string what = slot.Path() + "_expr";
  if (index < 0) {
    slot.Expr() = ExprTextFromJson(t, what);
    return;
  }
  const size_t n =
      obj->contains(key) && obj->at(key).is_array() ? obj->at(key).size() : 0;
  if (!t.is_array() || t.size() != n)
    throw std::runtime_error(key + "_expr is one text per entry of " + key +
                             " (" + std::to_string(n) + "), \"\" for a number");
  slot.Expr() = ExprTextFromJson(t[static_cast<size_t>(index)], what);
}

void WriteVec2Exprs(nlohmann::json& j, const std::string& key,
                    const Vec2Exprs& texts) {
  if (AnyBound(texts))
    j[key + "_expr"] = {texts[0], texts[1]};
}

Vec2Exprs ReadVec2Exprs(const nlohmann::json& j, const std::string& key) {
  const std::vector<std::string> t = ReadExprTexts(j, key, 2);
  return {t[0], t[1]};
}

void WritePointExprs(nlohmann::json& j, const std::string& key,
                     const std::vector<Vec3Exprs>& texts) {
  bool any = false;
  for (const Vec3Exprs& t : texts)
    any = any || AnyBound(t);
  if (!any)
    return;
  nlohmann::json a = nlohmann::json::array();
  for (const Vec3Exprs& t : texts)
    a.push_back({t[0], t[1], t[2]});
  j[key + "_expr"] = std::move(a);
}

std::vector<Vec3Exprs> ReadPointExprs(const nlohmann::json& j,
                                      const std::string& key, size_t n) {
  std::vector<Vec3Exprs> out(n);
  const std::string k = key + "_expr";
  if (!j.is_object() || !j.contains(k))
    return out;
  const nlohmann::json& a = j.at(k);
  if (!a.is_array() || a.size() != n)
    throw std::runtime_error(k + " is " + std::to_string(n) +
                             " [x, y, z] of expression texts (\"\" for a "
                             "number), not " +
                             a.dump());
  for (size_t i = 0; i < n; i++) {
    if (!a[i].is_array() || a[i].size() != 3)
      throw std::runtime_error(k + " entry " + std::to_string(i) +
                               " is [x, y, z] of texts, not " + a[i].dump());
    for (size_t c = 0; c < 3; c++)
      out[i][c] = ExprTextFromJson(a[i][c], k);
  }
  return out;
}
