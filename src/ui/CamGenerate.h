#pragma once

#include "ui/AppState.h"

// The Generate window (docs/ui/cam-panel-tidy-plan.md): the run -- generate,
// verify, what the verifier found, the cut order, one file per tool, Save
// G-code -- as a floating tool opened by the job toolbar's gears, like
// Simulate. It used to be a block of the docked CAM panel, above an operations
// list that pushed it off the bottom as the job grew.
namespace ui {

// Opens the window. `regenerate` re-cuts and re-verifies first -- what the
// toolbar's button means; the panel's status line opens it as it stands.
void OpenCamGenerate(AppState& state, bool regenerate);

// Drawn every frame; nothing when closed or with no job.
void DrawCamGenerateWindow(AppState& state);

} // namespace ui
