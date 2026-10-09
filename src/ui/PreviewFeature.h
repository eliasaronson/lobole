// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "core/Feature.h"
#include "ui/AppState.h"

#include <memory>
#include <vector>

namespace ui {

// Preview → accept/cancel for feature creation, as commercial CAD does it.
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

// A cheap "has the model changed?" summary: body count, each body's volume,
// and the feature count. Two documents with the same fingerprint are the same
// model as far as a user can tell — which is what makes it the right test for
// "would accepting this feature actually do anything?" (U10). Compared with
// ==; never serialized.
struct DocumentFingerprint {
  size_t features = 0;
  std::vector<double> volumes; // one per body, in body order
  bool operator==(const DocumentFingerprint&) const;
};
DocumentFingerprint FingerprintOf(const Document& doc);

// Opens the shared session-window frame (top-right anchor, stable
// "###ToolWindow" id). The caller draws the contents and calls ImGui::End().
void BeginToolWindowFrame(AppState& state, const std::string& title);

} // namespace ui
