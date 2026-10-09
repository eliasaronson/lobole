// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/cam/CamMachine.h"

#include <cctype>

namespace cam {

const Machine* FindMachine(const std::vector<Machine>& globals,
                           const std::string& name) {
  if (name.empty())
    return nullptr;
  for (const Machine& m : globals)
    if (m.name == name)
      return &m;
  return nullptr;
}

MachineMatch MatchMachine(const std::vector<Machine>& globals,
                          const Machine& embedded) {
  if (embedded.name.empty())
    return MachineMatch::Unnamed;
  const Machine* global = FindMachine(globals, embedded.name);
  if (global == nullptr)
    return MachineMatch::Missing;
  return *global == embedded ? MachineMatch::Global : MachineMatch::Differs;
}

std::string MachineFileName(const std::string& name) {
  std::string slug;
  bool dash = false;
  for (const char c : name) {
    if (std::isalnum(static_cast<unsigned char>(c))) {
      slug += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      dash = false;
    } else if (!slug.empty() && !dash) {
      slug += '-';
      dash = true;
    }
  }
  while (!slug.empty() && slug.back() == '-')
    slug.pop_back();
  return (slug.empty() ? std::string("machine") : slug) + ".json";
}

} // namespace cam
