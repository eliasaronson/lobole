// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/LookupTable.h"

#include <cctype>
#include <charconv>
#include <cmath>
#include <set>
#include <stdexcept>

namespace lookuptable {

namespace {

bool IsName(const std::string& s) {
  if (s.empty() ||
      (std::isalpha(static_cast<unsigned char>(s[0])) == 0 && s[0] != '_'))
    return false;
  for (const char c : s)
    if (std::isalnum(static_cast<unsigned char>(c)) == 0 && c != '_')
      return false;
  return true;
}

// The key as JSON: a number when it is a number's one spelling, so a file
// reads 6002 rather than "6002" for a numeric key.
nlohmann::json KeyToJson(const std::string& key) {
  double v = 0.0;
  const auto r = std::from_chars(key.data(), key.data() + key.size(), v);
  if (r.ec == std::errc() && r.ptr == key.data() + key.size() &&
      std::isfinite(v) && NumericKey(v) == key)
    return v;
  return key;
}

} // namespace

std::string Validate(const Table& t) {
  if (!IsName(t.name))
    return "a table name is letters, digits and _ (not a digit first), like "
           "bearings — \"" +
           t.name + "\" is not";
  if (t.columns.empty())
    return "table '" + t.name + "' has no columns";
  std::set<std::string> seen;
  for (const std::string& c : t.columns) {
    if (!IsName(c))
      return "table '" + t.name +
             "': a column name is letters, digits and _ (not a digit "
             "first) — \"" +
             c + "\" is not";
    if (!seen.insert(c).second)
      return "table '" + t.name + "' names column '" + c + "' twice";
  }
  std::set<std::string> keys;
  for (const Row& r : t.rows) {
    if (r.key.empty())
      return "table '" + t.name + "' has a row with an empty key";
    for (const char c : r.key)
      if (std::iscntrl(static_cast<unsigned char>(c)) != 0)
        return "table '" + t.name + "': key \"" + r.key +
               "\" holds a control character";
    if (!keys.insert(r.key).second)
      return "table '" + t.name + "' has two rows keyed '" + r.key + "'";
    if (r.values.size() != t.columns.size())
      return "table '" + t.name + "': row '" + r.key + "' has " +
             std::to_string(r.values.size()) + " values for " +
             std::to_string(t.columns.size()) + " columns";
    for (size_t i = 0; i < r.values.size(); i++)
      if (!std::isfinite(r.values[i]))
        return "table '" + t.name + "': row '" + r.key + "', column '" +
               t.columns[i] + "' is not a finite number";
  }
  return {};
}

std::optional<std::string> KeyFromJson(const nlohmann::json& j,
                                       std::string* why) {
  if (j.is_number())
    return NumericKey(j.get<double>());
  if (j.is_string())
    return j.get<std::string>();
  if (why)
    *why = "a row key is a number or a string, not " + j.dump();
  return std::nullopt;
}

nlohmann::json ToJson(const Table& t) {
  nlohmann::json j = {{"name", t.name}, {"columns", t.columns}};
  if (!t.comment.empty())
    j["comment"] = t.comment;
  nlohmann::json rows = nlohmann::json::array();
  for (const Row& r : t.rows)
    rows.push_back({{"key", KeyToJson(r.key)}, {"values", r.values}});
  j["rows"] = std::move(rows);
  return j;
}

nlohmann::json ToJson(const std::vector<Table>& tables) {
  nlohmann::json out = nlohmann::json::array();
  for (const Table& t : tables)
    out.push_back(ToJson(t));
  return out;
}

std::optional<Table> FromJson(const nlohmann::json& j, std::string* why) {
  const auto fail = [&](const std::string& message) -> std::optional<Table> {
    if (why)
      *why = message;
    return std::nullopt;
  };
  if (!j.is_object() || !j.contains("name") || !j.at("name").is_string())
    return fail("a table is {name, columns: [...], rows: [{key, values: "
                "[...]}]}");
  Table t;
  t.name = j.at("name").get<std::string>();
  t.comment = j.value("comment", std::string{});
  if (!j.contains("columns") || !j.at("columns").is_array())
    return fail("table '" + t.name + "' needs `columns`, a list of names");
  for (const auto& c : j.at("columns")) {
    if (!c.is_string())
      return fail("table '" + t.name + "': a column name is a string, not " +
                  c.dump());
    t.columns.push_back(c.get<std::string>());
  }
  for (const auto& rj : j.value("rows", nlohmann::json::array())) {
    if (!rj.is_object() || !rj.contains("key") || !rj.contains("values") ||
        !rj.at("values").is_array())
      return fail("table '" + t.name +
                  "': a row is {key, values: [one per column]}");
    std::string keyWhy;
    const auto key = KeyFromJson(rj.at("key"), &keyWhy);
    if (!key)
      return fail("table '" + t.name + "': " + keyWhy);
    Row r;
    r.key = *key;
    for (const auto& v : rj.at("values")) {
      if (!v.is_number())
        return fail("table '" + t.name + "': row '" + r.key +
                    "' holds a value that is not a number: " + v.dump());
      r.values.push_back(v.get<double>());
    }
    t.rows.push_back(std::move(r));
  }
  if (std::string bad = Validate(t); !bad.empty())
    return fail(bad);
  return t;
}

std::vector<Table> ListFromJson(const nlohmann::json& j) {
  std::vector<Table> out;
  if (j.is_null())
    return out;
  if (!j.is_array())
    throw std::runtime_error("`tables` is a list of tables");
  std::set<std::string> names;
  for (const auto& tj : j) {
    std::string why;
    auto t = FromJson(tj, &why);
    if (!t)
      throw std::runtime_error(why);
    if (!names.insert(t->name).second)
      throw std::runtime_error("two tables are named '" + t->name + "'");
    out.push_back(std::move(*t));
  }
  return out;
}

void AddCellSymbols(const std::vector<Table>& tables, SymbolTable& out) {
  for (const Table& t : tables)
    for (const Row& r : t.rows)
      for (size_t i = 0; i < t.columns.size() && i < r.values.size(); i++)
        out[CellSymbol(t.name, r.key, t.columns[i])] = r.values[i];
}

const Table* Find(const std::vector<Table>& tables, const std::string& name) {
  for (const Table& t : tables)
    if (t.name == name)
      return &t;
  return nullptr;
}

} // namespace lookuptable
