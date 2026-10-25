// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// The Derived feature's project-level verbs (docs/modeling/derived.md#verbs).
// The feature itself (core/features/DerivedFeature.h) knows only two Documents;
// everything that needs the PROJECT — which part a slot id is, whether a source
// is loaded, the context assembly's placements — lives here, and both the GUI
// (tool window, tree verbs) and the RPCs go through it, so the two surfaces
// cannot disagree about a refusal.

#include "core/features/DerivedFeature.h"
#include "ui/AppState.h"

#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

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
  glm::dmat4 placement{1.0}; // identity for a plain part
};
std::vector<DerivedSource> DerivedSourceCandidates(AppState& state);

// Points `feature` at `source` (slot id or, through `occurrenceId`, a
// neighbour) and takes the snapshot into the active part. Refuses, in words,
// a source that is not loaded, the part itself, a cycle (over body and
// sketch links alike), and everything derived::TakeSnapshot refuses. Empty
// on success.
[[nodiscard]] std::string PointDerivedAt(AppState& state,
                                         DerivedFeature& feature,
                                         const DerivedSource& source);

// Retakes `feature`'s snapshot from its source (the same slot id) into
// `host` (the part it lives in). Refuses when the source is not loaded
// (naming it), the link is broken, or the source now derives from `host` (a
// cycle, naming the chain). Only fills the feature; UpdateDerived is the
// undoable verb.
[[nodiscard]] std::string
SnapshotDerived(AppState& state, DerivedFeature& feature, Document& host);

// Whether the snapshot is behind its source: taking it again would change
// what it holds, or its source is itself behind (stale is transitive: C
// copying B copying A reads stale while B is). nullopt when the source is
// not loaded (unknown — reported as such, never as fresh). `host` is the
// part the feature lives in (default: the active one). `why`, when given and
// stale, says why.
[[nodiscard]] std::optional<bool> DerivedStale(AppState& state,
                                               const DerivedFeature& feature,
                                               const Document* host = nullptr,
                                               std::string* why = nullptr);

// The sketches a part offers to copy (its Sketch features, in history
// order), for the Derive tool's list.
struct SourceSketch {
  uint64_t id = 0;
  std::string name;
};
std::vector<SourceSketch> SourceSketches(AppState& state, uint64_t slotId);

// "Sketch on it": a new sketch of the active part on the plane of the
// copied sketch `sourceSketchId` of Derived `derivedId`, with every entity of
// it projected (linked, solver-fixed), opened for editing. Refuses a feature
// that is not a Derived, or a sketch it does not copy (naming the ones it
// does).
[[nodiscard]] std::string SketchOnDerived(AppState& state, uint64_t derivedId,
                                          uint64_t sourceSketchId);

// A Derived of the active part copying sketch `sourceSketch` of part
// `sourcePart`, placed before history index `beforeIndex`: an existing one
// that already copies it there, else a new one inserted there (one undo
// step; `beforeIndex` then moves on by one). What a linked paste from another
// part links through. `derivedId` receives its id; the refusal or empty.
[[nodiscard]] std::string
DerivedSketchBefore(AppState& state, uint64_t sourcePart, uint64_t sourceSketch,
                    size_t& beforeIndex, uint64_t& derivedId);

// The Derive tool: a preview Derived feature. With exactly one candidate the
// preview already carries its snapshot; with several (or none) the window
// asks, and Accept stays disabled until a source is chosen (U10).
void CreateDerived(AppState& state);

// Update from the source — one ModifyFeatureCommand, so one undo step that
// restores the previous snapshot. Returns the refusal or empty.
[[nodiscard]] std::string UpdateDerived(AppState& state, uint64_t featureId);
// Every linked Derived feature of the active part, as ONE undo step. The ids
// it updated go in `updated` (a copy already up to date counts: it was
// taken), and each one it could not take in `skipped` as "<name>: <why>".
void UpdateAllDerived(AppState& state, std::vector<uint64_t>& updated,
                      std::vector<std::string>& skipped);

// Turns `follow` on or off (one undo step). A copy that does not follow
// stays as taken until Update, and reads stale when behind.
[[nodiscard]] std::string SetDerivedFollow(AppState& state, uint64_t featureId,
                                           bool follow);

// What one gate did to one part (docs/modeling/derived.md#copies-that-keep-up).
struct LinkUpdate {
  uint64_t partId = 0;
  std::string partName;
  std::vector<uint64_t> features; // the Derived features retaken
  // A feature the update broke: it had no error before and has one now.
  struct Broken {
    uint64_t featureId = 0;
    std::string name;
    std::string error;
  };
  std::vector<Broken> broken;
};

// The gate (docs/modeling/derived.md#copies-that-keep-up): brings every
// stale FOLLOWING link of the project up to date, part by part in link
// order (C sees B after B has taken A), each part's updates as ONE command
// on that part's own undo stack. A source that is absent or rolled back is
// not taken (the link stays stale and says why), and neither is a link in a
// cycle. Cheap when nothing moved: a source's key is cached by its stamp.
// Called when a tab is shown or a project opened, before an RPC that reads
// geometry, and before a save, an export, a requirements check, a sweep and
// a CAM generate. When `notice` is set and anything was updated, the menu
// bar says so (AppState::linksNotice).
std::vector<LinkUpdate> RefreshLinks(AppState& state, bool notice = true);
// The RPC's `links_updated`: [{part, part_id, features, broken}].
nlohmann::json LinksUpdatedJson(const std::vector<LinkUpdate>& updates);
// "Bracket updated from Plate: Hole 2 failed" -- one line for them all.
std::string LinksNoticeText(AppState& state,
                            const std::vector<LinkUpdate>& updates);

// The audit behind the gates
// (docs/modeling/derived.md#nothing-may-miss-a-gate): while on, every resolver
// that hands a part to another document (PartDocById, and the drawing and job
// resolvers folded onto it) checks that the part has no stale following link a
// gate would have taken, and counts one that does -- a path that reads a part
// without passing a gate. The GUI suite turns it on and fails a test with a
// miss; LOBOLE_LINK_AUDIT=1 in the environment (every smoke sets it) aborts on
// one, naming it.
void SetLinkAudit(bool on);
[[nodiscard]] bool LinkAuditOn();
[[nodiscard]] int LinkAuditMisses();
[[nodiscard]] std::string LinkAuditLastMiss();
void ResetLinkAudit();
// The check itself, for `part` about to be handed out. Does nothing while
// the audit is off.
void AuditPartRead(AppState& state, const Document& part);

// Breaks the link: the snapshot stays, the source is forgotten (slot 0), so
// the copy is a frozen body like an Import's and never reads as stale.
[[nodiscard]] std::string BreakDerivedLink(AppState& state, uint64_t featureId);

// The stale Derived features of `doc` (source loaded and moved), for the
// tree badge, get_state and check_geometry.
std::vector<const DerivedFeature*> StaleDerived(AppState& state,
                                                const Document& doc);

} // namespace ui
