// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/PrintAllowance.h"
#include "core/io/Serialization.h" // WriteTextAtomic

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <stdexcept>

// Print profiles (docs/tolerances/README.md#print-profiles): the user's own
// copies of the allowance tables, one file each, and the same rows a printed
// part embeds in its `print` envelope. The store copies the machine store's
// contract (SerializeCam.cpp, LoadMachines/SaveMachine) on purpose: a broken
// file is reported and the rest still load; a refusal writes nothing.
namespace DocumentIO {

using nlohmann::json;

json PrintRowToJson(const printfit::Row& r) {
  json j{{"kind", printfit::PrintedName(r.kind)},
         {"from_mm", r.fromMm},
         {"allowance_mm", r.allowanceMm}};
  // "And up" is absent rather than a sentinel number a reader would have to
  // know about.
  if (r.toMm < printfit::kAboveMm)
    j["to_mm"] = r.toMm;
  if (!r.source.empty())
    j["source"] = r.source;
  return j;
}

printfit::Row PrintRowFromJson(const json& j) {
  if (!j.is_object())
    throw std::runtime_error("a print row is an object {kind, from_mm, "
                             "to_mm?, allowance_mm, source?}");
  printfit::Row r;
  const std::string kind = j.value("kind", std::string());
  const auto k = printfit::PrintedFromName(kind);
  if (!k)
    throw std::runtime_error("unknown print row kind '" + kind +
                             "'. Accepted: " + printfit::PrintedNames());
  r.kind = *k;
  if (!j.contains("allowance_mm") || !j.at("allowance_mm").is_number())
    throw std::runtime_error("a " + kind + " row needs allowance_mm");
  r.allowanceMm = j.at("allowance_mm").get<double>();
  r.fromMm = j.value("from_mm", 0.0);
  r.toMm = j.contains("to_mm") && !j.at("to_mm").is_null()
               ? j.at("to_mm").get<double>()
               : printfit::kAboveMm;
  r.source = j.value("source", std::string());
  return r;
}

json PrintProfileToJson(const printfit::Profile& p) {
  json j{{"name", p.name},
         {"process", p.table.process},
         {"material", p.table.material}};
  if (!p.table.conditions.empty())
    j["conditions"] = p.table.conditions;
  if (p.stlDeflectionMm > 0.0)
    j["stl_deflection_mm"] = p.stlDeflectionMm;
  json rows = json::array();
  for (const printfit::Row& r : p.table.rows)
    rows.push_back(PrintRowToJson(r));
  j["rows"] = std::move(rows);
  return j;
}

printfit::Profile PrintProfileFromJson(const json& j) {
  if (!j.is_object())
    throw std::runtime_error("a print profile is an object {name, process, "
                             "material, conditions?, stl_deflection_mm?, "
                             "rows}");
  printfit::Profile p;
  p.name = j.value("name", std::string());
  p.table.process = j.value("process", std::string());
  p.table.material = j.value("material", std::string());
  p.table.conditions = j.value("conditions", std::string());
  p.stlDeflectionMm = j.value("stl_deflection_mm", 0.0);
  if (j.contains("rows")) {
    if (!j.at("rows").is_array())
      throw std::runtime_error("print profile rows must be an array");
    for (const json& r : j.at("rows"))
      p.table.rows.push_back(PrintRowFromJson(r));
  }
  if (const std::string why = printfit::ValidateProfile(p); !why.empty())
    throw std::runtime_error(why);
  return p;
}

std::vector<printfit::Profile> LoadPrintProfiles(const std::string& dir,
                                                 std::string* error) {
  std::vector<printfit::Profile> out;
  if (error != nullptr)
    error->clear();
  std::error_code ec;
  if (!std::filesystem::is_directory(dir, ec))
    return out; // no store yet is not an error: nothing has been saved
  std::string problems;
  for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
    if (!entry.is_regular_file() || entry.path().extension() != ".json")
      continue;
    try {
      std::ifstream in(entry.path());
      out.push_back(PrintProfileFromJson(json::parse(in)));
    } catch (const std::exception& e) {
      // One broken file must not hide the others: say which, keep going.
      problems += (problems.empty() ? "" : "; ") +
                  entry.path().filename().string() + ": " + e.what();
    }
  }
  std::sort(out.begin(), out.end(),
            [](const printfit::Profile& a, const printfit::Profile& b) {
              return a.name < b.name;
            });
  if (error != nullptr)
    *error = problems;
  return out;
}

bool SavePrintProfile(const std::string& dir, const printfit::Profile& p,
                      std::string* error) {
  if (const std::string why = printfit::ValidateProfile(p); !why.empty()) {
    if (error != nullptr)
      *error = why;
    return false;
  }
  const std::filesystem::path file =
      std::filesystem::path(dir) / printfit::ProfileFileName(p.name);
  // Two names that fold to one file name ("K5 PLA" and "k5-pla") would
  // silently overwrite each other; refuse the second.
  std::error_code ec;
  if (std::filesystem::exists(file, ec)) {
    try {
      std::ifstream in(file);
      const json j = json::parse(in);
      const std::string held = j.value("name", std::string());
      if (!held.empty() && held != p.name) {
        if (error != nullptr)
          *error = "'" + p.name + "' would share a file with '" + held +
                   "' — pick a name that differs in more than case or "
                   "punctuation";
        return false;
      }
    } catch (const std::exception&) {
      // Unreadable: it is about to be replaced by something readable.
    }
  }
  std::filesystem::create_directories(dir, ec);
  return WriteTextAtomic(file, PrintProfileToJson(p).dump(2) + "\n", error);
}

bool DeletePrintProfile(const std::string& dir, const std::string& name,
                        std::string* error) {
  const std::filesystem::path file =
      std::filesystem::path(dir) / printfit::ProfileFileName(name);
  std::error_code ec;
  bool ours = false;
  if (std::filesystem::exists(file, ec)) {
    try {
      std::ifstream in(file);
      ours = json::parse(in).value("name", std::string()) == name;
    } catch (const std::exception&) {
      ours = false;
    }
  }
  if (!ours) {
    if (error != nullptr)
      *error = "no print profile called '" + name + "'";
    return false;
  }
  if (!std::filesystem::remove(file, ec)) {
    if (error != nullptr)
      *error = "could not delete " + file.string() + ": " + ec.message();
    return false;
  }
  return true;
}

} // namespace DocumentIO
