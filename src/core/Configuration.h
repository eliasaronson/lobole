#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// A named variant of a part (or assembly): a set of parameter-value overrides
// plus additionally-suppressed features. A configuration is applied by
// REGENERATING against its overrides — the document overlays them onto the
// symbol table and composes its suppression set, never mutating the authored
// model. A doc-level object, the FEM Study sibling — not a Feature, does not
// participate in the feature list. See docs/configurations/plan.md.
struct Configuration {
  uint64_t id = 0; // doc-unique, from Document::AllocateId(); never reused
  std::string name;

  // Parameter name -> override expression (may reference other parameters, so
  // `depth = width * 3` works). Reuses the {name, expression} parameter shape.
  std::vector<std::pair<std::string, std::string>> parameterOverrides;

  // Feature ids this configuration suppresses, composed on top of each
  // feature's own authored `suppressed` flag (effective = authored OR here).
  std::vector<uint64_t> suppressedFeatures;

  // The override expression for `paramName`, or nullptr if this config leaves
  // it alone.
  [[nodiscard]] const std::string*
  OverrideFor(const std::string& paramName) const {
    for (const auto& [name, expr] : parameterOverrides)
      if (name == paramName)
        return &expr;
    return nullptr;
  }

  [[nodiscard]] bool Suppresses(uint64_t featureId) const {
    for (uint64_t id : suppressedFeatures)
      if (id == featureId)
        return true;
    return false;
  }
};
