#pragma once

#include "core/Feature.h"
#include "ui/AppState.h"

#include <memory>

namespace ui {

// Onshape-style preview → accept/cancel for feature creation.
//
// A pending feature is added to the Document immediately (so it regenerates,
// renders, and is pickable) but is NOT pushed onto the undo stack until the
// user accepts. Accepting removes it (its id is preserved) and re-adds it via
// one AddFeatureCommand, so it becomes a single undoable action. Cancelling
// removes it and leaves the undo stack untouched. The undo stack is frozen
// while a preview is pending (Ctrl+Z cancels the preview instead).

uint64_t BeginPreviewFeature(AppState& state, std::unique_ptr<Feature> feature);

bool HasPendingFeature(const AppState& state);

// Resolves the pending feature by id (undo-safe); nullptr if none.
Feature* ResolvePendingFeature(AppState& state, size_t* outIndex = nullptr);

void AcceptPendingFeature(AppState& state);
void CancelPendingFeature(AppState& state);

// Draws the floating tool-session window (U7): a pending feature's slot
// rows + params + accept/cancel, or the mate session's connector slots on
// assembly tabs. No-op when no session is open.
void DrawToolWindow(AppState& state);

// Opens the shared session-window frame (top-right anchor, stable
// "###ToolWindow" id). The caller draws the contents and calls ImGui::End().
void BeginToolWindowFrame(AppState& state, const std::string& title);

} // namespace ui
