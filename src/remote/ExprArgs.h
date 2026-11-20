// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// `<key>_expr` beside `<key>` on a method that takes its numbers through its
// own parsing -- an assembly's mates, relations, motion studies, sequence
// and exploded views (docs/scripting/rpc.md#numbers-that-take-an-expression).
//
// Each text is evaluated NOW, over the scope the document reads (the
// project's variables), and its value put where `<key>` goes in a copy of
// the params, so the handler reads one number exactly as before and checks
// it as before. The texts are then bound onto what the handler built
// (Bind), and a key given as a number alone unbinds. A text that does not
// parse, or names a variable the scope does not have, is refused before
// anything changes, naming the key.
//
// A key may name a nested place, "transform/position", and an array: an
// array `<key>_expr` is the same shape as `<key>` with a text, or "" where
// `<key>`'s own number stands, at each entry ([min, max], [x, y, z], a cam's
// [[angle, lift], ...]). Without `<key>`, every entry is a text.

#include "core/Angles.h"
#include "core/ExprValue.h"
#include "remote/RpcParams.h"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace remote {

class ExprArgs {
public:
  // `keys` are the method's numbers that take an expression, as paths.
  ExprArgs(const RpcParams& p, const std::vector<std::string>& keys,
           const SymbolTable& symbols)
      : m_outer(p), m_copy(p.raw_view()) {
    for (const std::string& spec : keys) {
      // "rad:<key>": a number the method takes in radians, whose text is in
      // degrees as every angle expression is (a configuration's held turn).
      const bool radians = spec.starts_with("rad:");
      const std::string pattern = radians ? spec.substr(4) : spec;
      for (const std::string& key : Expand(pattern)) {
        const auto [head, leaf] = Split(key);
        const nlohmann::json* parent = head.empty() ? &m_copy : Find(head);
        // Marked read on the caller's params: the dispatcher refuses a key
        // nobody read.
        (void)p.contains(TopOf(key));
        (void)p.contains(TopOf(key + "_expr"));
        if (parent == nullptr || !parent->is_object())
          continue;
        const std::string exprKey = leaf + "_expr";
        const bool hasNumber = parent->contains(leaf);
        const bool hasText = parent->contains(exprKey);
        if (!hasNumber && !hasText)
          continue;
        nlohmann::json& place = const_cast<nlohmann::json&>(*parent);
        if (!hasText) {
          m_given[key] = nlohmann::json(nullptr); // a number alone: unbind
          continue;
        }
        nlohmann::json texts = place.at(exprKey);
        nlohmann::json number =
            hasNumber ? place.at(leaf) : nlohmann::json(nullptr);
        place[leaf] = Evaluate(
            texts,
            radians && number.is_number()
                ? nlohmann::json(angles::ToDegrees(number.get<double>()))
                : number,
            key, symbols);
        if (radians && place[leaf].is_number())
          place[leaf] = angles::ToRadians(place[leaf].get<double>());
        place.erase(exprKey);
        m_given[key] = std::move(texts);
      }
    }
    m_params = std::make_unique<RpcParams>(m_copy);
  }
  // Every key the handler reads through Params() is read on the caller's.
  ~ExprArgs() {
    for (const std::string& k : m_params->SeenKeys())
      (void)m_outer.contains(k);
  }
  ExprArgs(const ExprArgs&) = delete;
  ExprArgs& operator=(const ExprArgs&) = delete;

  // What the handler reads in place of the caller's params.
  const RpcParams& Params() const { return *m_params; }

  // Whether the call named `key` at all, as a number or a text.
  bool Given(const std::string& key) const { return m_given.contains(key); }
  // The text at `key` (entry `i` of an array, `j` of a pair within it), ""
  // for a literal -- what binds a slot. Empty when the key was not given.
  std::string Text(const std::string& key, int i = -1, int j = -1) const {
    const auto it = m_given.find(key);
    if (it == m_given.end() || it->second.is_null())
      return {};
    const nlohmann::json* t = &it->second;
    if (i >= 0) {
      if (!t->is_array() || static_cast<size_t>(i) >= t->size())
        return {};
      t = &(*t)[static_cast<size_t>(i)];
    }
    if (j >= 0) {
      if (!t->is_array() || static_cast<size_t>(j) >= t->size())
        return {};
      t = &(*t)[static_cast<size_t>(j)];
    }
    return t->is_string() ? t->get<std::string>() : std::string();
  }
  // Binds `v` to the text the call gave at `key` (or unbinds it: a number
  // alone), when the call named the key. The VALUE is the handler's.
  void Bind(const std::string& key, std::string& expr, int i = -1,
            int j = -1) const {
    if (Given(key))
      expr = Text(key, i, j);
  }

private:
  // "terms/*/coefficient" names each entry of the array `terms` holds:
  // "terms/0/coefficient", "terms/1/coefficient", ...
  std::vector<std::string> Expand(const std::string& pattern) const {
    const size_t star = pattern.find("/*/");
    if (star == std::string::npos)
      return {pattern};
    const std::string list = pattern.substr(0, star);
    const std::string rest = pattern.substr(star + 3);
    std::vector<std::string> out;
    const nlohmann::json* a = Find(list);
    if (a != nullptr && a->is_array())
      for (size_t i = 0; i < a->size(); i++)
        for (std::string& k :
             Expand(list + "/" + std::to_string(i) + "/" + rest))
          out.push_back(std::move(k));
    return out;
  }
  static std::pair<std::string, std::string> Split(const std::string& key) {
    const size_t slash = key.rfind('/');
    if (slash == std::string::npos)
      return {"", key};
    return {key.substr(0, slash), key.substr(slash + 1)};
  }
  static std::string TopOf(const std::string& key) {
    return key.substr(0, key.find('/'));
  }
  const nlohmann::json* Find(const std::string& path) const {
    const nlohmann::json* at = &m_copy;
    size_t from = 0;
    while (from <= path.size()) {
      const size_t slash = path.find('/', from);
      const std::string seg = path.substr(
          from, slash == std::string::npos ? std::string::npos : slash - from);
      if (at->is_array()) {
        const size_t i = std::strtoul(seg.c_str(), nullptr, 10);
        if (seg.empty() ||
            seg.find_first_not_of("0123456789") != std::string::npos ||
            i >= at->size())
          return nullptr;
        at = &(*at)[i];
      } else if (at->is_object() && at->contains(seg)) {
        at = &at->at(seg);
      } else {
        return nullptr;
      }
      if (slash == std::string::npos)
        break;
      from = slash + 1;
    }
    return at;
  }

  // The number(s) `texts` give, each "" taking `number`'s own entry.
  static nlohmann::json Evaluate(const nlohmann::json& texts,
                                 const nlohmann::json& number,
                                 const std::string& key,
                                 const SymbolTable& symbols) {
    const std::string what = key + "_expr";
    if (texts.is_array()) {
      if (!number.is_null() &&
          (!number.is_array() || number.size() != texts.size()))
        throw std::runtime_error(what + " is the shape of " + key + " (" +
                                 number.dump() + "): a text, or \"\" for " +
                                 key + "'s own number, at each entry; got " +
                                 texts.dump());
      nlohmann::json out = nlohmann::json::array();
      for (size_t i = 0; i < texts.size(); i++)
        out.push_back(Evaluate(
            texts[i], number.is_null() ? nlohmann::json(nullptr) : number[i],
            key, symbols));
      return out;
    }
    if (texts.is_null() ||
        (texts.is_string() && texts.get<std::string>().empty())) {
      if (number.is_null())
        throw std::runtime_error(what + ": an entry left \"\" takes " + key +
                                 "'s own number there, and " + key +
                                 " was not given");
      return number;
    }
    if (!texts.is_string())
      throw std::runtime_error(what + " is an expression (text), not " +
                               texts.dump());
    const std::string text = texts.get<std::string>();
    RefuseUnreadableExpr(text, what);
    const auto v = ExpressionEngine::Default().EvaluateText(text, symbols);
    if (!v.has_value())
      throw std::runtime_error(what + " '" + text + "': " + v.error().message +
                               " (an assembly reads the project's variables)");
    return *v;
  }

  const RpcParams& m_outer;
  nlohmann::json m_copy;
  std::unique_ptr<RpcParams> m_params;
  std::map<std::string, nlohmann::json> m_given;
};

} // namespace remote
