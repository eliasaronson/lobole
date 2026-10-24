// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/io/ReadContext.h"

#include "common/Logger.h"

#include <algorithm>
#include <cmath>
#include <set>

namespace readctx {

namespace {
thread_local std::vector<std::string> t_segments;
thread_local CollectDrops* t_collector = nullptr;

std::string JoinPath(const std::string& base, const std::string& rel) {
  if (rel.empty())
    return base;
  return base + rel;
}
} // namespace

Error::Error(std::string path, std::string reason)
    : std::runtime_error(path.empty() ? reason : path + ": " + reason),
      m_path(std::move(path)), m_reason(std::move(reason)) {}

FileContext::FileContext(std::string segment) : m_depth(t_segments.size()) {
  t_segments.push_back(std::move(segment));
}

FileContext::~FileContext() { t_segments.resize(m_depth); }

std::string Path() {
  std::string out;
  for (const std::string& s : t_segments)
    out += s;
  return out;
}

void Refuse(const std::string& reason, const std::string& rel) {
  throw Error(JoinPath(Path(), rel), reason);
}

std::string Named(const std::string& reason, const std::string& rel) {
  const std::string path = JoinPath(Path(), rel);
  return path.empty() ? reason : path + ": " + reason;
}

namespace {

// A SAX pass that only tracks each object's keys, to refuse one written
// twice. The DOM comes from a plain parse after it: nlohmann's callback
// parser, which could have done both at once, is quadratic on a long array
// (a 182 000-element list took 6 s to parse, against 0.1 s plain).
class DuplicateKeys {
public:
  using json = nlohmann::json;
  bool null() { return Done(); }
  bool boolean(bool) { return Done(); }
  bool number_integer(json::number_integer_t) { return Done(); }
  bool number_unsigned(json::number_unsigned_t) { return Done(); }
  bool number_float(json::number_float_t, const json::string_t&) {
    return Done();
  }
  bool string(json::string_t&) { return Done(); }
  bool binary(json::binary_t&) { return Done(); }
  bool start_object(std::size_t) {
    Deeper();
    m_frames.push_back({});
    return true;
  }
  bool key(json::string_t& k) {
    Frame& f = m_frames.back();
    if (!f.keys.insert(k).second)
      throw std::runtime_error(
          "the key '" + k + "' is written twice in the object at '" +
          Pointer(m_frames.size() - 1) +
          "' -- a merge or a hand edit kept both; keep one");
    f.key = k;
    return true;
  }
  bool end_object() {
    m_frames.pop_back();
    return Done();
  }
  bool start_array(std::size_t) {
    Deeper();
    m_frames.push_back({true, 0, {}, {}});
    return true;
  }
  bool end_array() {
    m_frames.pop_back();
    return Done();
  }
  // A syntax error is the DOM parse's to report, with its position.
  bool parse_error(std::size_t, const std::string&,
                   const nlohmann::detail::exception&) {
    return false;
  }

private:
  struct Frame {
    bool array = false;
    size_t index = 0;
    std::string key;
    std::set<std::string> keys;
  };
  // nlohmann parses and destroys a value without recursing, but copies
  // and prints one recursively: a file of ten thousand nested brackets
  // parsed, and its first copy ran off the stack (under ASan, whose
  // frames are larger; deeper still, in any build). No document nests
  // past a dozen levels.
  void Deeper() const {
    if (m_frames.size() >= kMaxJsonDepth)
      throw std::runtime_error(
          "the value at '" + Pointer(m_frames.size()) + "' nests deeper than " +
          std::to_string(kMaxJsonDepth) + " levels, which no writer writes");
  }
  bool Done() {
    if (!m_frames.empty() && m_frames.back().array)
      m_frames.back().index++;
    return true;
  }
  std::string Pointer(size_t upTo) const {
    std::string out;
    for (size_t i = 0; i < upTo && i < m_frames.size(); i++)
      out += "/" + (m_frames[i].array ? std::to_string(m_frames[i].index)
                                      : m_frames[i].key);
    return out;
  }
  std::vector<Frame> m_frames;
};

} // namespace

nlohmann::json ParseStrict(const std::string& text) {
  // The checking pass first, so a value nested too deep is refused before
  // a DOM of it exists. A syntax error stops it quietly: the DOM parse
  // below reports that, with its position.
  DuplicateKeys keys;
  nlohmann::json::sax_parse(text, &keys);
  return nlohmann::json::parse(text);
}

namespace {
template <class List>
void RefuseUnknown(const nlohmann::json& j, const List& accepted,
                   std::string_view what) {
  if (!j.is_object())
    throw std::runtime_error(std::string(what) + ": expected an object, got " +
                             std::string(j.type_name()));
  for (auto it = j.begin(); it != j.end(); ++it) {
    if (std::find(accepted.begin(), accepted.end(),
                  std::string_view(it.key())) != accepted.end())
      continue;
    std::vector<std::string_view> sorted(accepted.begin(), accepted.end());
    std::sort(sorted.begin(), sorted.end());
    sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
    std::string list;
    for (std::string_view k : sorted)
      list += (list.empty() ? "" : ", ") + std::string(k);
    throw std::runtime_error(std::string(what) + ": unknown key '" + it.key() +
                             "' (accepted: " + list + ")");
  }
}
} // namespace

void RefuseUnknownKeys(const nlohmann::json& j,
                       std::initializer_list<std::string_view> accepted,
                       std::string_view what) {
  RefuseUnknown(j, accepted, what);
}

void RefuseUnknownKeys(const nlohmann::json& j,
                       const std::vector<std::string_view>& accepted,
                       std::string_view what) {
  RefuseUnknown(j, accepted, what);
}

void RequireTag(const nlohmann::json& j, std::string_view tag,
                std::string_view what) {
  const auto it = j.find("type");
  if (it == j.end())
    return;
  if (!it->is_string() || it->get<std::string>() != tag)
    throw std::runtime_error(std::string(what) + ": its type is " +
                             it->dump().substr(0, 80) + ", not \"" +
                             std::string(tag) + "\"");
}

void RequireCount(const nlohmann::json& j, std::string_view key,
                  std::string_view what) {
  const auto it = j.find(std::string(key));
  if (it != j.end() && !IsWhole(*it))
    throw std::runtime_error(std::string(what) + ": " + std::string(key) +
                             " is " + it->dump().substr(0, 80) +
                             ", not a whole number");
}

bool LoadReport::Lossy() const {
  return std::any_of(drops.begin(), drops.end(),
                     [](const Drop& d) { return !d.derived; });
}

std::string LoadReport::Summary(bool lossyOnly) const {
  std::string out;
  for (const Drop& d : drops) {
    if (lossyOnly && d.derived)
      continue;
    if (!out.empty())
      out += "\n";
    out += d.path;
    if (!d.pointer.empty())
      out += " " + d.pointer;
    out += ": " + d.reason;
  }
  return out;
}

nlohmann::json LoadReport::ToJson() const {
  nlohmann::json out = nlohmann::json::array();
  for (const Drop& d : drops)
    out.push_back({{"path", d.path},
                   {"pointer", d.pointer},
                   {"reason", d.reason},
                   {"derived", d.derived}});
  return out;
}

void LoadReport::Append(const LoadReport& other) {
  drops.insert(drops.end(), other.drops.begin(), other.drops.end());
}

void Dropped(const std::string& pointer, const std::string& reason,
             bool derived, const std::string& rel) {
  Drop d{JoinPath(Path(), rel), pointer, reason, derived};
  LOG_WARN("Load kept going past {}{}: {}{}", d.path,
           d.pointer.empty() ? "" : " " + d.pointer, d.reason,
           derived ? " (derived; rebuilt)" : "");
  if (t_collector != nullptr)
    t_collector->m_report.drops.push_back(std::move(d));
}

CollectDrops::CollectDrops() : m_outer(t_collector) { t_collector = this; }

CollectDrops::~CollectDrops() {
  t_collector = m_outer;
  if (m_outer != nullptr)
    m_outer->m_report.Append(m_report);
}

LoadReport CollectDrops::Take() {
  LoadReport out = std::move(m_report);
  m_report = {};
  return out;
}

} // namespace readctx
