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
  m_features.insert(m_features.begin() + index, std::move(feature));
  Regenerate();
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

  for (const auto& feature : m_features) {
    if (!feature->Execute(m_bodies)) {
      LOG_ERROR("Regeneration stopped at feature '{}'", feature->GetName());
      return false;
    }
  }
  return true;
}
