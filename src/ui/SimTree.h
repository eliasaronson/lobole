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

} // namespace ui
