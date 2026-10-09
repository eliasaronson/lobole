// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/catalog/Catalog.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <set>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace catalog {
namespace {
constexpr size_t kMaxBytes = 8 * 1024 * 1024;
void Require(bool yes, const std::string& why) {
  if (!yes)
    throw std::runtime_error("catalog: " + why);
}
std::string Trim(std::string s) {
  const auto a = s.find_first_not_of(" \t\r\n");
  return a == std::string::npos
             ? ""
             : s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
}
std::string Text(const Json& j, const char* key) {
  Require(j.contains(key) && j.at(key).is_string(),
          std::string(key) + " must be text");
  auto s = Trim(j.at(key).get<std::string>());
  Require(!s.empty() && s.size() <= 2048,
          std::string(key) + " must be nonempty (max 2048 bytes)");
  return s;
}
const std::vector<std::string> fields = {
    "bore_diameter", "outer_diameter",   "width",       "shaft_min",
    "shaft_max",     "housing_min",      "housing_max", "shoulder_min",
    "shoulder_max",  "fillet_radius_max"};
bool NumericField(const std::string& s) {
  return std::find(fields.begin(), fields.end(), s) != fields.end();
}
// RFC-style quoted fields, including embedded newlines and escaped quotes.
Json Csv(std::string_view input) {
  std::vector<std::vector<std::string>> rows;
  std::vector<std::string> row;
  std::string cell;
  bool quoted = false, closed = false;
  auto endCell = [&] {
    row.push_back(Trim(cell));
    cell.clear();
    closed = false;
  };
  auto endRow = [&] {
    endCell();
    if (row.size() != 1 || !row[0].empty())
      rows.push_back(std::move(row));
    row.clear();
  };
  for (size_t i = 0; i < input.size(); ++i) {
    const char c = input[i];
    if (quoted) {
      if (c == '"') {
        if (i + 1 < input.size() && input[i + 1] == '"') {
          cell += '"';
          ++i;
        } else {
          quoted = false;
          closed = true;
        }
      } else
        cell += c;
    } else if (c == ',')
      endCell();
    else if (c == '\n' || c == '\r') {
      endRow();
      if (c == '\r' && i + 1 < input.size() && input[i + 1] == '\n')
        ++i;
    } else if (c == '"') {
      Require(cell.empty() && !closed, "malformed CSV quote");
      quoted = true;
    } else {
      Require(!closed || c == ' ' || c == '\t',
              "characters after closing CSV quote");
      if (!closed)
        cell += c;
    }
  }
  Require(!quoted, "unterminated CSV quote");
  if (!cell.empty() || !row.empty() || closed)
    endRow();
  Require(rows.size() >= 2, "CSV needs a header and records");
  std::set<std::string> headers;
  for (const auto& h : rows[0])
    Require(!h.empty() && headers.insert(h).second,
            "empty or duplicate CSV header");
  Json out = Json::array();
  for (size_t i = 1; i < rows.size(); ++i) {
    Require(rows[i].size() == rows[0].size(),
            "CSV row " + std::to_string(i + 1) + " has wrong column count");
    Json r = Json::object();
    for (size_t c = 0; c < rows[0].size(); ++c)
      r[rows[0][c]] = rows[i][c];
    out.push_back(std::move(r));
  }
  return out;
}
double Number(const Json& v, const std::string& field, bool allowZero = false) {
  double n = 0;
  if (v.is_number())
    n = v.get<double>();
  else {
    Require(v.is_string(), field + " must be numeric");
    const auto s = Trim(v.get<std::string>());
    size_t used = 0;
    try {
      n = std::stod(s, &used);
    } catch (...) {
      Require(false, field + " is not a number");
    }
    Require(used == s.size(),
            field + " has trailing text; specify units separately");
  }
  Require(std::isfinite(n) && (allowZero ? n >= 0 : n > 0),
          field + " must be finite and " +
              (allowZero ? "nonnegative" : "positive"));
  return n;
}
Json Normalize(Json rows, double scale, const Json& mapping) {
  Require(rows.is_array() && !rows.empty() && rows.size() <= 10000,
          "expected 1..10000 records");
  Require(mapping.is_object(),
          "mapping must map canonical fields to source columns");
  std::set<std::string> mapped;
  for (const auto& [k, v] : mapping.items()) {
    Require(k == "designation" || k == "step_reference" || NumericField(k),
            "unknown mapped field: " + k);
    Require(v.is_string() && mapped.insert(v.get<std::string>()).second,
            "mapping columns must be unique text");
  }
  Json result = Json::array();
  std::set<std::string> ids;
  for (size_t i = 0; i < rows.size(); ++i) {
    const auto& row = rows[i];
    Require(row.is_object(), "record must be an object");
    Json record = Json::object(), unsupported = Json::array();
    std::set<std::string> consumed;
    auto take = [&](const std::string& field) -> const Json* {
      std::string column = mapping.value(field, field);
      if (!row.contains(column))
        return nullptr;
      consumed.insert(column);
      const auto& value = row.at(column);
      if (value.is_null() ||
          (value.is_string() && Trim(value.get<std::string>()).empty()))
        return nullptr;
      return &value;
    };
    const auto* id = take("designation");
    Require(id && id->is_string(),
            "record " + std::to_string(i + 1) + " needs a text designation");
    std::string designation = Trim(id->get<std::string>());
    Require(!designation.empty() && ids.insert(designation).second,
            "empty or duplicate designation: " + designation);
    record["designation"] = designation;
    for (const auto& field : fields)
      if (const auto* v = take(field)) {
        const double n = Number(*v, designation + "." + field,
                                field == "fillet_radius_max") *
                         scale;
        Require(std::isfinite(n), "dimension overflow");
        record[field] = n;
      }
    for (const auto* field : {"bore_diameter", "outer_diameter", "width"})
      Require(record.contains(field), designation + " missing " + field);
    Require(record["outer_diameter"].get<double>() >
                record["bore_diameter"].get<double>(),
            designation + " outer diameter must exceed bore");
    for (const auto* base : {"shaft", "housing", "shoulder"}) {
      const std::string lo = std::string(base) + "_min",
                        hi = std::string(base) + "_max";
      Require(record.contains(lo) == record.contains(hi),
              designation + " needs both " + lo + " and " + hi);
      if (record.contains(lo))
        Require(record[lo].get<double>() <= record[hi].get<double>(),
                designation + " reversed " + base + " limits");
    }
    if (const auto* step = take("step_reference")) {
      Require(step->is_string(), "step_reference must be text");
      record["step_reference"] =
          *step; // Never fetched or embedded automatically.
    }
    for (const auto& [k, v] : row.items())
      if (!consumed.contains(k))
        unsupported.push_back(k);
    record["unsupported_fields"] = unsupported;
    result.push_back(std::move(record));
  }
  std::sort(result.begin(), result.end(), [](const Json& a, const Json& b) {
    return a["designation"].get<std::string>() <
           b["designation"].get<std::string>();
  });
  return result;
}
} // namespace

std::string Sha256(std::string_view input) {
  static constexpr uint32_t k[64] = {
      0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
      0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
      0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
      0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
      0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
      0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
      0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
      0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
      0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
      0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
      0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
  std::vector<uint8_t> bytes(input.begin(), input.end());
  const uint64_t bits = bytes.size() * 8ULL;
  bytes.push_back(0x80);
  while (bytes.size() % 64 != 56)
    bytes.push_back(0);
  for (int i = 7; i >= 0; --i)
    bytes.push_back(static_cast<uint8_t>(bits >> (i * 8)));
  std::array<uint32_t, 8> h = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                               0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  for (size_t off = 0; off < bytes.size(); off += 64) {
    uint32_t w[64];
    for (int i = 0; i < 16; ++i) {
      const auto p = off + 4 * i;
      w[i] = uint32_t(bytes[p]) << 24 | uint32_t(bytes[p + 1]) << 16 |
             uint32_t(bytes[p + 2]) << 8 | bytes[p + 3];
    }
    for (int i = 16; i < 64; ++i) {
      const auto a = w[i - 15], b = w[i - 2];
      w[i] = w[i - 16] + (std::rotr(a, 7) ^ std::rotr(a, 18) ^ (a >> 3)) +
             w[i - 7] + (std::rotr(b, 17) ^ std::rotr(b, 19) ^ (b >> 10));
    }
    auto [a, b, c, d, e, f, g, hh] = h;
    for (int i = 0; i < 64; ++i) {
      const uint32_t t1 =
          hh + (std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25)) +
          ((e & f) ^ (~e & g)) + k[i] + w[i];
      const uint32_t t2 =
          (std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22)) +
          ((a & b) ^ (a & c) ^ (b & c));
      hh = g;
      g = f;
      f = e;
      e = d + t1;
      d = c;
      c = b;
      b = a;
      a = t1 + t2;
    }
    std::array<uint32_t, 8> v = {a, b, c, d, e, f, g, hh};
    for (size_t i = 0; i < 8; ++i)
      h[i] += v[i];
  }
  std::ostringstream out;
  out << std::hex << std::setfill('0');
  for (auto v : h)
    out << std::setw(8) << v;
  return out.str();
}
Json Preview(std::string_view content, const std::string& format,
             const Json& options) {
  Require(content.size() <= kMaxBytes, "file exceeds 8 MiB");
  Require(options.is_object(), "options must be an object");
  const std::set<std::string> allowed = {
      "id", "revision", "source", "license", "units", "storage", "mapping"};
  for (const auto& [k, v] : options.items())
    Require(allowed.contains(k), "unknown option: " + k);
  if (format == "json") {
    const auto package = Json::parse(content);
    if (package.is_object()) {
      Require(package.value("format", std::string{}) == "designer-catalog" &&
                  package.value("version", 0) == 1,
              "unsupported catalog package");
      auto entries = Deserialize(package.at("catalogs"));
      Require(entries.size() == 1, "package must contain one catalog revision");
      auto entry = entries[0];
      for (const auto* key : {"id", "revision", "source", "license"})
        if (options.contains(key) && options.at(key) != "")
          Require(options.at(key) == entry.at(key),
                  "package metadata conflicts with import options");
      Require(!options.contains("mapping") || options.at("mapping").empty(),
              "packages already have canonical fields");
      Require(!options.contains("units") || options.at("units") == "mm",
              "packages use mm");
      const auto storage = options.value("storage", std::string("reference"));
      Require(storage == "reference" || storage == "embedded",
              "invalid storage");
      Require(storage != "embedded" || entry.contains("records"),
              "cannot embed unresolved reference");
      entry["storage"] = storage;
      return entry;
    }
  }
  Json out = {{"schema", "bearing-v1"}};
  for (const auto* key : {"id", "revision", "source", "license"})
    out[key] = Text(options, key);
  const auto units = Text(options, "units");
  Require(units == "mm" || units == "in", "units must be mm or in");
  const auto storage = options.value("storage", std::string("reference"));
  Require(storage == "reference" || storage == "embedded",
          "storage must be reference or embedded");
  Json rows;
  if (format == "csv")
    rows = Csv(content);
  else if (format == "json")
    rows = Json::parse(content);
  else
    Require(false, "format must be csv or json");
  out["records"] = Normalize(rows, units == "in" ? 25.4 : 1.0,
                             options.value("mapping", Json::object()));
  out["hash"] = Sha256(out["records"].dump());
  out["units"] = "mm";
  out["storage"] = storage;
  out["record_count"] = out["records"].size();
  return out;
}
Json ReadFile(const std::string& path, const Json& options) {
  Require(std::filesystem::is_regular_file(path), "not a regular file");
  Require(std::filesystem::file_size(path) <= kMaxBytes, "file exceeds 8 MiB");
  std::ifstream in(path, std::ios::binary);
  Require(bool(in), "cannot read file");
  std::string content((std::istreambuf_iterator<char>(in)), {});
  auto ext = std::filesystem::path(path).extension().string();
  return Preview(content,
                 ext == ".csv"    ? "csv"
                 : ext == ".json" ? "json"
                                  : "",
                 options);
}
Json Summary(const Json& entry) {
  Json j = Json::object();
  for (const auto& [key, value] : entry.items())
    if (key != "records")
      j[key] = value;
  j["resolved"] = entry.contains("records");
  return j;
}
Json Serialize(const Json& entries) {
  Json out = Json::array();
  for (const auto& e : entries) {
    Json stored = Json::object();
    for (const auto& [key, value] : e.items())
      if (key != "records" || e.at("storage") == "embedded")
        stored[key] = value;
    out.push_back(std::move(stored));
  }
  return out;
}
Json Deserialize(const Json& stored) {
  Require(stored.is_array() && stored.size() <= 256,
          "expected up to 256 catalogs");
  Json out = Json::array();
  for (auto e : stored) {
    const std::set<std::string> keys = {
        "id",    "revision", "source", "license",      "schema",
        "units", "storage",  "hash",   "record_count", "records"};
    Require(e.is_object(), "catalog entry must be an object");
    for (const auto& [key, value] : e.items())
      Require(keys.contains(key), "unknown stored field: " + key);

    for (const auto* f : {"id", "revision", "source", "license", "hash"})
      Text(e, f);
    Require(e.at("schema") == "bearing-v1" && e.at("units") == "mm",
            "unsupported schema or units");
    Require(e.at("storage") == "reference" || e.at("storage") == "embedded",
            "invalid storage");
    Require(e.at("hash").get<std::string>().size() == 64,
            "invalid fingerprint");
    Require(e.at("record_count").is_number_unsigned() ||
                e.at("record_count").is_number_integer(),
            "invalid record count");
    Require(e.at("record_count").get<int>() > 0 &&
                e.at("record_count").get<int>() <= 10000,
            "invalid record count");
    if (e.contains("records")) {
      const auto& rows = e.at("records");
      Require(rows.is_array() &&
                  rows.size() == e.at("record_count").get<size_t>(),
              "record count mismatch");
      Json raw = rows;
      for (auto& row : raw)
        row.erase("unsupported_fields");
      auto normalized = Normalize(raw, 1.0, Json::object());
      for (size_t i = 0; i < normalized.size(); ++i) {
        Require(rows[i].value("unsupported_fields", Json::array()).is_array(),
                "invalid unsupported fields");
        normalized[i]["unsupported_fields"] =
            rows[i].value("unsupported_fields", Json::array());
      }
      Require(normalized == rows &&
                  Sha256(rows.dump()) == e.at("hash").get<std::string>(),
              "record fingerprint or schema mismatch");
    } else
      Require(e.at("storage") == "reference",
              "embedded catalog is missing records");
    for (const auto& old : out)
      Require(old["id"] != e["id"] || old["revision"] != e["revision"],
              "duplicate catalog revision");
    out.push_back(std::move(e));
  }
  return out;
}
bool Install(Json& entries, const Json& entry) {
  const Json verified = Deserialize(Json::array({entry})).at(0);
  for (auto& old : entries)
    if (old["id"] == entry["id"] && old["revision"] == entry["revision"]) {
      Require(old["hash"] == entry["hash"],
              "revision content changed; import under a new revision");
      Require(old["source"] == entry["source"] &&
                  old["license"] == entry["license"] &&
                  old["storage"] == entry["storage"],
              "revision metadata changed; import under a new revision");
      if (old.contains("records") || !entry.contains("records"))
        return false;
      old = verified;
      return true;
    }
  Require(entries.size() < 256, "catalog limit reached");
  entries.push_back(verified);
  return true;
}
const Json& Find(const Json& entries, const std::string& id,
                 const std::string& revision) {
  for (const auto& e : entries)
    if (e.at("id") == id && e.at("revision") == revision)
      return e;
  throw std::runtime_error("catalog not found: " + id + " @ " + revision);
}
const Json& Record(const Json& entry, const std::string& designation) {
  Require(
      entry.contains("records"),
      "unresolved reference: import the matching dataset to inspect records");
  for (const auto& r : entry.at("records"))
    if (r.at("designation") == designation)
      return r;
  throw std::runtime_error("catalog record not found: " + designation);
}
Json SeatRequirements(const Json& entry, const std::string& designation,
                      const Json& measurements) {
  const auto& r = Record(entry, designation);
  Require(measurements.is_object(), "measurements must be an object in mm");
  const std::set<std::string> supported = {"shaft_diameter", "housing_diameter",
                                           "shoulder_diameter",
                                           "fillet_radius"};
  for (const auto& [k, v] : measurements.items()) {
    Require(supported.contains(k), "unsupported observation: " + k);
    Number(v, k, k == "fillet_radius");
  }
  Json checks = Json::array();
  bool fail = false, missing = false;
  for (const auto* base : {"shaft", "housing", "shoulder", "fillet"}) {
    const std::string name(base),
        key = name == "fillet" ? "fillet_radius" : name + "_diameter";
    const std::string lo = name + "_min", hi = name == "fillet"
                                                   ? "fillet_radius_max"
                                                   : name + "_max";
    Json c = {{"characteristic", key}, {"units", "mm"}};
    if (!r.contains(hi)) {
      c["status"] = "unverified";
      c["reason"] = "catalog has no limits";
      missing = true;
    } else {
      c["maximum"] = r[hi];
      if (r.contains(lo))
        c["minimum"] = r[lo];
      if (!measurements.contains(key)) {
        c["status"] = "unverified";
        c["reason"] = "no measurement supplied";
        missing = true;
      } else {
        double value = Number(measurements[key], key, key == "fillet_radius");
        c["observed"] = value;
        bool pass = value <= r[hi].get<double>() &&
                    (!r.contains(lo) || value >= r[lo].get<double>());
        c["status"] = pass ? "pass" : "fail";
        fail |= !pass;
      }
    }
    checks.push_back(std::move(c));
  }
  return {{"catalog", Summary(entry)},
          {"designation", designation},
          {"checks", checks},
          {"status", fail      ? "fail"
                     : missing ? "unverified"
                               : "pass"},
          {"evidence", "caller-supplied measurements; load, speed, temperature "
                       "and fit selection not evaluated"}};
}
} // namespace catalog
