#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>

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

} // namespace ui
