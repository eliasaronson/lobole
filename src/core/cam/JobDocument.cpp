#include "core/cam/JobDocument.h"

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
  for (auto it = m_parts.begin(); it != m_parts.end(); ++it) {
    if (it->id == id) {
      m_parts.erase(it);
      return true;
    }
  }
  return false;
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
