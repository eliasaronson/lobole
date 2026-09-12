#pragma once

#include "render/SketchRenderer.h" // PrimVertex
#include "ui/AppState.h"

namespace ui {

// U1b (docs/ui/u1-selection.md): the one model-space pick pipeline for the
// default (nothing-armed) part-tab context. Builds the pick ray once, ranks
// edge/face/body candidates under the cursor, drives the hover preview, and
// applies the signed-off click conventions: plain click replaces the
// selection (empty space clears), Ctrl+click toggles, clicking the same spot
// again cycles the overlapping candidates, Shift+click also aligns the
// camera to the picked face (the old Ctrl gesture — Ctrl now multi-selects).
// While a tool is armed (blend edge collection, measure, sim BC/keep — U1c),
// its filter restricts the candidates and the resolved pick is routed to the
// tool instead of the Selection. `allowEscapeClear` is false when a tool
// already consumed this frame's Escape (disarming), so the selection isn't
// also cleared by the same key press.
void HandleSelectionInput(AppState& state, bool overViewport,
                          bool allowEscapeClear = true);

// The armed context's status-bar prompt, or null when nothing is armed.
const char* ArmedPrompt(AppState& state);

// Renderer highlights as a per-frame projection of state.selection — the one
// sync point that replaces scattered SetSelectedFace/SetFaceHighlight pairs.
// (The face highlight slot is single: the most recent Face item shows; the
// full multi-face visual arrives with the renderer work in U5.)
void SyncSelectionHighlights(AppState& state);

// Selected + hovered B-Rep edges as thick AA primitives on the sketch
// overlay channel (same look as the blend edge-pick display, which takes
// precedence while it is active).
void AppendSelectionPrimitives(AppState& state, std::vector<PrimVertex>& out);

// U2: while a Profiles slot is armed, the sketch's selected regions and the
// hovered one drawn as emphasized loop boundaries.
void AppendRegionPickPrimitives(AppState& state, std::vector<PrimVertex>& out);

// The "id,id,..." region key (SelectionItem element name) back to ids.
std::vector<uint32_t> ParseRegionIds(const std::string& joined);

// Resolve a stable item back to this frame's render handles. False when the
// referent doesn't resolve on the current geometry (stale name, no meshes).
bool ResolveFaceItem(const AppState& state, const SelectionItem& item,
                     uint32_t& meshId, uint32_t& faceIndex);
bool ResolveEdgeItem(const AppState& state, const SelectionItem& item,
                     uint32_t& meshId, uint32_t& edgeIndex);

// What is selected -- the questions the app asks about the selection set.
//
// Split out of ui/AppActions.h, which declared 170 functions across every
// domain in the app and was included by 64 files -- so a change to any one
// of them rebuilt 73 targets (146 s) instead of the handful that care.
// docs/architecture-review-2026-09-06.md, stage 4.

// Feature selection through the one Selection set (U1a). Stored by stable
// feature id, so it survives regeneration, reordering and sibling deletes;
// callers that need the index resolve it fresh each time.
uint64_t SelectedFeatureId(const AppState& state); // 0 = none
Feature* SelectedFeature(AppState& state, size_t* indexOut = nullptr);

int SelectedFeatureIndexOrMinus1(AppState& state);       // RPC back-compat
void SelectFeature(AppState& state, uint64_t featureId); // replaces the set
void SelectFeatureByIndex(AppState& state, size_t index);

// Opens the committed-feature editor session in the Tool window (U7e —
// tree double-click / context-menu Edit). No-op for unknown ids.
void OpenFeatureEditor(AppState& state, uint64_t featureId);

// Assembly-tab selection through the same set (U1d). Occurrence and mate
// items coexist (the panel lists select independently, as the old separate
// fields did): each setter replaces only its own kind; 0 removes it.
uint64_t SelectedOccurrenceId(const AppState& state);

// Every selected occurrence, in selection order. The single-id form above is
// the head of this and stays the right question for the editor rows, which
// show ONE occurrence's transform.
[[nodiscard]] std::vector<uint64_t>
SelectedOccurrenceIds(const AppState& state);

uint64_t SelectedMateId(const AppState& state);

// The selected relation, 0 for none, and its setter. Kind-scoped like the
// mate's: an assembly's occurrence, mate and relation selections are
// independent of each other.
uint64_t SelectedRelationId(const AppState& state);
void SelectRelation(AppState& state, uint64_t relationId);

void SelectMate(AppState& state, uint64_t mateId);

// Drops items whose referent no longer exists (deleted feature, undone
// creation, File > New). Called once per frame before the panels draw.
void PruneSelection(AppState& state);

// The calibrate-image picks and the span between them, on the sketch plane.
void AppendCalibrationMarkers(AppState& state, std::vector<PrimVertex>& out);

} // namespace ui
