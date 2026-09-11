#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// A named variant of a part (or assembly): a set of parameter-value overrides
// plus additionally-suppressed features. A configuration is applied by
// REGENERATING against its overrides — the document overlays them onto the
// symbol table and composes its suppression set, never mutating the authored
// model. A doc-level object, the FEM Study sibling — not a Feature, does not
// participate in the feature list. See docs/configurations/plan.md.

// A mate degree of freedom an ASSEMBLY configuration holds at a value of its
// own (v101). The solve uses it in place of the mate's value, which stays as
// authored — so leaving the configuration restores it.
struct MateOverride {
  uint64_t mate = 0;
  std::optional<float> rotation; // radians
  std::optional<float> offset;   // mm: a slide, or a revolute's axial seat
};

struct Configuration {
  uint64_t id = 0; // doc-unique, from Document::AllocateId(); never reused
  std::string name;

  // Parameter name -> override expression (may reference other parameters, so
  // `depth = width * 3` works). Reuses the {name, expression} parameter shape.
  std::vector<std::pair<std::string, std::string>> parameterOverrides;

  // Feature ids this configuration suppresses, composed on top of each
  // feature's own authored `suppressed` flag (effective = authored OR here).
  // On an ASSEMBLY these are occurrence ids.
  std::vector<uint64_t> suppressedFeatures;

  // Assembly only (v101): mates and relations taken out of the solve, and
  // mate DOFs held at a value of the configuration's own. An OVERLAY applied
  // inside the solve, never written into the mates — "crown pulled out" is a
  // configuration, and pushing it back in restores every authored value.
  std::vector<uint64_t> suppressedMates;
  std::vector<uint64_t> suppressedRelations;
  std::vector<MateOverride> mateOverrides;

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

  [[nodiscard]] bool SuppressesMate(uint64_t mateId) const {
    for (uint64_t id : suppressedMates)
      if (id == mateId)
        return true;
    return false;
  }

  [[nodiscard]] bool SuppressesRelation(uint64_t relationId) const {
    for (uint64_t id : suppressedRelations)
      if (id == relationId)
        return true;
    return false;
  }

  [[nodiscard]] const MateOverride* OverrideForMate(uint64_t mateId) const {
    for (const MateOverride& o : mateOverrides)
      if (o.mate == mateId)
        return &o;
    return nullptr;
  }
};
