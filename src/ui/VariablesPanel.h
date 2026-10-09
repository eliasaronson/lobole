// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "ui/AppState.h"

namespace ui {

// "Variables" panel: the document's parameter table (name / expression /
// value / comment). Add, edit, rename, and delete parameters; every change
// goes through the command stack (undoable) and regenerates the model, so
// editing a variable rebuilds the sketches and features that reference it.
void DrawVariablesPanel(AppState& state);

} // namespace ui
