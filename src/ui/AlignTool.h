// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// The Align tool (docs/interchange/README.md#placing-an-import): the four
// modes of `align_import` -- floor, three points, principal axes, scale --
// on an Import, picked in the viewport. The import previews in its new
// placement as soon as the mode has its points; Accept is one undo step,
// and Cancel, Escape, the undo chord or a tab switch put it back.

#include "ui/AppState.h"

#include <cstdint>

namespace ui {

// Opens on `importId`, or (0) the Import whose body is selected, else the
// newest Import. Does nothing without one.
void OpenAlignTool(AppState& state, uint64_t importId = 0);
// Whether the part has an Import to align (what gates the command).
[[nodiscard]] bool HasImportToAlign(const AppState& state);
// Puts back what the preview moved and closes; a no-op when closed.
void CancelAlignTool(AppState& state);
// Commits the previewed placement as one ModifyFeatureCommand and closes.
// False (the tool left open) when there is no preview to commit.
bool AcceptAlignTool(AppState& state);
// A face pick landed while one of the tool's points was armed.
void AlignPointPicked(AppState& state, const SelectionItem& item);
// The tool window's body.
void DrawAlignToolWindow(AppState& state);

} // namespace ui
