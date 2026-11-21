// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

namespace ui {

struct AppState;

// The first row of a simulation tab's Model panel (###sim_subject): what
// the simulation analyses, its name as the subject's tab has it, "missing"
// when that tab is gone. The study panel heads its list with it.
void DrawSimSubjectRow(AppState& state);

} // namespace ui
