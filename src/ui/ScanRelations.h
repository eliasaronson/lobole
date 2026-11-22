// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// The sketch's "Scan relations" session
// (docs/sketching/README.md#relations-the-scan-supports): the relations and
// round sizes the geometry traced over a section comes within its own
// uncertainty of, listed with their sigmas, each row lighting its entities;
// the ticked rows applied as sketch_candidates' accept_candidates does, one
// undo step, and a row the solve took back saying why.

#include "ui/AppState.h"

namespace ui {

// Whether the active sketch has geometry traced over a section.
bool SketchHasTraced(AppState& state);

// The session's tool window. Called by DrawSketchToolWindow while
// `sketchEdit.relationsOpen` and no drawing tool owns the window.
void DrawScanRelationsSession(AppState& state, SketchFeature& sketch);

// Applies the ticked candidates within the session's sigma limit to the
// active sketch, one undo step; the outcome lands in the session. False
// when nothing was there to apply.
bool ApplyScanRelations(AppState& state);

} // namespace ui
