#pragma once

#include <span>
#include <string_view>

namespace ui {

// The glyph a command shows wherever it appears: its toolbar button, its row
// in a flyout, and the tree row of whatever it made (the feature tree, the
// mate list, the CAM operation list). One table, so a row cannot drift from
// the button that made it — four hand-kept copies had
// (docs/ui/README.md#icons).
//
// Glyphs are Font Awesome's (ICON_FA_*) where it has the word, and Designer's
// own (ICON_DS_*, tools/icons/build_icon_font.py) for the CAD vocabulary it
// does not.
struct CommandIconEntry {
  const char* command;
  const char* icon;
};

std::span<const CommandIconEntry> CommandIcons();

// The command's glyph, or kUnknownIcon for a command the table does not know.
const char* CommandIcon(std::string_view command);

extern const char* const kUnknownIcon;

} // namespace ui
