// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// The "nothing new arrives unbindable" probe's walk over a document's file
// (docs/variables/README.md#numbers-that-are-expressions): every number in
// the JSON by path, a path with its indices written `*`, and where a slot's
// text is written. A probe gives each slot a sentinel, writes the file, and
// checks that each sentinel landed beside its number and that every other
// number is on its list of the deliberately unbindable, with the reason.

#include <nlohmann/json.hpp>

#include <string>
#include <utility>
#include <vector>

namespace exprprobe {

using Reasons = std::vector<std::pair<std::string, std::string>>;

inline std::vector<std::string> Segments(const std::string& path) {
  std::vector<std::string> segs;
  size_t from = 0;
  while (true) {
    const size_t slash = path.find('/', from);
    segs.push_back(path.substr(from, slash - from));
    if (slash == std::string::npos)
      return segs;
    from = slash + 1;
  }
}

inline bool IsIndex(const std::string& s) {
  return !s.empty() && s.find_first_not_of("0123456789") == std::string::npos;
}

// "cam/0/tools/2/diameter" -> "cam/*/tools/*/diameter".
inline std::string Generic(const std::string& path) {
  std::string out;
  for (const std::string& seg : Segments(path))
    out += (out.empty() ? "" : "/") + (IsIndex(seg) ? std::string("*") : seg);
  return out;
}

// Whether a generic path is on the list: exactly, or under a "/**" entry.
inline bool Listed(const Reasons& list, const std::string& generic) {
  for (const auto& [pattern, why] : list) {
    if (pattern == generic)
      return true;
    if (pattern.ends_with("/**") &&
        generic.starts_with(pattern.substr(0, pattern.size() - 2)))
      return true;
  }
  return false;
}

// Every number in `j`, by path.
inline void Numbers(const nlohmann::json& j, const std::string& at,
                    std::vector<std::string>& out) {
  if (j.is_number()) {
    out.push_back(at);
  } else if (j.is_object()) {
    for (auto it = j.begin(); it != j.end(); ++it)
      Numbers(it.value(), at.empty() ? it.key() : at + "/" + it.key(), out);
  } else if (j.is_array()) {
    for (size_t i = 0; i < j.size(); i++)
      Numbers(j[i], at + "/" + std::to_string(i), out);
  }
}

// Where a slot's text is written: `<last key>_expr` in place of its last
// key ("stock/margin/1" -> "stock/margin_expr/1").
inline std::string ExprPathOf(const std::string& path) {
  std::vector<std::string> segs = Segments(path);
  for (size_t i = segs.size(); i-- > 0;)
    if (!IsIndex(segs[i])) {
      segs[i] += "_expr";
      break;
    }
  std::string out;
  for (const std::string& s : segs)
    out += (out.empty() ? "" : "/") + s;
  return out;
}

inline const nlohmann::json* At(const nlohmann::json& j,
                                const std::string& path) {
  const nlohmann::json* at = &j;
  for (const std::string& seg : Segments(path)) {
    if (at->is_array() && IsIndex(seg)) {
      const size_t i = std::stoul(seg);
      if (i >= at->size())
        return nullptr;
      at = &(*at)[i];
    } else if (at->is_object() && at->contains(seg)) {
      at = &at->at(seg);
    } else {
      return nullptr;
    }
  }
  return at;
}

} // namespace exprprobe
