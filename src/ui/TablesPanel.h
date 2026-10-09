// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

namespace ui {

struct AppState;

// The Tables section of the Variables panel: the project's lookup tables
// (docs/variables/README.md#tables), edited in place.
void DrawTablesSection(AppState& state);

} // namespace ui
