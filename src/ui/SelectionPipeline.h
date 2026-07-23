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

} // namespace ui
