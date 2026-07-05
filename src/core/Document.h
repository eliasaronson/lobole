#pragma once

#include "core/Feature.h"

#include <cstdint>
#include <memory>
#include <vector>

// The parametric model: an ordered feature list plus the bodies produced by
// regenerating it. This is the single source of truth for model state — the
// UI, scripts, and (later) files all act on the Document through Commands,
// never on geometry directly.
class Document {
public:
  size_t FeatureCount() const { return m_features.size(); }
  Feature* GetFeature(size_t index);
  const std::vector<std::unique_ptr<Feature>>& GetFeatures() const {
    return m_features;
  }

  // Mutators regenerate automatically so bodies always match the features.
  void AddFeature(std::unique_ptr<Feature> feature);
  void InsertFeature(std::unique_ptr<Feature> feature, size_t index);
  std::unique_ptr<Feature> RemoveFeature(size_t index);

  // Rebuilds all bodies by executing the feature list in order. Returns
  // false if any feature failed; bodies then reflect the successful prefix.
  bool Regenerate();

  const std::vector<Body>& GetBodies() const { return m_bodies; }

  // Incremented on every regeneration; lets the UI detect when to re-upload
  // render meshes without observing individual edits.
  uint64_t Version() const { return m_version; }

private:
  std::vector<std::unique_ptr<Feature>> m_features;
  std::vector<Body> m_bodies;
  uint64_t m_version = 0;
};
