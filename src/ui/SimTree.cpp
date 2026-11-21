// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "ui/SimTree.h"

#include "core/fem/SimDocument.h"
#include "ui/AppState.h"
#include "ui/DocumentSlots.h"
#include "ui/Widgets.h"

#include "IconsFontAwesome6.h"
#include "imgui.h"

#include <optional>
#include <string>

namespace ui {

void DrawSimSubjectRow(AppState& state) {
  if (!state.sim)
    return;
  const fem::SimSubject& subject = state.sim->Subject();
  // The subject's name as its tab has it now; the document's copy is
  // what a deleted subject is still called.
  const std::optional<size_t> at = SlotIndexById(state, subject.docId);
  const bool gone = !at;
  ListRowSpec row;
  row.glyph =
      subject.kind == fem::SubjectKind::Assembly ? ICON_FA_CUBES : ICON_FA_CUBE;
  row.label = (gone ? subject.name : state.parts[*at].name) + "###sim_subject";
  row.dimmed = gone;
  row.tail = gone ? "missing" : fem::SubjectKindToString(subject.kind);
  if (gone)
    row.tooltip = "The " + std::string(fem::SubjectKindToString(subject.kind)) +
                  " this simulation analyses is no longer in the project";
  ListRow(row);
}

} // namespace ui
