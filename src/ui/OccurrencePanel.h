#pragma once

#include "ui/AppState.h"

#include <string>

// The assembly panel (occurrences, mates, relations, BOM), the mate tool
// window, and the two assembly viewport modes that float over the model.
//
// Split out of ui/AppActions.h, which had grown to ~180 declarations spanning
// every domain in the app and is included by 40 translation units — so every
// panel and RPC module paid for all of it. See
// docs/architecture-review-2026-08-24.md.
namespace ui {

// The mate session's Tool-window contents (connector slot rows, flip/error,
// accept/cancel). Lives with the other mate UI in OccurrencePanel.cpp;
// called from DrawToolWindow.
void DrawMateToolWindow(AppState& state);

// The occurrence list + properties editor for an active assembly tab (drawn
// where the feature tree is for a part). Defined in OccurrencePanel.cpp.
void DrawOccurrencePanel(AppState& state);

// The assembly's two viewport MODES, drawn in the floating tool window rather
// than in the Model dock (docs/ui/u15-assembly-panel.md): both pose the
// occurrences and are driven with a slider or a drag gizmo, so they belong
// over the model they are moving. Called from DrawToolWindow, which gives
// them the one-at-a-time slot — an exploded view and a motion study placing
// the same occurrences at once has no meaning.
void DrawExplodeToolWindow(AppState& state);
void DrawMotionToolWindow(AppState& state);

// The assembly panel's editors, floating in the tool window
// (docs/ui/assembly-panel-tidy-plan.md): a double-click on an occurrence's or
// a relation's row opens its properties there. Drawn from DrawToolWindow. A
// mate edits in the mate tool's own Edit Mate mode (StartMateEdit).
void EditAssemblyOccurrence(AppState& state, uint64_t occId);
void EditAssemblyRelation(AppState& state, uint64_t relId);
void DrawAssemblyEditWindow(AppState& state);

// The bill of materials as a floating window with real columns, opened from
// the assembly toolbar (state.bomWindowOpen).
void DrawBomWindow(AppState& state);

// The name of the occurrence an id refers to (its label, else its part name).
// Shared with the Configurations panel, whose assembly arm lists occurrences
// by the same name this panel does.
std::string OccurrenceLabel(const AppState& state,
                            const AssemblyDocument& asmDoc, uint64_t occId);

} // namespace ui
