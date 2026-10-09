// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "remote/RpcContract.h"

#include <cctype>
#include <cstdlib>
#include <stdexcept>

namespace remote::contract {

namespace {

std::string Trim(std::string_view s) {
  size_t b = 0, e = s.size();
  while (b < e && std::isspace(static_cast<unsigned char>(s[b])))
    b++;
  while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1])))
    e--;
  return std::string(s.substr(b, e - b));
}

std::vector<std::string> SplitTop(std::string_view s, char sep) {
  // Literals may hold anything but a quote, so split outside them only.
  std::vector<std::string> out;
  std::string cur;
  bool quoted = false;
  for (char c : s) {
    if (c == '\'')
      quoted = !quoted;
    if (c == sep && !quoted) {
      out.push_back(cur);
      cur.clear();
      continue;
    }
    cur += c;
  }
  out.push_back(cur);
  return out;
}

[[noreturn]] void Fail(const std::string& key, const std::string& why) {
  throw std::invalid_argument("contract key '" + key + "': " + why);
}

bool IsNumeric(Kind k) { return k == Kind::Int || k == Kind::Number; }

Alt ParseAlt(const std::string& key, std::string a) {
  Alt alt;
  if (a.empty())
    Fail(key, "an empty type in a union");
  if (a.front() == '\'') {
    if (a.size() < 2 || a.back() != '\'')
      Fail(key, "unterminated literal " + a);
    alt.kind = Kind::Literal;
    alt.text = a.substr(1, a.size() - 2);
    return alt;
  }
  if (a.front() == '@') {
    alt.kind = Kind::Vocab;
    alt.text = a.substr(1);
    if (alt.text.empty())
      Fail(key, "'@' names no vocabulary");
    for (char c : alt.text)
      if (!std::islower(static_cast<unsigned char>(c)) && c != '_')
        Fail(key, "vocabulary '" + alt.text + "' is not a describe key");
    return alt;
  }
  size_t i = 0;
  while (i < a.size() && std::isalpha(static_cast<unsigned char>(a[i])))
    i++;
  const std::string base = a.substr(0, i);
  static const std::pair<const char*, Kind> kBases[] = {
      {"int", Kind::Int},       {"number", Kind::Number},
      {"bool", Kind::Bool},     {"string", Kind::String},
      {"object", Kind::Object}, {"null", Kind::Null},
      {"any", Kind::Any},       {"revision", Kind::Revision}};
  bool known = false;
  for (const auto& [name, kind] : kBases)
    if (base == name) {
      alt.kind = kind;
      known = true;
    }
  if (!known)
    Fail(key, "unknown type '" + base +
                  "' (int, number, bool, string, object, null, any, "
                  "revision, 'literal', @vocabulary)");
  // Ranges on the scalar, then an optional [] / [N].
  while (i < a.size() && (a[i] == '>' || a[i] == '<')) {
    const bool lower = a[i] == '>';
    i++;
    const bool inclusive = i < a.size() && a[i] == '=';
    if (inclusive)
      i++;
    const char* start = a.c_str() + i;
    char* end = nullptr;
    const double v = std::strtod(start, &end);
    if (end == start)
      Fail(key, "a range bound must be a number in '" + a + "'");
    i += static_cast<size_t>(end - start);
    if (!IsNumeric(alt.kind))
      Fail(key, "a range applies to int or number, not '" + base + "'");
    if (lower) {
      alt.min = v;
      alt.minExclusive = !inclusive;
    } else {
      alt.max = v;
      alt.maxExclusive = !inclusive;
    }
  }
  if (i < a.size() && a[i] == '[') {
    const size_t close = a.find(']', i);
    if (close == std::string::npos)
      Fail(key, "unterminated '[' in '" + a + "'");
    const std::string n = a.substr(i + 1, close - i - 1);
    alt.array = true;
    if (!n.empty()) {
      for (char c : n)
        if (!std::isdigit(static_cast<unsigned char>(c)))
          Fail(key, "an array length is a count, not '" + n + "'");
      alt.length = std::atoi(n.c_str());
    }
    i = close + 1;
  }
  if (i != a.size())
    Fail(key, "unexpected '" + a.substr(i) + "' in '" + a + "'");
  return alt;
}

nlohmann::json AltSchema(const Alt& alt, const VocabResolver& vocab) {
  nlohmann::json s;
  switch (alt.kind) {
  case Kind::Int:
    s = {{"type", "integer"}};
    break;
  case Kind::Number:
    s = {{"type", "number"}};
    break;
  case Kind::Bool:
    s = {{"type", "boolean"}};
    break;
  case Kind::String:
    s = {{"type", "string"}};
    break;
  case Kind::Object:
    s = {{"type", "object"}};
    break;
  case Kind::Null:
    s = {{"type", "null"}};
    break;
  case Kind::Any:
    s = nlohmann::json::object();
    break;
  case Kind::Revision:
    s = RevisionSchema();
    break;
  case Kind::Literal:
    s = {{"type", "string"}, {"enum", {alt.text}}};
    break;
  case Kind::Vocab: {
    const Vocabulary* v = vocab ? vocab(alt.text) : nullptr;
    if (v == nullptr)
      throw std::invalid_argument("no vocabulary '" + alt.text +
                                  "' (a contract's @name is a list describe "
                                  "publishes)");
    s = {{"type", "string"}, {"enum", v->values}};
    if (!v->hint.empty())
      s["description"] = v->hint;
    break;
  }
  }
  if (alt.min)
    s[alt.minExclusive ? "exclusiveMinimum" : "minimum"] = *alt.min;
  if (alt.max)
    s[alt.maxExclusive ? "exclusiveMaximum" : "maximum"] = *alt.max;
  if (!alt.array)
    return s;
  nlohmann::json arr = {{"type", "array"}, {"items", s}};
  if (alt.length >= 0) {
    arr["minItems"] = alt.length;
    arr["maxItems"] = alt.length;
  }
  return arr;
}

} // namespace

const Param* Contract::Find(std::string_view name) const {
  for (const Param& p : params)
    if (p.name == name)
      return &p;
  return nullptr;
}

std::optional<Contract> ParseColumn(std::string_view column) {
  const std::string all = Trim(column);
  // A method that takes nothing has a complete contract already.
  if (all.empty())
    return Contract{};
  if (all == "*")
    return std::nullopt;
  std::vector<std::string> entries = SplitTop(all, ',');
  size_t typed = 0;
  for (std::string& e : entries) {
    e = Trim(e);
    if (e.find(':') != std::string::npos)
      typed++;
  }
  if (typed == 0)
    return std::nullopt;
  Contract c;
  for (const std::string& e : entries) {
    const size_t colon = e.find(':');
    std::string name = Trim(e.substr(0, colon));
    bool required = true;
    if (!name.empty() && name.back() == '?') {
      required = false;
      name = Trim(name.substr(0, name.size() - 1));
    }
    if (name.empty())
      throw std::invalid_argument("contract entry '" + e + "' names no key");
    if (colon == std::string::npos)
      Fail(name, "untyped in a typed row (a contract types every key)");
    if (c.Find(name) != nullptr)
      Fail(name, "listed twice");
    Param p;
    p.name = name;
    p.required = required;
    p.type = Trim(e.substr(colon + 1));
    for (const std::string& a : SplitTop(p.type, '|'))
      p.alts.push_back(ParseAlt(name, Trim(a)));
    c.params.push_back(std::move(p));
  }
  return c;
}

std::vector<std::string> ParamNames(std::string_view column) {
  std::vector<std::string> out;
  const std::string all = Trim(column);
  if (all.empty() || all == "*")
    return out;
  for (const std::string& e : SplitTop(all, ',')) {
    std::string name = Trim(e.substr(0, e.find(':')));
    if (!name.empty() && name.back() == '?')
      name = Trim(name.substr(0, name.size() - 1));
    if (!name.empty())
      out.push_back(name);
  }
  return out;
}

nlohmann::json RevisionSchema() {
  const nlohmann::json id = {
      {"type", "integer"}, {"minimum", 0}, {"maximum", 9007199254740991.0}};
  return {{"type", "object"},
          {"properties",
           {{"session", {{"type", "string"}, {"minLength", 1}}},
            {"part_id", id},
            {"instance", id},
            {"version", id}}},
          {"required", {"session", "part_id", "instance", "version"}},
          {"additionalProperties", false}};
}

nlohmann::json RequestSchema(const Contract& contract,
                             const VocabResolver& vocab) {
  nlohmann::json props = nlohmann::json::object();
  nlohmann::json required = nlohmann::json::array();
  for (const Param& p : contract.params) {
    // Consecutive literals fold into one enum, which is what a caller reads
    // as "one of".
    std::vector<nlohmann::json> alts;
    nlohmann::json literals = nlohmann::json::array();
    const auto flush = [&] {
      if (!literals.empty())
        alts.push_back({{"type", "string"}, {"enum", literals}});
      literals = nlohmann::json::array();
    };
    for (const Alt& a : p.alts) {
      if (a.kind == Kind::Literal && !a.array) {
        literals.push_back(a.text);
        continue;
      }
      flush();
      alts.push_back(AltSchema(a, vocab));
    }
    flush();
    props[p.name] =
        alts.size() == 1 ? alts.front() : nlohmann::json{{"anyOf", alts}};
    if (p.required)
      required.push_back(p.name);
  }
  return {{"type", "object"},
          {"properties", props},
          {"required", required},
          {"additionalProperties", false}};
}

} // namespace remote::contract
