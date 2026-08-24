#pragma once

#include "ui/AppState.h"

namespace ui {

// Main menu bar: File / Edit / View / Help. Keyboard bindings are NOT handled
// here — they live in the one command table (ui/Commands.h) and are dispatched
// once per frame from DrawUI.
void DrawMainMenuBar(AppState& state);

} // namespace ui
