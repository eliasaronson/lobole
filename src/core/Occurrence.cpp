#include "core/Occurrence.h"

#include <algorithm>

uint64_t AssemblyDocument::AddOccurrence(Occurrence occ) {
  occ.id = m_nextId++;
  m_occurrences.push_back(std::move(occ));
  m_version++;
  return m_occurrences.back().id;
}

bool AssemblyDocument::RemoveOccurrence(uint64_t id) {
  const auto it =
      std::find_if(m_occurrences.begin(), m_occurrences.end(),
                   [id](const Occurrence& o) { return o.id == id; });
  if (it == m_occurrences.end())
    return false;
  m_occurrences.erase(it);
  m_version++;
  return true;
}

Occurrence* AssemblyDocument::FindById(uint64_t id) {
  for (Occurrence& o : m_occurrences)
    if (o.id == id)
      return &o;
  return nullptr;
}

const Occurrence* AssemblyDocument::FindById(uint64_t id) const {
  for (const Occurrence& o : m_occurrences)
    if (o.id == id)
      return &o;
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
  if (it == m_mates.end())
    return false;
  m_mates.erase(it);
  m_version++;
  return true;
}

Mate* AssemblyDocument::FindMateById(uint64_t id) {
  for (Mate& m : m_mates)
    if (m.id == id)
      return &m;
  return nullptr;
}

const Mate* AssemblyDocument::FindMateById(uint64_t id) const {
  for (const Mate& m : m_mates)
    if (m.id == id)
      return &m;
  return nullptr;
}

uint64_t AssemblyDocument::AddExplodedView(ExplodedView view) {
  view.id = m_nextId++;
  m_explodedViews.push_back(std::move(view));
  m_version++;
  return m_explodedViews.back().id;
}

bool AssemblyDocument::RemoveExplodedView(uint64_t id) {
  const auto it =
      std::find_if(m_explodedViews.begin(), m_explodedViews.end(),
                   [id](const ExplodedView& v) { return v.id == id; });
  if (it == m_explodedViews.end())
    return false;
  m_explodedViews.erase(it);
  m_version++;
  return true;
}

ExplodedView* AssemblyDocument::FindExplodedViewById(uint64_t id) {
  for (ExplodedView& v : m_explodedViews)
    if (v.id == id)
      return &v;
  return nullptr;
}

const ExplodedView* AssemblyDocument::FindExplodedViewById(uint64_t id) const {
  for (const ExplodedView& v : m_explodedViews)
    if (v.id == id)
      return &v;
  return nullptr;
}

// ---- Configurations (C4) --------------------------------------------------

const Configuration* AssemblyDocument::FindConfiguration(uint64_t id) const {
  for (const Configuration& c : m_configurations)
    if (c.id == id)
      return &c;
  return nullptr;
}

Configuration* AssemblyDocument::FindConfiguration(uint64_t id) {
  for (Configuration& c : m_configurations)
    if (c.id == id)
      return &c;
  return nullptr;
}

Configuration& AssemblyDocument::AddConfiguration(const std::string& name) {
  Configuration c;
  c.id = m_nextId++;
  c.name =
      name.empty()
          ? ("Configuration " + std::to_string(m_configurations.size() + 1))
          : name;
  m_configurations.push_back(std::move(c));
  m_version++;
  return m_configurations.back();
}

bool AssemblyDocument::RemoveConfiguration(uint64_t id) {
  const auto it =
      std::find_if(m_configurations.begin(), m_configurations.end(),
                   [id](const Configuration& c) { return c.id == id; });
  if (it == m_configurations.end())
    return false;
  m_configurations.erase(it);
  if (m_activeConfiguration == id)
    m_activeConfiguration = 0;
  m_version++;
  return true;
}

void AssemblyDocument::RestoreConfiguration(Configuration cfg) {
  EnsureNextIdAtLeast(cfg.id + 1);
  m_configurations.push_back(std::move(cfg));
}

bool AssemblyDocument::SetActiveConfiguration(uint64_t id) {
  if (id != 0 && FindConfiguration(id) == nullptr)
    return false;
  m_activeConfiguration = id;
  m_version++; // renderers/solve resync against the new variant
  return true;
}

const Configuration* AssemblyDocument::ActiveConfig() const {
  return m_activeConfiguration == 0 ? nullptr
                                    : FindConfiguration(m_activeConfiguration);
}

bool AssemblyDocument::OccurrenceSuppressed(uint64_t occurrenceId) const {
  const Configuration* c = ActiveConfig();
  return c != nullptr && c->Suppresses(occurrenceId);
}
