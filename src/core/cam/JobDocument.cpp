// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/cam/JobDocument.h"

#include <algorithm>

namespace cam {

uint64_t JobDocument::AddPart(uint64_t sourceDocId, const std::string& name) {
  JobPart part;
  part.id = m_nextId++;
  part.sourceDocId = sourceDocId;
  part.name = name;
  m_parts.push_back(std::move(part));
  return m_parts.back().id;
}

bool JobDocument::RemovePart(uint64_t id) {
  bool found = false;
  for (auto it = m_parts.begin(); it != m_parts.end(); ++it) {
    if (it->id == id) {
      m_parts.erase(it);
      found = true;
      break;
    }
  }
  if (!found)
    return false;
  // Everything that named it goes too. A placement for a part the job no
  // longer holds puts a ghost in the stock, and an operation target pointing
  // at one is the dangling reference the id exists to prevent -- it would
  // resolve against whatever body happened to answer to the same element
  // name, which is the wrong bracket, cut for real.
  for (Setup& setup : m_setups) {
    std::erase_if(setup.placements,
                  [id](const Placement& p) { return p.jobPartId == id; });
    for (Operation& op : setup.operations)
      std::erase_if(op.targets,
                    [id](const Target& t) { return t.jobPartId == id; });
    if (setup.topFace.jobPartId == id)
      setup.topFace = Target{};
  }
  return true;
}

JobPart* JobDocument::FindPart(uint64_t id) {
  for (JobPart& p : m_parts)
    if (p.id == id)
      return &p;
  return nullptr;
}

const JobPart* JobDocument::FindPart(uint64_t id) const {
  for (const JobPart& p : m_parts)
    if (p.id == id)
      return &p;
  return nullptr;
}

Setup& JobDocument::AddSetup(const std::string& name) {
  Setup setup;
  setup.id = m_nextId++;
  setup.name =
      name.empty() ? ("Setup " + std::to_string(m_setups.size() + 1)) : name;
  // One tool by default, because a setup with no tools can produce nothing
  // and the first thing anyone does is add a 6 mm end mill.
  Tool tool;
  tool.number = 1;
  tool.name = "6 mm flat";
  setup.tools.push_back(tool);
  m_setups.push_back(std::move(setup));
  return m_setups.back();
}

bool JobDocument::RemoveSetup(uint64_t id) {
  for (auto it = m_setups.begin(); it != m_setups.end(); ++it) {
    if (it->id == id) {
      m_setups.erase(it);
      return true;
    }
  }
  return false;
}

Setup* JobDocument::FindSetup(uint64_t id) {
  for (Setup& s : m_setups)
    if (s.id == id)
      return &s;
  return nullptr;
}

const Setup* JobDocument::FindSetup(uint64_t id) const {
  for (const Setup& s : m_setups)
    if (s.id == id)
      return &s;
  return nullptr;
}

} // namespace cam
