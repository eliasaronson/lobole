// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

namespace ui {
struct AppState;

// The sheet-metal rules and the bends they produced, as a section of the
// Variables panel — its neighbours there are tolerances and units, the other
// document-level statements about a part that are not features.
//
// Unlike those two this one IS geometry: changing the gauge rebuilds every
// flange, which is why every edit goes through SetSheetMetalRulesCommand
// rather than straight onto the document.
void DrawSheetMetalSection(AppState& state);

} // namespace ui
