#include "core/Document.h"

#include "Logger.h"

Feature* Document::GetFeature(size_t index) {
  return index < m_features.size() ? m_features[index].get() : nullptr;
}

void Document::AddFeature(std::unique_ptr<Feature> feature) {
  InsertFeature(std::move(feature), m_features.size());
}

void Document::InsertFeature(std::unique_ptr<Feature> feature, size_t index) {
  if (index > m_features.size()) {
    index = m_features.size();
  }
  // Assign a stable id, or respect one carried in (file load, RPC, undo
  // re-insertion) while keeping the counter ahead of it.
  if (feature->GetId() == 0) {
    feature->SetId(m_nextId++);
  } else {
    m_nextId = std::max(m_nextId, feature->GetId() + 1);
  }
  m_features.insert(m_features.begin() + index, std::move(feature));
  Regenerate();
}

Feature* Document::FindFeatureById(uint64_t id, size_t* outIndex) {
  if (id == 0)
    return nullptr;
  for (size_t i = 0; i < m_features.size(); i++) {
    if (m_features[i]->GetId() == id) {
      if (outIndex)
        *outIndex = i;
      return m_features[i].get();
    }
  }
  return nullptr;
}

std::unique_ptr<Feature> Document::RemoveFeature(size_t index) {
  if (index >= m_features.size()) {
    return nullptr;
  }
  std::unique_ptr<Feature> removed = std::move(m_features[index]);
  m_features.erase(m_features.begin() + index);
  Regenerate();
  return removed;
}

bool Document::ReplaceFeature(size_t index, std::unique_ptr<Feature> feature) {
  if (index >= m_features.size() || !feature) {
    return false;
  }
  m_features[index] = std::move(feature);
  Regenerate();
  return true;
}

bool Document::Regenerate() {
  m_bodies.clear();
  m_version++;

  for (size_t i = 0; i < m_features.size(); i++) {
    RegenContext ctx{m_bodies, m_features, i};
    if (!m_features[i]->Execute(ctx)) {
      LOG_ERROR("Regeneration stopped at feature '{}'",
                m_features[i]->GetName());
      return false;
    }
  }
  return true;
}
