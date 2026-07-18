#pragma once

#include "ui/AppState.h"

namespace ui {

// Ctrl+K command palette (U6): a centered fuzzy finder over the
// model-context command surface — create features, file/edit/view actions,
// tools — with shortcuts shown. The palette and the menus call the same
// action functions, so it doubles as living documentation of the command
// layer (goal 5: humans, the palette and the LLM drive the same commands).
void DrawCommandPalette(AppState& state);

} // namespace ui
