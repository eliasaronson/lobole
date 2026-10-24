// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace ui {

struct AppState;

// The Design sweep window (docs/requirements/README.md#design-sweep): one
// row per swept variable (name, from, to, samples), the planned count
// against the cap, Run with its progress and Cancel, and the answer as a
// table -- a row a point, failing cells in the fail colour, each
// variable's first failure above it. It changes nothing in the document:
// the sweep rebuilds copies.
void DrawDesignSweepWindow(AppState& state);

// Opens it on one variable ("Sweep..." on a Variables row): its range
// defaults to half to twice the variable's value, derived from it; a
// variable at zero has no default and says so. `part` 0 = the project
// variable, else the part whose local one it is.
void OpenDesignSweepForVariable(AppState& state, const std::string& name,
                                uint64_t part);

// Opens it on the ranges the requirements state ("Sweep the ranges" in the
// Requirements panel): design_sweep's from_requirements.
void OpenDesignSweepFromRequirements(AppState& state);

// What the window last drew, for the GUI suite: the job it runs (0: none),
// the planned line, each first-failure line, and per point whether it
// failed and the reason its failing cell's tooltip gives.
struct DesignSweepDrawn {
  bool open = false;
  uint64_t job = 0;
  std::string planned;
  std::vector<std::string> firstFailures;
  int points = 0, failing = 0;
  std::string firstFailingWhy;
  bool cancelled = false;
};
const DesignSweepDrawn& DesignSweepDrawnForTest();

} // namespace ui
