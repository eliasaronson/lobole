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

uint64_t AssemblyDocument::AddMate(Mate mate) {
  mate.id = m_nextId++;
  m_mates.push_back(std::move(mate));
  m_version++;
  return m_mates.back().id;
}

bool AssemblyDocument::RemoveMate(uint64_t id) {
  const auto it = std::find_if(m_mates.begin(), m_mates.end(),
                               [id](const Mate& m) { return m.id == id; });
  if (it == m_mates.end()) return false;
  m_mates.erase(it);
  m_version++;
  return true;
}

Mate* AssemblyDocument::FindMateById(uint64_t id) {
  for (Mate& m : m_mates)
    if (m.id == id) return &m;
  return nullptr;
}

const Mate* AssemblyDocument::FindMateById(uint64_t id) const {
  for (const Mate& m : m_mates)
    if (m.id == id) return &m;
  return nullptr;
}
