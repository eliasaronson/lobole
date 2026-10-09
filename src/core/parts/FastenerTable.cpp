// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/parts/FastenerTable.h"

#include "common/Logger.h"
#include "common/RuntimePaths.h"
#include "core/ThreadStandards.h"
#include "core/parts/FastenerProfile.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <mutex>
#include <sstream>
#include <vector>

namespace parts {

namespace {

std::mutex g_mutex;
std::map<std::string, TableFile> g_cache;
std::string g_dataDirOverride;

std::filesystem::path DataDir() {
  if (!g_dataDirOverride.empty())
    return g_dataDirOverride;
  return RuntimePaths::PartsDir();
}

// Strip surrounding double quotes and whitespace. These files quote their
// headers and size keys but not their numbers, so this runs on every field
// rather than being guessed at per column.
std::string Unquote(std::string s) {
  const auto notSpace = [](unsigned char c) { return !std::isspace(c); };
  s.erase(s.begin(), std::find_if(s.begin(), s.end(), notSpace));
  s.erase(std::find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());
  if (s.size() >= 2 && s.front() == '"' && s.back() == '"')
    s = s.substr(1, s.size() - 2);
  return s;
}

std::vector<std::string> SplitCsv(const std::string& line) {
  // No embedded commas inside the quoted fields of these files — they are
  // identifiers and numbers — so a plain split is correct and a real CSV
  // parser would be a dependency bought for a case that does not occur. If a
  // future table needs one, this is the single place that changes.
  std::vector<std::string> out;
  std::string field;
  std::istringstream in(line);
  while (std::getline(in, field, ','))
    out.push_back(Unquote(field));
  // A line ending in the delimiter has a trailing EMPTY field, and getline
  // does not produce one — it hits EOF and stops. Dropping it made a row whose
  // last column is blank look like a row with a column missing, which the
  // field-count check then refused. A blank last column is exactly the shape a
  // standard that does not define its final dimension at some size takes, so
  // this is a real row, not a malformed one. Caught by a test that wrote one.
  if (!line.empty() && line.back() == ',')
    out.emplace_back();
  return out;
}

std::string Lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  return s;
}

// The numeric part of a size key: 6 from "M6", 1.6 from "M1.6", 20 from "T20".
// nullopt for anything that is not <letters><number>.
std::optional<double> KeyNumber(const std::string& raw) {
  // "(M3.5)" is M3.5, marked non-preferred. Stripping the marker here is what
  // lets a refusal name it as a neighbour: without this, ISO 4032's
  // parenthesised sizes were invisible to the nearest-stocked search, so
  // "M3.6 is not stocked" could not offer the M3.5 sitting right beside it.
  std::string key = raw;
  if (key.size() >= 2 && key.front() == '(' && key.back() == ')')
    key = key.substr(1, key.size() - 2);
  size_t i = 0;
  while (i < key.size() && (std::isalpha(static_cast<unsigned char>(key[i]))))
    i++;
  if (i == 0 || i >= key.size())
    return std::nullopt;
  try {
    size_t consumed = 0;
    const double value = std::stod(key.substr(i), &consumed);
    if (consumed != key.size() - i)
      return std::nullopt;
    return value;
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

// What a table stocks, for a refusal that says what to ask for INSTEAD.
//
// The neighbours first, when the key is a size that falls between two stocked
// ones: "M7" is not in ISO 4762 and what the caller needs to hear is "M6 or
// M8", not the ends of an 84-entry list. Truncating the list alone dropped
// exactly the two sizes that answer the question, which is what a test caught.
// (docs/scripting/rpc.md#refusals: a refusal carries one concrete correction.)
std::string KeyList(const Table& table, const std::string& wanted) {
  const std::vector<std::string>& keys = table.order;
  std::string neighbours;
  if (const std::optional<double> want = KeyNumber(wanted)) {
    const std::string* below = nullptr;
    const std::string* above = nullptr;
    for (const std::string& key : keys) {
      const std::optional<double> value = KeyNumber(key);
      if (!value)
        continue;
      if (*value < *want && (below == nullptr || *value > *KeyNumber(*below)))
        below = &key;
      if (*value > *want && (above == nullptr || *value < *KeyNumber(*above)))
        above = &key;
    }
    if (below && above)
      neighbours = "the nearest stocked are " + *below + " and " + *above +
                   "; all sizes are ";
    else if (below)
      neighbours = "the largest stocked is " + *below + "; all sizes are ";
    else if (above)
      neighbours = "the smallest stocked is " + *above + "; all sizes are ";
  }

  std::string out;
  if (keys.size() <= 12) {
    for (size_t i = 0; i < keys.size(); i++)
      out += (i ? ", " : "") + keys[i];
    return neighbours + out;
  }
  for (size_t i = 0; i < 6; i++)
    out += (i ? ", " : "") + keys[i];
  out += ", … , ";
  for (size_t i = keys.size() - 3; i < keys.size(); i++)
    out += keys[i] + (i + 1 < keys.size() ? ", " : "");
  return neighbours + out;
}

} // namespace

const std::string* Table::Key(const std::string& key) const {
  const auto exact = rows.find(key);
  if (exact != rows.end())
    return &exact->first;
  const auto marked = rows.find("(" + key + ")");
  if (marked != rows.end())
    return &marked->first;
  // And the other way, so a caller echoing a key back out of `order` is not
  // refused for quoting the marker it was given.
  if (key.size() >= 2 && key.front() == '(' && key.back() == ')') {
    const auto bare = rows.find(key.substr(1, key.size() - 2));
    if (bare != rows.end())
      return &bare->first;
  }
  return nullptr;
}

const std::map<std::string, double>* Table::Find(const std::string& key) const {
  const std::string* resolved = Key(key);
  return resolved == nullptr ? nullptr : &rows.at(*resolved);
}

const Table* TableFile::ById(const std::string& idSuffix) const {
  const std::string want = Lower(idSuffix);
  for (const Table& t : tables)
    if (Lower(t.id).size() >= want.size() &&
        Lower(t.id).compare(Lower(t.id).size() - want.size(), want.size(),
                            want) == 0)
      return &t;
  return nullptr;
}

std::optional<TableFile> ParseFile(const std::string& text,
                                   const std::string& stem,
                                   std::string* error) {
  const auto fail = [&](const std::string& why) -> std::optional<TableFile> {
    if (error)
      *error = why;
    return std::nullopt;
  };

  TableFile out;
  std::istringstream in(text);
  std::string line;
  size_t lineNo = 0;

  // One pass, switching tables on a blank line. A file may hold several
  // (iso4762def.csv holds its dimensions, its length series and its per-size
  // length ranges), and a table may or may not name itself: a line with ONE
  // field is an id, a line with several is a header. Both shapes occur in the
  // vendored data, so both are read rather than one being normalised away —
  // the files are kept byte-identical to upstream so a re-vendor can diff.
  Table current;
  bool haveHeader = false;
  const auto finish = [&]() {
    if (haveHeader && !current.rows.empty())
      out.tables.push_back(std::move(current));
    current = Table{};
    haveHeader = false;
  };

  while (std::getline(in, line)) {
    lineNo++;
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    if (line.empty()) {
      finish();
      continue;
    }
    if (line[0] == '#') {
      // Our provenance header -- and the one line of it that is data: the
      // unit the numbers are in.
      if (line.find("units: inch") != std::string::npos)
        out.mmPerUnit = 25.4;
      continue;
    }

    const std::vector<std::string> fields = SplitCsv(line);
    if (!haveHeader) {
      if (fields.size() == 1) {
        current.id = fields[0]; // a table that names itself
        continue;
      }
      current.columns = fields;
      haveHeader = true;
      if (current.id.empty())
        current.id = stem; // a table that does not
      if (current.columns.size() < 2)
        return fail("table \"" + current.id + "\" line " +
                    std::to_string(lineNo) + " has " +
                    std::to_string(current.columns.size()) +
                    " columns; a key and at least one value are required");
      continue;
    }

    if (fields.size() != current.columns.size())
      return fail("table \"" + current.id + "\" line " +
                  std::to_string(lineNo) + ": " +
                  std::to_string(fields.size()) + " fields against " +
                  std::to_string(current.columns.size()) + " columns");
    const std::string key = fields[0];
    if (key.empty())
      return fail("table \"" + current.id + "\" line " +
                  std::to_string(lineNo) + " has an empty key");
    if (current.rows.count(key))
      return fail("table \"" + current.id + "\" lists " + key +
                  " twice — a lookup would be ambiguous");

    std::map<std::string, double> row;
    std::map<std::string, std::string> text;
    for (size_t i = 1; i < fields.size(); i++) {
      // Absent, not zero: a blank means the standard does not define that
      // dimension at this size, and zero is a real dimension.
      if (fields[i].empty())
        continue;
      text[current.columns[i]] = fields[i];
      try {
        size_t consumed = 0;
        const double value = std::stod(fields[i], &consumed);
        if (consumed != fields[i].size())
          continue; // a name, not a dimension — see Table::text
        row[current.columns[i]] = value * out.mmPerUnit;
      } catch (const std::exception&) {
        // Likewise. A dimension column that turns out to hold a name is not a
        // malformed file: `iso14579def.csv` puts the driver size ("T30")
        // between two real dimensions, and refusing the file over it would
        // mean no hexalobular drive at all. It is absent from `rows`, so a
        // caller asking for it as a millimetre gets the same refusal a
        // missing dimension gives, naming the column.
      }
    }
    current.order.push_back(key);
    current.rows[key] = std::move(row);
    current.text[key] = std::move(text);
  }
  finish();

  if (out.tables.empty())
    return fail("no table with any rows — the file is empty, all comments, or "
                "a header with nothing under it");
  return out;
}

const TableFile* Load(const std::string& named, std::string* error) {
  // A standard can be named the way a person names it ("iso4762") or the way
  // the vendored file is ("iso4762def"). The files keep UPSTREAM's names so a
  // re-vendor can diff, and upstream calls ISO 4017's `iso4017head` — which
  // is not a thing anybody would think to type. So the profile table resolves
  // one to the other, here, once, rather than at every call site.
  const Profile* profile = ProfileFor(named);
  const std::string stem = profile ? profile->stem : named;

  std::lock_guard<std::mutex> lock(g_mutex);
  const auto cached = g_cache.find(stem);
  if (cached != g_cache.end())
    return &cached->second;

  const std::filesystem::path path = DataDir() / (stem + ".csv");
  std::ifstream file(path);
  if (!file) {
    if (error)
      *error = "no table for \"" + stem + "\": " + path.string() +
               " is not readable. A development build stages assets/parts/ "
               "beside the executable; an installed one uses "
               "share/lobole/parts (DESIGNER_DATA_DIR overrides both).";
    return nullptr;
  }
  std::stringstream buffer;
  buffer << file.rdbuf();

  std::string why;
  std::optional<TableFile> parsed = ParseFile(buffer.str(), stem, &why);
  if (!parsed) {
    if (error)
      *error = path.string() + ": " + why;
    return nullptr;
  }
  LOG_INFO("Loaded standard-parts file {} ({} tables, {} sizes)", stem,
           parsed->tables.size(), parsed->tables.front().rows.size());
  return &(g_cache[stem] = std::move(*parsed));
}

std::optional<std::map<std::string, double>> Dimensions(const std::string& stem,
                                                        const std::string& size,
                                                        std::string* error) {
  const TableFile* file = Load(stem, error);
  if (file == nullptr)
    return std::nullopt;
  // Table 0 is the dimensions: the length series and the ranges follow it.
  const Table& table = file->tables.front();
  const std::map<std::string, double>* row = table.Find(size);
  if (row == nullptr) {
    if (error)
      *error =
          table.id + " does not stock " + size + " — " + KeyList(table, size);
    return std::nullopt;
  }
  return *row;
}

std::vector<double> NominalLengths(const std::string& stem,
                                   std::string* error) {
  const TableFile* file = Load(stem, error);
  if (file == nullptr)
    return {};
  const Table* lengths = file->ById("length");
  if (lengths == nullptr) {
    if (error)
      *error = stem + " carries no length series — it is a standard with no "
                      "length (a nut or a washer)";
    return {};
  }
  std::vector<double> out;
  out.reserve(lengths->order.size());
  for (const std::string& key : lengths->order) {
    try {
      out.push_back(std::stod(key) * file->mmPerUnit);
    } catch (const std::exception&) {
      if (error)
        *error = lengths->id + " lists a non-numeric length \"" + key + "\"";
      return {};
    }
  }
  std::sort(out.begin(), out.end());
  return out;
}

std::optional<std::pair<double, double>> LengthRange(const std::string& stem,
                                                     const std::string& size,
                                                     std::string* error) {
  const TableFile* file = Load(stem, error);
  if (file == nullptr)
    return std::nullopt;
  const Table* range = file->ById("range");
  if (range == nullptr)
    return std::nullopt; // no range table: not an error, just not a screw
  const std::map<std::string, double>* row = range->Find(size);
  if (row == nullptr)
    return std::nullopt;
  // The columns are the range's own; take the two extremes rather than
  // assuming their names, since one file spells them Min/Max and the shape of
  // a future one is not ours to promise.
  double lo = 0.0, hi = 0.0;
  bool first = true;
  for (const auto& [column, value] : *row) {
    lo = first ? value : std::min(lo, value);
    hi = first ? value : std::max(hi, value);
    first = false;
  }
  if (first)
    return std::nullopt;
  return std::make_pair(lo, hi);
}

// How far past a nominal length a derived grip may sit and still snap to it.
//
// Not an engineering tolerance — a NUMERIC one. A grip is the sum of boolean
// intersection distances, so two 10 mm plates measure 20.0000005 rather than
// 20, and a bare `<` comparison then rejected the 20 mm screw and offered a
// 25: five millimetres of error out of half a micron of noise, on the most
// ordinary joint there is. A micron is far below anything anybody models
// deliberately and far above float noise, and a real 20.5 mm grip still goes
// to 25.
constexpr double kSnapTol = 1.0e-3;

std::optional<double> SnapLength(const std::string& stem,
                                 const std::string& size, double required,
                                 std::string* error) {
  const std::vector<double> lengths = NominalLengths(stem, error);
  if (lengths.empty())
    return std::nullopt;

  // The per-size range narrows the series: an M6 is not made in every length
  // an M64 is, and offering one is the same substitution as inventing a size.
  const std::optional<std::pair<double, double>> range =
      LengthRange(stem, size, nullptr);

  for (const double length : lengths) {
    if (length < required - kSnapTol)
      continue;
    if (range && (length < range->first || length > range->second))
      continue;
    return length;
  }
  if (error) {
    std::ostringstream why;
    why.precision(4);
    why << "no nominal length reaches " << required << " mm";
    if (range)
      why << " — " << size << " is made from " << range->first << " to "
          << range->second << " mm";
    else
      why << " — the series runs " << lengths.front() << " to "
          << lengths.back() << " mm";
    *error = why.str();
  }
  return std::nullopt;
}

std::optional<double> SnapLengthDown(const std::string& stem,
                                     const std::string& size, double available,
                                     std::string* error) {
  const std::vector<double> lengths = NominalLengths(stem, error);
  if (lengths.empty())
    return std::nullopt;
  const std::optional<std::pair<double, double>> range =
      LengthRange(stem, size, nullptr);
  std::optional<double> best;
  for (const double length : lengths) {
    if (length > available + kSnapTol)
      break;
    if (range && (length < range->first || length > range->second))
      continue;
    best = length;
  }
  if (!best && error) {
    std::ostringstream why;
    why.precision(4);
    why << "no " << size << " is made short enough to fit " << available
        << " mm";
    if (range)
      why << " — the shortest is " << range->first << " mm";
    *error = why.str();
  }
  return best;
}

std::optional<std::pair<std::string, std::string>>
ThreadForClearance(double diameter, std::string* error) {
  const TableFile* file = Load("clearance_hole_sizes", error);
  if (file == nullptr)
    return std::nullopt;
  const Table& table = file->tables.front();
  // Exact to a hundredth of a millimetre: these are drilled sizes, and the
  // tolerance is there to absorb a float round trip, not to let a 6.5 mm hole
  // pass as an M6 clearance.
  constexpr double kTol = 0.005;
  // EVERY match, not the first. 46 of the 111 diameters in this table are a
  // clearance for two different threads — 11.0 mm is an M9 Loose and an M10
  // Normal — and returning whichever came first in table order silently
  // answered every one of them with the SMALLER thread on the LOOSER fit.
  // For 11 mm that was M9, which no standard stocks, so a perfectly ordinary
  // M10 clearance hole came back as a baffling refusal about M9.
  std::vector<std::pair<std::string, std::string>> matches;
  for (const std::string& size : table.order)
    for (const auto& [fit, value] : table.rows.at(size))
      if (std::abs(value - diameter) < kTol)
        matches.emplace_back(size, fit);
  if (matches.size() == 1)
    return matches.front();
  if (matches.size() > 1) {
    if (error) {
      std::ostringstream why;
      why.precision(4);
      why << diameter
          << " mm is a clearance hole for more than one "
             "thread — ";
      for (size_t i = 0; i < matches.size(); ++i)
        why << (i == 0                    ? ""
                : i + 1 == matches.size() ? " and "
                                          : ", ")
            << matches[i].first << " " << matches[i].second;
      why << ". Which one it is cannot be read off the hole, so say it: pass "
             "`size` (\""
          << matches.back().first
          << "\", say), or drill it with a Hole feature, which records the "
             "fit it was made for and is never ambiguous.";
      *error = why.str();
    }
    return std::nullopt;
  }
  if (error) {
    std::ostringstream why;
    why.precision(4);
    why << "no standard clearance hole is " << diameter
        << " mm across — that is not a size anything is drilled to, so which "
           "fastener it is for cannot be read from it. Say the size outright.";
    *error = why.str();
  }
  return std::nullopt;
}

std::optional<double> WidestClearance(const std::string& size,
                                      std::string* error) {
  const auto fits = Dimensions("clearance_hole_sizes", size, error);
  if (!fits)
    return std::nullopt;
  std::optional<double> widest;
  for (const auto& [fit, value] : *fits)
    widest = widest ? std::max(*widest, value) : value;
  if (!widest && error)
    *error = "the clearance table has no fit for " + size;
  return widest;
}

std::optional<double> NominalDiameter(const std::string& size) {
  if (const auto metric = KeyNumber(size))
    return metric;
  // A unified size ("1/4-20", "#10-24") is its thread's major diameter.
  // The inch tables key a size as a supplier writes it, "1/4-20"; the
  // thread table names its series as well.
  for (const std::string& name : {size, size + " UNC"})
    if (const auto thread = threads::Lookup(name);
        thread && thread->series != threads::Series::Metric)
      return thread->majorDia;
  return std::nullopt;
}

std::optional<std::string> TextField(const std::string& stem,
                                     const std::string& size,
                                     const std::string& column,
                                     std::string* error) {
  const TableFile* file = Load(stem, error);
  if (file == nullptr)
    return std::nullopt;
  const Table& table = file->tables.front();
  const std::string* key = table.Key(size);
  if (key == nullptr) {
    if (error)
      *error =
          table.id + " does not stock " + size + " — " + KeyList(table, size);
    return std::nullopt;
  }
  const auto row = table.text.find(*key);
  if (row == table.text.end())
    return std::nullopt;
  const auto field = row->second.find(column);
  if (field == row->second.end()) {
    if (error)
      *error = table.id + " " + size + " has no " + column + " column";
    return std::nullopt;
  }
  return field->second;
}

std::string FitName(Fit fit) {
  switch (fit) {
  case Fit::Close:
    return "close";
  case Fit::Normal:
    return "normal";
  case Fit::Loose:
    return "loose";
  case Fit::Tapped:
    return "tapped";
  }
  return "normal";
}

std::optional<Fit> FitFromName(const std::string& name) {
  if (name == "close")
    return Fit::Close;
  if (name == "normal")
    return Fit::Normal;
  if (name == "loose")
    return Fit::Loose;
  if (name == "tapped")
    return Fit::Tapped;
  return std::nullopt;
}

std::optional<double> CoarsePitch(const std::string& size, std::string* error) {
  // ISO 4762 carries `P` for every size it stocks, and the coarse pitch of a
  // metric thread does not depend on which fastener it is on — so this reads
  // the standard already vendored rather than tabulating the same numbers a
  // second time somewhere they could disagree.
  const auto dims = Dimensions("iso4762", size, error);
  if (!dims)
    return std::nullopt;
  const auto it = dims->find("P");
  if (it == dims->end()) {
    if (error)
      *error = size + " has no pitch in the vendored tables";
    return std::nullopt;
  }
  return it->second;
}

std::optional<double> HoleDiameter(const std::string& thread, Fit fit,
                                   double pitch, std::string* error) {
  if (fit != Fit::Tapped) {
    const auto fits = Dimensions("clearance_hole_sizes", thread, error);
    if (!fits)
      return std::nullopt;
    // The column names ARE the fit names, capitalised as the table writes
    // them. Matching on the table's own words rather than an index means a
    // re-vendor that adds a column cannot silently shift the meaning.
    const std::string column = fit == Fit::Close   ? "Close"
                               : fit == Fit::Loose ? "Loose"
                                                   : "Normal";
    const auto it = fits->find(column);
    if (it == fits->end()) {
      if (error)
        *error = "the clearance table has no " + column + " fit for " + thread;
      return std::nullopt;
    }
    return it->second;
  }

  // A unified thread ("3/8-16 UNC") names its pitch in its designation, and
  // its tap drill is the number, letter or fraction drill the thread table
  // carries — the vendored tap table is metric only.
  if (const auto unified = threads::Lookup(thread);
      unified && unified->series != threads::Series::Metric) {
    if (pitch > 0.0 && std::abs(pitch - unified->pitch) > 1e-6) {
      if (error) {
        std::ostringstream msg;
        msg << thread << " has a pitch of " << unified->pitch
            << " mm (its TPI); a pitch of " << pitch
            << " is a different thread";
        *error = msg.str();
      }
      return std::nullopt;
    }
    return threads::TapDrill(*unified);
  }

  // Tapped: the drill depends on the PITCH as well as the size, and the two
  // are one key in the table ("M6-1"). A hole tapped to the wrong one taps
  // badly rather than looking wrong, which is why the pitch is derived from
  // the standard when it is not given rather than assumed to be 1.
  double p = pitch;
  if (p <= 0.0) {
    const std::optional<double> coarse = CoarsePitch(thread, error);
    if (!coarse)
      return std::nullopt;
    p = *coarse;
  }
  std::ostringstream key;
  key << thread << "-";
  // "M6-1", not "M6-1.000000": the table's keys are written the way a person
  // writes a pitch.
  if (std::abs(p - std::round(p)) < 1e-9)
    key << static_cast<long long>(std::llround(p));
  else
    key << p;
  const auto row = Dimensions("tap_hole_sizes", key.str(), error);
  if (!row)
    return std::nullopt;
  const auto it = row->find("Soft");
  if (it == row->end()) {
    if (error)
      *error = "the tap table has no drill for " + key.str();
    return std::nullopt;
  }
  return it->second;
}

std::vector<std::string> ClearanceSizes(std::string* error) {
  const TableFile* file = Load("clearance_hole_sizes", error);
  return file == nullptr ? std::vector<std::string>{}
                         : file->tables.front().order;
}

void SetDataDirForTesting(const std::string& dir) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_dataDirOverride = dir;
  g_cache.clear();
}

} // namespace parts
