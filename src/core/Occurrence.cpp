#include "core/Occurrence.h"

#include <algorithm>

uint64_t AssemblyDocument::AddOccurrence(Occurrence occ) {
  occ.id = m_nextId++;
  m_occurrences.push_back(std::move(occ));
  m_version++;
  return m_occurrences.back().id;
}

bool AssemblyDocument::RemoveOccurrence(uint64_t id) {
  const auto it = std::find_if(
      m_occurrences.begin(), m_occurrences.end(),
      [id](const Occurrence& o) { return o.id == id; });
  if (it == m_occurrences.end()) return false;
  m_occurrences.erase(it);
  m_version++;
  return true;
}

Occurrence* AssemblyDocument::FindById(uint64_t id) {
  for (Occurrence& o : m_occurrences)
    if (o.id == id) return &o;
  return nullptr;
}

const Occurrence* AssemblyDocument::FindById(uint64_t id) const {
  for (const Occurrence& o : m_occurrences)
    if (o.id == id) return &o;
  return nullptr;
}
