// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

namespace ui {

struct AppState;

// The Model panel on a simulation tab (DocKind::Simulation): what the
// simulation analyses, then each of its studies, read-only. The tab's own
// window -- editing the studies, running them -- is a later step
// (docs/plans/simulation-document.md#stages); until then this says what
// the tab holds. Draws nothing on any other tab.
void DrawSimTree(AppState& state);
// Its first row alone (###sim_subject): what the simulation analyses, its
// name as the subject's tab has it, "missing" when that tab is gone. The
// study panel heads its list with it on a simulation tab.
void DrawSimSubjectRow(AppState& state);

} // namespace ui
