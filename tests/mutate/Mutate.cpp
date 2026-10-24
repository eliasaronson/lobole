// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "mutate/Mutate.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <functional>
#include <map>
#include <set>

namespace mutate {

namespace fs = std::filesystem;
using json = nlohmann::json;

uint64_t Hash(std::string_view s) {
  uint64_t h = 1469598103934665603ull;
  for (unsigned char c : s) {
    h ^= c;
    h *= 1099511628211ull;
  }
  return h;
}

Rng::Rng(uint64_t seed) : m_state(seed == 0 ? 0x9e3779b97f4a7c15ull : seed) {}

uint64_t Rng::Next() {
  uint64_t x = m_state;
  x ^= x << 13;
  x ^= x >> 7;
  x ^= x << 17;
  m_state = x;
  return x;
}

size_t Rng::Below(size_t n) {
  return n == 0 ? 0 : static_cast<size_t>(Next() % n);
}

const std::vector<std::string>& Liars() {
  static const std::vector<std::string> liars = {"nan",
                                                 "inf",
                                                 "1e400",
                                                 "-0",
                                                 "-1",
                                                 "0",
                                                 "2147483648",
                                                 "9007199254740993",
                                                 "18446744073709551616",
                                                 "-9223372036854775808",
                                                 "1.5"};
  return liars;
}

std::vector<std::string> FileOperators() {
  return {// bytes
          "truncate", "flip", "zero", "ff", "drop_run", "repeat_run",
          "swap_runs", "append_garbage", "double", "reverse", "empty",
          // text
          "drop_line", "repeat_line", "swap_lines", "replace_line",
          "replace_word", "liar_number", "all_liars",
          // json
          "delete_key", "dup_key", "reorder_keys", "retype", "json_liar",
          "enum_char", "version", "deep_wrap", "grow_array",
          // model
          "ref_missing", "ref_self", "ref_later", "dup_id", "id_zero",
          "reorder"};
}

std::vector<std::string> TreeOperators() {
  return {"rm_file",   "empty_file", "dup_file",    "unlist",
          "swap_kind", "escape",     "symlink_out", "bin_lie"};
}

namespace {

// Builds the cases of one file, numbering each operator's.
class Builder {
public:
  Builder(const std::string& corpus, const std::string& file,
          const Options& opt, std::vector<FileCase>& out)
      : m_prefix(corpus + "/" + file + ":"), m_opt(opt), m_out(out) {}

  std::string Id(const std::string& op, size_t n) const {
    return m_prefix + op + ":" + std::to_string(n);
  }
  Rng RngFor(const std::string& op, size_t n) const {
    return Rng(Hash(Id(op, n)) ^ (m_opt.seed * 0x9e3779b97f4a7c15ull));
  }
  bool Wanted(const std::string& op, size_t n) const {
    return !m_opt.want || m_opt.want(Id(op, n));
  }
  void Add(const std::string& op, size_t n, std::string bytes) {
    m_out.push_back({Id(op, n), op, std::move(bytes)});
  }
  // A case built only when it is wanted.
  void Lazy(const std::string& op, size_t n,
            const std::function<std::string()>& make) {
    if (Wanted(op, n))
      Add(op, n, make());
  }
  // `count` random-target cases (capped), each made by `make(rng)`; one
  // that returns false makes no case.
  void Random(const std::string& op, size_t candidates,
              const std::function<bool(Rng&, std::string&)>& make) {
    if (candidates == 0)
      return;
    const size_t n = m_opt.perOperator == 0
                         ? candidates
                         : std::min(candidates, m_opt.perOperator);
    for (size_t i = 0; i < n; i++) {
      if (!Wanted(op, i))
        continue;
      Rng rng = RngFor(op, i);
      std::string bytes;
      if (make(rng, bytes))
        Add(op, i, std::move(bytes));
    }
  }
  const Options& Opt() const { return m_opt; }

private:
  std::string m_prefix;
  const Options& m_opt;
  std::vector<FileCase>& m_out;
};

// ---- bytes -----------------------------------------------------------------

void ByteCases(Builder& b, const std::string& bytes) {
  const Options& opt = b.Opt();
  const size_t size = bytes.size();
  // Truncation: down to empty.
  {
    std::set<size_t> lengths;
    if (size < opt.truncateAll) {
      for (size_t l = 0; l < size; l++)
        lengths.insert(l);
    } else {
      for (size_t l = 0; l < opt.truncateEnds && l < size; l++) {
        lengths.insert(l);
        lengths.insert(size - 1 - l);
      }
      for (size_t i = 0; i < opt.truncateSpread; i++)
        lengths.insert(size * i / opt.truncateSpread);
    }
    size_t n = 0;
    for (size_t l : lengths)
      if (l < size) {
        b.Lazy("truncate", n, [&] { return bytes.substr(0, l); });
        n++;
      }
  }
  if (size == 0)
    return;
  const auto setByte = [&](const char* op, auto&& change) {
    b.Random(op, size, [&](Rng& rng, std::string& out) {
      out = bytes;
      const size_t at = rng.Below(size);
      out[at] = change(out[at], rng);
      return out != bytes;
    });
  };
  setByte("flip", [](char c, Rng& rng) {
    return static_cast<char>(c ^ (1 << rng.Below(8)));
  });
  setByte("zero", [](char, Rng&) { return '\0'; });
  setByte("ff", [](char, Rng&) { return static_cast<char>(0xff); });
  const auto run = [&](Rng& rng, size_t& at, size_t& len) {
    len = 1 + rng.Below(std::min<size_t>(64, size));
    at = rng.Below(size - len + 1);
  };
  b.Random("drop_run", size, [&](Rng& rng, std::string& out) {
    size_t at, len;
    run(rng, at, len);
    out = bytes.substr(0, at) + bytes.substr(at + len);
    return true;
  });
  b.Random("repeat_run", size, [&](Rng& rng, std::string& out) {
    size_t at, len;
    run(rng, at, len);
    out = bytes.substr(0, at + len) + bytes.substr(at);
    return true;
  });
  b.Random("swap_runs", size, [&](Rng& rng, std::string& out) {
    size_t at, len;
    run(rng, at, len);
    const size_t len2 = std::min(len, size - at - len);
    if (len2 == 0)
      return false;
    out = bytes.substr(0, at) + bytes.substr(at + len, len2) +
          bytes.substr(at, len) + bytes.substr(at + len + len2);
    return out != bytes;
  });
  b.Random("append_garbage", size, [&](Rng& rng, std::string& out) {
    out = bytes;
    const size_t n = 1 + rng.Below(256);
    for (size_t i = 0; i < n; i++)
      out += static_cast<char>(rng.Next() & 0xff);
    return true;
  });
  b.Lazy("double", 0, [&] { return bytes + bytes; });
  b.Lazy("reverse", 0,
         [&] { return std::string(bytes.rbegin(), bytes.rend()); });
  b.Lazy("empty", 0, [] { return std::string(); });
}

// ---- text ------------------------------------------------------------------

std::vector<std::string> SplitLines(const std::string& text) {
  std::vector<std::string> lines;
  size_t start = 0;
  while (start <= text.size()) {
    const size_t nl = text.find('\n', start);
    if (nl == std::string::npos) {
      if (start < text.size())
        lines.push_back(text.substr(start));
      break;
    }
    lines.push_back(text.substr(start, nl - start));
    start = nl + 1;
  }
  return lines;
}

std::string JoinLines(const std::vector<std::string>& lines) {
  std::string out;
  for (const std::string& l : lines)
    out += l + "\n";
  return out;
}

struct Span {
  size_t at, len;
};

// Number tokens outside strings, and word tokens anywhere.
void Tokens(const std::string& text, std::vector<Span>& numbers,
            std::vector<Span>& words) {
  bool inString = false;
  for (size_t i = 0; i < text.size();) {
    const char c = text[i];
    if (inString) {
      if (c == '\\') {
        i += 2;
        continue;
      }
      if (c == '"')
        inString = false;
    } else if (c == '"') {
      inString = true;
    } else if (c == '-' || std::isdigit(static_cast<unsigned char>(c))) {
      size_t j = i + 1;
      while (j < text.size() &&
             (std::isdigit(static_cast<unsigned char>(text[j])) ||
              text[j] == '.' || text[j] == 'e' || text[j] == 'E' ||
              text[j] == '+' || text[j] == '-'))
        j++;
      numbers.push_back({i, j - i});
      i = j;
      continue;
    }
    if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
      size_t j = i + 1;
      while (
          j < text.size() &&
          (std::isalnum(static_cast<unsigned char>(text[j])) || text[j] == '_'))
        j++;
      words.push_back({i, j - i});
      i = j;
      continue;
    }
    i++;
  }
}

std::string Splice(const std::string& text, Span s, const std::string& with) {
  return text.substr(0, s.at) + with + text.substr(s.at + s.len);
}

void TextCases(Builder& b, const std::string& text) {
  const std::vector<std::string> lines = SplitLines(text);
  const size_t n = lines.size();
  if (n >= 1) {
    b.Random("drop_line", n, [&](Rng& rng, std::string& out) {
      auto l = lines;
      l.erase(l.begin() + static_cast<long>(rng.Below(n)));
      out = JoinLines(l);
      return true;
    });
    b.Random("repeat_line", n, [&](Rng& rng, std::string& out) {
      auto l = lines;
      const size_t at = rng.Below(n);
      l.insert(l.begin() + static_cast<long>(at), lines[at]);
      out = JoinLines(l);
      return true;
    });
  }
  if (n >= 2) {
    b.Random("swap_lines", n - 1, [&](Rng& rng, std::string& out) {
      auto l = lines;
      const size_t at = rng.Below(n - 1);
      std::swap(l[at], l[at + 1]);
      out = JoinLines(l);
      return out != text;
    });
    b.Random("replace_line", n, [&](Rng& rng, std::string& out) {
      auto l = lines;
      l[rng.Below(n)] = lines[rng.Below(n)];
      out = JoinLines(l);
      return out != JoinLines(lines);
    });
  }
  std::vector<Span> numbers, words;
  Tokens(text, numbers, words);
  if (words.size() >= 2)
    b.Random("replace_word", words.size(), [&](Rng& rng, std::string& out) {
      const Span at = words[rng.Below(words.size())];
      const Span with = words[rng.Below(words.size())];
      const std::string w = text.substr(with.at, with.len);
      if (w == text.substr(at.at, at.len))
        return false;
      out = Splice(text, at, w);
      return true;
    });
  // Each number in turn (up to the cap, then a seeded pick), each lying.
  const auto& liars = Liars();
  if (!numbers.empty()) {
    const bool each =
        b.Opt().perOperator == 0 || numbers.size() <= b.Opt().perOperator;
    const size_t count = each ? numbers.size() : b.Opt().perOperator;
    for (size_t i = 0; i < count; i++) {
      if (!b.Wanted("liar_number", i))
        continue;
      Rng rng = b.RngFor("liar_number", i);
      const Span at = numbers[each ? i : rng.Below(numbers.size())];
      b.Add("liar_number", i, Splice(text, at, liars[rng.Below(liars.size())]));
    }
    for (size_t li = 0; li < liars.size(); li++) {
      if (!b.Wanted("all_liars", li))
        continue;
      std::string out;
      size_t last = 0;
      for (const Span& s : numbers) {
        out += text.substr(last, s.at - last) + liars[li];
        last = s.at + s.len;
      }
      out += text.substr(last);
      b.Add("all_liars", li, std::move(out));
    }
  }
}

// ---- json ------------------------------------------------------------------

struct Node {
  json::json_pointer ptr;
  std::string key; // its key in an object parent ("" in an array, or root)
  bool inObject = false;
  const json* value = nullptr;
  json::json_pointer item; // nearest enclosing object carrying "id"
  bool hasItem = false;
};

void Walk(const json& j, const json::json_pointer& ptr, const std::string& key,
          bool inObject, const json::json_pointer& item, bool hasItem,
          std::vector<Node>& out) {
  json::json_pointer here = item;
  bool hereHas = hasItem;
  if (j.is_object() && j.contains("id") && j.at("id").is_number_unsigned()) {
    here = ptr;
    hereHas = true;
  }
  out.push_back({ptr, key, inObject, &j, here, hereHas});
  if (j.is_object()) {
    for (auto it = j.begin(); it != j.end(); ++it)
      Walk(it.value(), ptr / it.key(), it.key(), true, here, hereHas, out);
  } else if (j.is_array()) {
    for (size_t i = 0; i < j.size(); i++)
      Walk(j[i], ptr / i, "", false, here, hereHas, out);
  }
}

// A dump(2) of `j` with the value at `at` replaced by raw text.
std::string WithRaw(json j, const json::json_pointer& at,
                    const std::string& raw) {
  static const std::string token = "@@RAW_TEXT@@";
  if (at.empty())
    return raw + "\n";
  j[at] = token;
  std::string text = j.dump(2) + "\n";
  const std::string quoted = "\"" + token + "\"";
  const size_t p = text.find(quoted);
  return text.substr(0, p) + raw + text.substr(p + quoted.size());
}

// The same json, every object's keys in reverse order.
nlohmann::ordered_json Reversed(const json& j) {
  if (j.is_object()) {
    nlohmann::ordered_json out = nlohmann::ordered_json::object();
    std::vector<std::string> keys;
    for (auto it = j.begin(); it != j.end(); ++it)
      keys.push_back(it.key());
    for (auto k = keys.rbegin(); k != keys.rend(); ++k)
      out[*k] = Reversed(j.at(*k));
    return out;
  }
  if (j.is_array()) {
    nlohmann::ordered_json out = nlohmann::ordered_json::array();
    for (const json& e : j)
      out.push_back(Reversed(e));
    return out;
  }
  return nlohmann::ordered_json::parse(j.dump());
}

bool IsEnumLike(const json& v) {
  if (!v.is_string())
    return false;
  const std::string s = v.get<std::string>();
  if (s.size() < 2 || !std::islower(static_cast<unsigned char>(s[0])))
    return false;
  return std::all_of(s.begin(), s.end(), [](char c) {
    return std::islower(static_cast<unsigned char>(c)) ||
           std::isdigit(static_cast<unsigned char>(c)) || c == '_';
  });
}

bool IsRefKey(const std::string& k) {
  static const std::set<std::string> refs = {"source",
                                             "occurrence",
                                             "setup",
                                             "view",
                                             "base_view",
                                             "plane_ref",
                                             "mate",
                                             "dim",
                                             "parent",
                                             "configuration",
                                             "active_configuration"};
  return (k.size() > 3 && k.ends_with("_id")) || refs.contains(k);
}

void JsonCases(Builder& b, const json& doc) {
  std::vector<Node> nodes;
  Walk(doc, json::json_pointer(), "", false, json::json_pointer(), false,
       nodes);
  std::vector<const Node*> keyed, numbers, enums, arrays, values, refs, items;
  std::set<uint64_t> idSet;
  std::vector<uint64_t> idsInOrder;
  for (const Node& n : nodes) {
    if (!n.ptr.empty())
      values.push_back(&n);
    if (n.inObject)
      keyed.push_back(&n);
    if (n.value->is_number())
      numbers.push_back(&n);
    if (IsEnumLike(*n.value))
      enums.push_back(&n);
    if (n.value->is_array() && !n.value->empty())
      arrays.push_back(&n);
    if (n.inObject && IsRefKey(n.key) && n.value->is_number_unsigned() &&
        n.value->get<uint64_t>() > 0)
      refs.push_back(&n);
    if (n.inObject && n.key == "id" && n.value->is_number_unsigned()) {
      items.push_back(&n);
      const uint64_t id = n.value->get<uint64_t>();
      if (idSet.insert(id).second)
        idsInOrder.push_back(id);
    }
  }
  const auto dump = [](const json& j) { return j.dump(2) + "\n"; };
  b.Random("delete_key", keyed.size(), [&](Rng& rng, std::string& out) {
    const Node& n = *keyed[rng.Below(keyed.size())];
    json j = doc;
    j[n.ptr.parent_pointer()].erase(n.key);
    out = dump(j);
    return true;
  });
  b.Random("dup_key", keyed.size(), [&](Rng& rng, std::string& out) {
    const Node& n = *keyed[rng.Below(keyed.size())];
    static const std::string token = "@@DUP_KEY@@";
    json j = doc;
    json other = n.value->is_number() ? json(n.value->get<double>() + 1.0)
                 : n.value->is_string()
                     ? json(n.value->get<std::string>() + "x")
                     : json(nullptr);
    j[n.ptr.parent_pointer()][token] = other;
    out = dump(j);
    const std::string quoted = "\"" + token + "\"";
    const size_t p = out.find(quoted);
    out = out.substr(0, p) + json(n.key).dump() + out.substr(p + quoted.size());
    return true;
  });
  b.Lazy("reorder_keys", 0, [&] { return Reversed(doc).dump(2) + "\n"; });
  b.Random("retype", values.size(), [&](Rng& rng, std::string& out) {
    const Node& n = *values[rng.Below(values.size())];
    static const json kinds[] = {json(nullptr), json(true), json(7),
                                 json(2.5),     json("x"),  json::array(),
                                 json::object()};
    json with = kinds[rng.Below(std::size(kinds))];
    if (with.type() == n.value->type())
      return false;
    json j = doc;
    j[n.ptr] = with;
    out = dump(j);
    return true;
  });
  b.Random("json_liar", numbers.size(), [&](Rng& rng, std::string& out) {
    const Node& n = *numbers[rng.Below(numbers.size())];
    out = WithRaw(doc, n.ptr, Liars()[rng.Below(Liars().size())]);
    return true;
  });
  b.Random("enum_char", enums.size(), [&](Rng& rng, std::string& out) {
    const Node& n = *enums[rng.Below(enums.size())];
    std::string s = n.value->get<std::string>();
    const size_t at = rng.Below(s.size());
    const char c = static_cast<char>('a' + rng.Below(26));
    if (s[at] == c)
      return false;
    s[at] = c;
    json j = doc;
    j[n.ptr] = s;
    out = dump(j);
    return true;
  });
  if (doc.is_object() && doc.contains("version")) {
    const int v = b.Opt().version;
    const json variants[] = {json(0),     json(1),  json(v),
                             json(v + 1), json(-1), json(std::to_string(v))};
    size_t i = 0;
    for (const json& with : variants)
      b.Lazy("version", i++, [&] {
        json j = doc;
        j["version"] = with;
        return dump(j);
      });
    b.Lazy("version", i, [&] {
      json j = doc;
      j.erase("version");
      return dump(j);
    });
  }
  b.Random("deep_wrap", values.size(), [&](Rng& rng, std::string& out) {
    const Node& n = *values[rng.Below(values.size())];
    out = WithRaw(doc, n.ptr,
                  std::string(10000, '[') + n.value->dump() +
                      std::string(10000, ']'));
    return true;
  });
  {
    // Growing is the costly case; a third of the usual cap.
    const size_t n = std::min<size_t>(
        arrays.size(), b.Opt().perOperator == 0
                           ? arrays.size()
                           : std::max<size_t>(1, b.Opt().perOperator / 3));
    for (size_t i = 0; i < n; i++) {
      if (!b.Wanted("grow_array", i))
        continue;
      Rng rng = b.RngFor("grow_array", i);
      const Node& a = *arrays[rng.Below(arrays.size())];
      const std::string first = a.value->at(0).dump();
      // Bounded: a large first element grows to at most 8 MB of text.
      const size_t copies = std::max<size_t>(
          2, std::min(b.Opt().growTo, (size_t{8} << 20) / (first.size() + 1)));
      std::string raw;
      raw.reserve(copies * (first.size() + 1) + 2);
      raw += '[';
      for (size_t c = 0; c < copies; c++) {
        if (c)
          raw += ',';
        raw += first;
      }
      raw += ']';
      b.Add("grow_array", i, WithRaw(doc, a.ptr, raw));
    }
  }
  // ---- model ----
  const uint64_t maxId =
      idsInOrder.empty()
          ? 0
          : *std::max_element(idsInOrder.begin(), idsInOrder.end());
  b.Random("ref_missing", refs.size(), [&](Rng& rng, std::string& out) {
    const Node& n = *refs[rng.Below(refs.size())];
    json j = doc;
    j[n.ptr] = std::max(maxId, n.value->get<uint64_t>()) + 1000;
    out = dump(j);
    return true;
  });
  b.Random("ref_self", refs.size(), [&](Rng& rng, std::string& out) {
    const Node& n = *refs[rng.Below(refs.size())];
    if (!n.hasItem || n.item == n.ptr.parent_pointer())
      return false; // its own object's id: not a reference to itself
    const json& owner = doc.at(n.item);
    json j = doc;
    j[n.ptr] = owner.at("id");
    out = dump(j);
    return true;
  });
  b.Random("ref_later", refs.size(), [&](Rng& rng, std::string& out) {
    const Node& n = *refs[rng.Below(refs.size())];
    // An id that first appears after this reference's own item.
    uint64_t own = 0;
    if (n.hasItem)
      own = doc.at(n.item).at("id").get<uint64_t>();
    auto it = std::find(idsInOrder.begin(), idsInOrder.end(), own);
    if (it == idsInOrder.end() || it + 1 == idsInOrder.end())
      return false;
    const size_t after = static_cast<size_t>(it - idsInOrder.begin()) + 1;
    json j = doc;
    j[n.ptr] = idsInOrder[after + rng.Below(idsInOrder.size() - after)];
    out = dump(j);
    return true;
  });
  b.Random("dup_id", items.size(), [&](Rng& rng, std::string& out) {
    if (items.size() < 2)
      return false;
    const Node& a = *items[rng.Below(items.size())];
    const Node& c = *items[rng.Below(items.size())];
    if (a.value->get<uint64_t>() == c.value->get<uint64_t>())
      return false;
    json j = doc;
    j[c.ptr] = *a.value;
    out = dump(j);
    return true;
  });
  b.Random("id_zero", items.size(), [&](Rng& rng, std::string& out) {
    const Node& n = *items[rng.Below(items.size())];
    json j = doc;
    j[n.ptr] = 0;
    out = dump(j);
    return true;
  });
  // Items reordered: two elements of a list of items (or of an index's id
  // list) swapped, so a reference can point forward.
  std::vector<const Node*> lists;
  for (const Node* a : arrays) {
    const json& v = *a->value;
    if (v.size() < 2)
      continue;
    const bool ofItems = std::all_of(v.begin(), v.end(), [](const json& e) {
      return e.is_object() && e.contains("id");
    });
    const bool ofIds = std::all_of(v.begin(), v.end(), [](const json& e) {
      return e.is_number_unsigned();
    });
    if (ofItems || ofIds)
      lists.push_back(a);
  }
  b.Random("reorder", lists.size(), [&](Rng& rng, std::string& out) {
    const Node& a = *lists[rng.Below(lists.size())];
    json j = doc;
    json& l = j[a.ptr];
    const size_t x = rng.Below(l.size()), y = rng.Below(l.size());
    if (x == y)
      return false;
    std::swap(l[x], l[y]);
    out = dump(j);
    return true;
  });
}

} // namespace

std::vector<FileCase> FileCases(const std::string& corpus,
                                const std::string& file,
                                const std::string& bytes, const Options& opt) {
  std::vector<FileCase> out;
  Builder b(corpus, file, opt, out);
  ByteCases(b, bytes);
  if (opt.binary)
    return out;
  TextCases(b, bytes);
  json doc;
  try {
    doc = json::parse(bytes);
  } catch (const std::exception&) {
    return out;
  }
  JsonCases(b, doc);
  return out;
}

namespace {

std::string ReadBytes(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
}

bool IsBinaryName(const fs::path& p) {
  const std::string ext = p.extension().string();
  return ext == ".brep" || ext == ".bin" || ext == ".png" || ext == ".jpg" ||
         ext == ".jpeg" || ext == ".stl" || ext == ".step";
}

// "<digits>-<rest>" -> digits, rest; false otherwise.
bool IdName(const std::string& name, uint64_t& id, std::string& rest) {
  size_t d = 0;
  while (d < name.size() && std::isdigit(static_cast<unsigned char>(name[d])))
    d++;
  if (d == 0 || d > 18 || d >= name.size() || name[d] != '-')
    return false;
  id = std::stoull(name.substr(0, d));
  rest = name.substr(d);
  return true;
}

} // namespace

std::vector<TreeCase> TreeCases(const std::string& corpus, const fs::path& root,
                                const TreeOptions& opt) {
  std::vector<TreeCase> out;
  std::vector<std::string> files;
  for (auto it = fs::recursive_directory_iterator(root);
       it != fs::recursive_directory_iterator(); ++it)
    if (it->is_regular_file() && !it->is_symlink())
      files.push_back(fs::relative(it->path(), root).generic_string());
  std::sort(files.begin(), files.end());
  const std::string prefix = corpus + "/";
  const auto wanted = [&](const std::string& file, const std::string& op,
                          size_t n) {
    return !opt.file.want ||
           opt.file.want(prefix + file + ":" + op + ":" + std::to_string(n));
  };
  const auto add = [&](const std::string& file, const std::string& op, size_t n,
                       std::vector<Edit> edits) {
    if (!wanted(file, op, n))
      return;
    out.push_back({prefix + file + ":" + op + ":" + std::to_string(n), op,
                   std::move(edits)});
  };
  // Every file's own cases (or a selection: see TreeOptions::everyFile).
  std::set<std::string> kindsSeen;
  for (const std::string& rel : files) {
    const bool binary = IsBinaryName(rel);
    if (!opt.everyFile) {
      const fs::path p(rel);
      uint64_t id;
      std::string rest;
      if (IdName(p.filename().string(), id, rest)) {
        // "features/*.json", "studies/*.result.json", "geometry/*.brep"
        const std::string name = p.filename().string();
        const size_t dot = name.find('.');
        const std::string kind =
            p.parent_path().generic_string() + "/*" +
            (dot == std::string::npos ? "" : name.substr(dot));
        if (!kindsSeen.insert(kind).second)
          continue;
      }
    }
    for (FileCase& c : FileCases(corpus, rel, ReadBytes(root / rel),
                                 binary ? opt.binary : opt.file))
      out.push_back(
          {c.id, c.op, {{Edit::Kind::Write, rel, std::move(c.bytes), {}}}});
  }
  // The directory operators.
  uint64_t maxId = 0;
  for (const std::string& rel : files) {
    uint64_t id;
    std::string rest;
    if (IdName(fs::path(rel).filename().string(), id, rest))
      maxId = std::max(maxId, id);
  }
  for (const std::string& rel : files) {
    add(rel, "rm_file", 0, {{Edit::Kind::Remove, rel, {}, {}}});
    if (!IsBinaryName(rel))
      add(rel, "empty_file", 0, {{Edit::Kind::Write, rel, "", {}}});
    uint64_t id;
    std::string rest;
    const fs::path p(rel);
    if (p.has_parent_path() && IdName(p.filename().string(), id, rest)) {
      const std::string copy =
          (p.parent_path() / (std::to_string(maxId + 1000) + rest))
              .generic_string();
      add(rel, "dup_file", 0,
          {{Edit::Kind::Write, copy, ReadBytes(root / rel), {}}});
    }
    if (!opt.outsideFile.empty() && !IsBinaryName(rel))
      add(rel, "symlink_out", 0,
          {{Edit::Kind::Symlink, rel, {}, opt.outsideFile}});
  }
  // An index entry removed while its file stays; a document swapped for
  // one of another kind; a name that leaves the project; a result whose
  // offset or count lies.
  for (const std::string& rel : files) {
    const std::string name = fs::path(rel).filename().string();
    if (IsBinaryName(rel))
      continue;
    json j;
    try {
      j = json::parse(ReadBytes(root / rel));
    } catch (const std::exception&) {
      continue;
    }
    if (name == "index.json" && j.is_object()) {
      size_t n = 0;
      for (auto it = j.begin(); it != j.end(); ++it) {
        const json& v = it.value();
        if (!v.is_array() || v.empty() ||
            !std::all_of(v.begin(), v.end(),
                         [](const json& e) { return e.is_number_unsigned(); }))
          continue;
        const size_t cap = opt.file.perOperator == 0
                               ? v.size()
                               : std::min(v.size(), opt.file.perOperator);
        for (size_t k = 0; k < cap; k++) {
          const size_t i = k * v.size() / cap; // spread over the list
          json m = j;
          m[it.key()].erase(i);
          add(rel, "unlist", n++,
              {{Edit::Kind::Write, rel, m.dump(2) + "\n", {}}});
        }
      }
      if (!opt.foreignIndex.empty() && rel == "index.json")
        add(rel, "swap_kind", 0,
            {{Edit::Kind::Write, rel, opt.foreignIndex, {}}});
    }
    // Names that leave the project: a document's dir, an asset, a blob.
    {
      std::vector<Node> nodes;
      Walk(j, json::json_pointer(), "", false, json::json_pointer(), false,
           nodes);
      size_t n = 0;
      for (const Node& node : nodes) {
        if (!node.inObject || !node.value->is_string())
          continue;
        const std::string& k = node.key;
        if (k != "dir" && k != "asset" && k != "brep")
          continue;
        for (const char* escape : {"../outside", "/tmp/outside"}) {
          json m = j;
          m[node.ptr] = std::string(escape) + (k == "brep"    ? ".brep"
                                               : k == "asset" ? ".png"
                                                              : "");
          add(rel, "escape", n++,
              {{Edit::Kind::Write, rel, m.dump(2) + "\n", {}}});
        }
      }
    }
    if (name.ends_with(".result.json") || name.ends_with(".projection.json")) {
      std::vector<Node> nodes;
      Walk(j, json::json_pointer(), "", false, json::json_pointer(), false,
           nodes);
      size_t n = 0;
      for (const Node& node : nodes) {
        const json& v = *node.value;
        const bool ref =
            v.is_object() && v.contains("offset") && v.contains("count");
        if (ref) {
          for (const char* field : {"offset", "count"})
            for (const json lie :
                 {json(uint64_t{1} << 40), json(-1), json(3)}) {
              json m = j;
              m[node.ptr][field] = lie;
              add(rel, "bin_lie", n++,
                  {{Edit::Kind::Write, rel, m.dump() + "\n", {}}});
            }
        }
        if (node.inObject && node.key == "bin_bytes")
          for (const json lie :
               {json(v.get<uint64_t>() + 1), json(uint64_t{1} << 62)}) {
            json m = j;
            m[node.ptr] = lie;
            add(rel, "bin_lie", n++,
                {{Edit::Kind::Write, rel, m.dump() + "\n", {}}});
          }
      }
    }
  }
  return out;
}

} // namespace mutate
