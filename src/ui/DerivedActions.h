#pragma once

// The Derived feature's project-level verbs (docs/modeling/derived-plan.md,
// D2). The feature itself (core/features/DerivedFeature.h) knows only two
// Documents; everything that needs the PROJECT — which part a slot id is,
// whether a source is loaded, the context assembly's placements — lives
// here, and both the GUI (tool window, tree verbs) and the RPCs go through
// it, so the two surfaces cannot disagree about a refusal.

#include "core/features/DerivedFeature.h"
#include "ui/AppState.h"

#include <glm/glm.hpp>

#include <optional>
#include <string>
#include <vector>

namespace ui {

// One thing the active part may derive from: a part of the project (`slotId`)
// or, during an in-context edit, a neighbour occurrence of the context
// assembly (`occurrenceId` set, `placement` = where it sits relative to the
// edited part). The active part itself, and any part whose derived chain
// reaches it, are left out — deriving from them is the cycle the plan
// refuses.
struct DerivedSource {
  uint64_t slotId = 0;
  std::string name;
  uint64_t occurrenceId = 0; // 0 = a plain part of the project
  glm::mat4 placement{1.0f}; // identity for a plain part
};
std::vector<DerivedSource> DerivedSourceCandidates(AppState& state);

// Points `feature` at `source` (slot id or, through `occurrenceId`, a
// neighbour) and takes the snapshot into the active part. Refuses, in words,
// a source that is not loaded, the part itself, a cycle, and everything
// derived::TakeSnapshot refuses. Empty on success.
[[nodiscard]] std::string PointDerivedAt(AppState& state,
                                         DerivedFeature& feature,
                                         const DerivedSource& source);

// Retakes `feature`'s snapshot from its source (the same slot id). Refuses
// when the source is not loaded (naming it) or the link is broken. Only
// fills the feature; SnapshotDerivedNow is the undoable verb.
[[nodiscard]] std::string SnapshotDerived(AppState& state,
                                          DerivedFeature& feature);

// Whether the snapshot is behind its source. nullopt when the source is not
// loaded (unknown — reported as such, never as fresh).
[[nodiscard]] std::optional<bool> DerivedStale(AppState& state,
                                               const DerivedFeature& feature);

// The Derive tool: a preview Derived feature. With exactly one candidate the
// preview already carries its snapshot; with several (or none) the window
// asks, and Accept stays disabled until a source is chosen (U10).
void CreateDerived(AppState& state);

// Update from the source — one ModifyFeatureCommand, so one undo step that
// restores the previous snapshot. Returns the refusal or empty.
[[nodiscard]] std::string UpdateDerived(AppState& state, uint64_t featureId);
// Every Derived feature of the active part whose source is loaded; the
// count updated, and the first refusal (a source that is gone) in `why`.
size_t UpdateAllDerived(AppState& state, std::string* why = nullptr);

// Breaks the link: the snapshot stays, the source is forgotten (slot 0), so
// the copy is a frozen body like an Import's and never reads as stale.
[[nodiscard]] std::string BreakDerivedLink(AppState& state, uint64_t featureId);

// The stale Derived features of `doc` (source loaded and moved), for the
// tree badge, get_state and check_geometry.
std::vector<const DerivedFeature*> StaleDerived(AppState& state,
                                                const Document& doc);

} // namespace ui
