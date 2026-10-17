// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>

namespace ui {

struct AppState;

// What the panel asks of the requirements engine, which measures through the
// RPC handlers (src/remote/RemoteRequirements.cpp) and so lives beside them:
// the rows list_requirements answers, and starting the check job. The
// engine installs these when its module is linked; the panel draws nothing
// without them.
struct RequirementsEngine {
  nlohmann::json (*rows)(AppState&) = nullptr;
  uint64_t (*check)(AppState&, bool recompute) = nullptr;
};
void SetRequirementsEngine(const RequirementsEngine& engine);

// The Requirements panel (docs/requirements/README.md#the-panel): each
// requirement's state, value and bound, and Check. Exists only while the
// project states requirements, like the Conflicts panel.
void DrawRequirementsPanel(AppState& state);

// What the panel drew for requirement `id` on its last frame, for the GUI
// suite to read back: the Value cell and the width it had, and the finding
// line under the row (whole, wrapped) with the width it was wrapped in.
// Null when the row was not drawn.
struct RequirementRowDrawn {
  std::string value;
  float valueTextWidth = 0.0f, valueCellWidth = 0.0f;
  std::string finding;
  float findingWidth = 0.0f, findingCellWidth = 0.0f;
};
const RequirementRowDrawn* RequirementRowDrawnForTest(uint64_t id);

} // namespace ui
