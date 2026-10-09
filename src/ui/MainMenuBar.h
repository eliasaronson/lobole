// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "ui/AppState.h"

namespace ui {

// Main menu bar: File / Edit / View / Help. Keyboard bindings are NOT handled
// here — they live in the one command table (ui/Commands.h) and are dispatched
// once per frame from DrawUI.
void DrawMainMenuBar(AppState& state);

} // namespace ui
