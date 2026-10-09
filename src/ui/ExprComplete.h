// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "ui/AppState.h"

#include "imgui.h"

#include <cstddef>
#include <string>

namespace ui {

// Completion over the document's variables (plus the builtin functions and
// unit suffixes) for an expression field.
//
// It only works over a buffer the FIELD owns — writing the picked name into
// the text is the whole point, and ImGui's own scratch buffers are refilled
// from the numeric value every frame. That is what u14 phase 1 bought.
//
// Called in two halves around the InputText, because the click that picks a
// row also deactivates the field, in the same frame and before our code runs
// (imgui_widgets.cpp: any outside click clears an active InputText). So the
// popup is drawn FIRST, from the geometry and candidates cached last frame,
// and its verdict is known by the time the field reports itself deactivated:
//
//   const ExprPick pick = ExprCompletePopup(state, id, buf, sizeof buf);
//   ImGui::InputText(...);
//   ExprCompleteRefresh(state, id, buf);
//
// A pick is NOT a commit: keep the field open, re-focus it, and put the caret
// at `caret`.
//
// The new text comes back rather than being written in place: the field is
// deactivating on this very frame, and a deactivating InputText applies ITS
// text to the buffer as it goes — anything written before the InputText call
// is overwritten by it. Apply `text` after.
struct ExprPick {
  bool picked = false;
  int caret = 0;
  std::string text;
};

ExprPick ExprCompletePopup(AppState& state, ImGuiID fieldId, const char* buf,
                           size_t bufSize);
void ExprCompleteRefresh(AppState& state, ImGuiID fieldId, const char* buf);
void ExprCompleteClose();

// An InputText with completion, over a buffer the caller owns — for the
// fields that store the expression TEXT (a dimension's fx mode, a variable's
// definition, a configuration's override) rather than its value. It owns the
// re-focus a pick requires, and reports the two events those callers act on
// without the re-focus masquerading as either.
struct ExprFieldEvents {
  bool activated = false; // the user entered the field (NOT a re-focus)
  bool left = false;      // Enter or a click away: commit the text
};
ExprFieldEvents ExprCompletingInput(AppState& state, const char* label,
                                    char* buf, size_t bufSize);

} // namespace ui
