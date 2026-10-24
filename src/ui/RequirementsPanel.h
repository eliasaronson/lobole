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
  // Measures, in place, every `expression` requirement whose inputs moved
  // (docs/requirements/README.md#expression): microseconds each, so it never
  // waits for a Check.
  void (*refresh)(AppState&) = nullptr;
  // The design sweep (docs/requirements/README.md#design-sweep), with the
  // parameters design_sweep takes: what it would run ({planned, cap,
  // variables}, or throws the refusal the RPC would), starting it (the job
  // id, or throws), and a finished sweep's answer (null while it runs or
  // for a job that was not one).
  nlohmann::json (*sweepPlan)(AppState&, const nlohmann::json&) = nullptr;
  uint64_t (*sweepStart)(AppState&, const nlohmann::json&) = nullptr;
  nlohmann::json (*sweepResult)(AppState&, uint64_t job) = nullptr;
};
void SetRequirementsEngine(const RequirementsEngine& engine);
const RequirementsEngine& GetRequirementsEngine();

// The engine's refresh, when one is installed: called once a loop iteration
// (PollBackgroundJobs), so a value edited anywhere -- a panel, an undo, an
// RPC -- turns an expression requirement's row without a Check.
void RefreshRequirementsInPlace(AppState& state);

// The Requirements panel (docs/requirements/README.md#the-panel): each
// requirement's state, value and bound, the whole finding of one that does
// not pass on a line under it, and Check. Exists only while the
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
