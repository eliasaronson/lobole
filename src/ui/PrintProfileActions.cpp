// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "ui/PrintProfileActions.h"

#include "common/Logger.h"
#include "core/io/Serialization.h"
#include "core/io/Settings.h"

#include <filesystem>

namespace ui {

std::string PrintProfileDir() {
  return (std::filesystem::path(settings::ConfigDir()) / "print_profiles")
      .string();
}

void ReloadPrintProfiles(AppState& state) {
  std::string problems;
  state.printProfiles =
      DocumentIO::LoadPrintProfiles(PrintProfileDir(), &problems);
  if (!problems.empty())
    LOG_WARN("print profiles: {}", problems);
}

bool SavePrintProfile(AppState& state, const printfit::Profile& profile,
                      std::string* error) {
  if (!DocumentIO::SavePrintProfile(PrintProfileDir(), profile, error))
    return false;
  ReloadPrintProfiles(state);
  return true;
}

bool DeletePrintProfile(AppState& state, const std::string& name,
                        std::string* error) {
  if (!DocumentIO::DeletePrintProfile(PrintProfileDir(), name, error))
    return false;
  ReloadPrintProfiles(state);
  return true;
}

bool RenamePrintProfile(AppState& state, const std::string& from,
                        const std::string& to, std::string* error) {
  const printfit::Profile* have =
      printfit::FindProfile(state.printProfiles, from);
  if (have == nullptr) {
    if (error != nullptr)
      *error = "no print profile called '" + from + "'";
    return false;
  }
  if (from == to)
    return true;
  if (printfit::FindProfile(state.printProfiles, to) != nullptr) {
    if (error != nullptr)
      *error = "there is already a print profile called '" + to + "'";
    return false;
  }
  printfit::Profile renamed = *have;
  renamed.name = to;
  // Two names that fold to one file ("K5 PLA" -> "k5 pla") are the same
  // file: write the new name over it and there is nothing to delete.
  const bool sameFile =
      printfit::ProfileFileName(from) == printfit::ProfileFileName(to);
  if (!sameFile) {
    if (!DocumentIO::SavePrintProfile(PrintProfileDir(), renamed, error))
      return false;
    (void)DocumentIO::DeletePrintProfile(PrintProfileDir(), from, nullptr);
  } else {
    if (!DocumentIO::DeletePrintProfile(PrintProfileDir(), from, error) ||
        !DocumentIO::SavePrintProfile(PrintProfileDir(), renamed, error)) {
      ReloadPrintProfiles(state);
      return false;
    }
  }
  ReloadPrintProfiles(state);
  return true;
}

std::string FreePrintProfileName(const AppState& state,
                                 const std::string& base) {
  const auto taken = [&](const std::string& name) {
    for (const printfit::Profile& p : state.printProfiles)
      if (p.name == name ||
          printfit::ProfileFileName(p.name) == printfit::ProfileFileName(name))
        return true;
    return false;
  };
  if (!taken(base))
    return base;
  for (int n = 2;; n++) {
    const std::string name = base + " " + std::to_string(n);
    if (!taken(name))
      return name;
  }
}

} // namespace ui
