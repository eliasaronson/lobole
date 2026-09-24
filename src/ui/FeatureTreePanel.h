#pragma once

#include "core/Feature.h"
#include "ui/AppState.h"

namespace ui {

// "Model" panel: the parametric feature history (selectable, deletable via
// context menu) and the bodies it produces.
void DrawFeatureTreePanel(AppState& state);

// The state badge for one feature row, or nullptr when the row is normal.
//
// The four states a row can be in — failed, warned, suppressed, rolled past —
// were distinguished by COLOUR ALONE: red, amber, dimmed, dimmed. Red against
// amber is the pair red-green colour blindness confuses, and the last two were
// not distinguishable by anyone. A glyph is a second channel, and it is also
// the only one that survives a screenshot posted in grayscale.
//
// Separated from the drawing because the CHOICE is the logic — precedence
// between the four states, which the panel gets right and nothing checked.
// See tests/test_actions.cpp.
// `suppressed` is the EFFECTIVE state (Document::EffectiveSuppressed): the
// feature's own flag, or the active configuration turning it off or on.
const char* FeatureRowBadge(const Feature& feature, bool rolledPast,
                            bool suppressed);

// The features that MADE what is picked: a face's stable name is minted by
// the feature that created it (ElementMap — an extrude's side face is
// "4/g7", a fillet's is the fillet's), so a picked face says which row to
// look at. The tree MARKS those rows; it does not select them, because the
// feature is not what was clicked.
std::vector<uint64_t> FeaturesThatMadeThePick(const AppState& state);

} // namespace ui
