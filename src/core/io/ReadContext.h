// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// What a reader is reading, and what a load kept going past
// (docs/versioning/README.md#what-a-load-refuses).
//
// A reader given damaged bytes does one of two things: refuses, naming the
// file and what is wrong, or loads the file correctly. Every reader opens a
// FileContext around each file it reads; an exception that leaves one is
// rethrown with that file's path in front, so a `type_error.302` deep inside
// a feature arrives as `features/7-fillet.json: radius: type must be number,
// but is string` rather than as nlohmann's bare message. The path is
// relative to the project: a project container pushes `parts/<name>/`
// before it loads a part, and the part pushes `features/7-fillet.json`.
//
// A load that keeps going past damage -- a feature file the index does not
// list, a saved study result that does not read -- REPORTS it (Drop), into
// the LoadReport of the CollectDrops alive on that thread. Derived data (a
// study or motion result, a projection, baked geometry) is rebuilt, so its
// drop never blocks a save; anything else does
// (docs/versioning/README.md#a-lossy-load).

#include <nlohmann/json.hpp>

#include <initializer_list>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace readctx {

// An exception that names its file. what() is "<path>: <reason>".
class Error : public std::runtime_error {
public:
  Error(std::string path, std::string reason);
  const std::string& Path() const { return m_path; }
  const std::string& Reason() const { return m_reason; }

private:
  std::string m_path, m_reason;
};

// One segment of the path being read, for as long as it lives: a directory
// ("parts/stand/", ending in '/') or a file ("features/7-fillet.json").
// Thread-local: a reader on a pool thread pushes its own.
class FileContext {
public:
  explicit FileContext(std::string segment);
  ~FileContext();
  FileContext(const FileContext&) = delete;
  FileContext& operator=(const FileContext&) = delete;

private:
  size_t m_depth;
};

// The path being read now, relative to the project ("" at its root).
std::string Path();

// Throws an Error at Path() + `rel`.
[[noreturn]] void Refuse(const std::string& reason,
                         const std::string& rel = {});

// The same as an exception message: "parts/a/index.json: reason".
std::string Named(const std::string& reason, const std::string& rel = {});

// Runs `fn` reading `segment`: any exception leaving it that does not name
// its file yet is rethrown naming this one.
template <class F> decltype(auto) InFile(std::string segment, F&& fn) {
  FileContext ctx(std::move(segment));
  try {
    return fn();
  } catch (const Error&) {
    throw;
  } catch (const std::exception& e) {
    throw Error(Path(), e.what());
  }
}

// Parses `text` as JSON, REFUSING a key written twice in one object (the
// last value would win, and the first would be gone with no word) and a
// value nested deeper than kMaxJsonDepth. The message names the key and
// its JSON pointer; the file is the caller's FileContext.
inline constexpr size_t kMaxJsonDepth = 256;
nlohmann::json ParseStrict(const std::string& text);

// Refuses a key of `j` that is not in `accepted`, naming it and every key
// that is accepted, and an `j` that is not an object at all. `what` names
// the object in the message ("Fillet", "mate", "sketch entity 'arc'").
void RefuseUnknownKeys(const nlohmann::json& j,
                       std::initializer_list<std::string_view> accepted,
                       std::string_view what);
// The same with the list built at run time (a feature's common keys plus
// its type's).
void RefuseUnknownKeys(const nlohmann::json& j,
                       const std::vector<std::string_view>& accepted,
                       std::string_view what);

// A whole number >= 0, as an id or a count is. A file parsed from text
// holds one as unsigned, but JSON built in code (a test, an RPC handler
// assembling a feature) holds `1` as a signed integer: both are whole.
inline bool IsWhole(const nlohmann::json& v) {
  return v.is_number_unsigned() ||
         (v.is_number_integer() && v.get<int64_t>() >= 0);
}

// A key a reader accepts and does not use -- a written type tag
// ("type": "Mate"), a counter an older build kept -- still has to be what
// it says: anything else in it would be dropped on the next save with no
// word. RequireTag: absent, or exactly `tag`. RequireCount: absent, or a
// whole number >= 0.
void RequireTag(const nlohmann::json& j, std::string_view tag,
                std::string_view what);
void RequireCount(const nlohmann::json& j, std::string_view key,
                  std::string_view what);

// What a load kept going past.
struct Drop {
  std::string path;    // relative to the project: "features/12-fillet.json"
  std::string pointer; // a JSON pointer inside it; "" for the whole file
  std::string reason;
  bool derived = false; // a result, a projection, baked geometry: rebuilt
};

struct LoadReport {
  std::vector<Drop> drops;
  bool Empty() const { return drops.empty(); }
  // Lost something the user wrote: a save over the source would lose it
  // for good. Derived drops alone are not lossy.
  bool Lossy() const;
  // One line per drop, for a log or a refusal.
  std::string Summary(bool lossyOnly = false) const;
  // [{path, pointer, reason, derived}]: open_project's `dropped`.
  nlohmann::json ToJson() const;
  void Append(const LoadReport& other);
};

// Records a drop at Path() (+ `rel`), into the innermost CollectDrops on
// this thread, and logs it. With no collector alive it is only logged.
void Dropped(const std::string& pointer, const std::string& reason,
             bool derived, const std::string& rel = {});

// Collects, while it lives, every drop recorded on this thread.
class CollectDrops {
public:
  CollectDrops();
  ~CollectDrops();
  CollectDrops(const CollectDrops&) = delete;
  CollectDrops& operator=(const CollectDrops&) = delete;
  LoadReport Take();

private:
  LoadReport m_report;
  CollectDrops* m_outer = nullptr;
  friend void Dropped(const std::string&, const std::string&, bool,
                      const std::string&);
};

} // namespace readctx
