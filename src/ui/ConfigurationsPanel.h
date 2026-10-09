// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "ui/AppState.h"

namespace ui {

// The Configurations panel for a part tab (tabbed behind Model/Variables):
// the active-configuration selector, the configuration list, and the active
// config's parameter-override table + feature-suppression toggles. Configs are
// applied by regenerating against them (docs/configurations/README.md).
void DrawConfigurationsPanel(AppState& state);

} // namespace ui
