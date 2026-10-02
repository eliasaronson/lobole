#pragma once

namespace ui {

struct AppState;

// The Literals section of the Variables panel
// (docs/variables/README.md#literals-that-should-be-expressions): the
// check_literals lint on demand, a "Use" per literal that equals a variable,
// and extract_variables as a dry run to read and an Apply.
void DrawLiteralsSection(AppState& state);

} // namespace ui
