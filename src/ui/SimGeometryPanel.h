// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// The geometry step in the simulation window (docs/simulation/README.md
// #the-geometry-step, docs/ui/panels.md#simulation-the-tool-window): the
// toolbar's Suppress, Symmetry and Refine, what a face clicked with one of
// them does, and the tool window of the tree's Geometry group with the
// geometry check. The tree's rows are SimTree.cpp's.

#include "core/ElementMap.h"
#include "core/fem/SimDocument.h"

#include <cstdint>
#include <string>

namespace ui {

struct AppState;

// Sets the geometry step of the simulation in front to `geometry`, one undo
// step, when fem::SimGeometryRefusal accepts it; otherwise nothing changes
// and the refusal is returned.
std::string SetSimGeometry(AppState& state, fem::SimGeometry geometry);

// A face clicked with Suppress, Symmetry or Refine armed: suppress the
// feature that made it, cut on it, or refine the mesh on it. A refusal is
// left in the tool window (SimulationState::placeRefusal) and nothing is
// changed. True when the tool was one of these.
bool SimulationGeometryPick(AppState& state, uint64_t occurrenceId,
                            const ElementRef& face);

// The tool window's body for an armed geometry tool, and for the Geometry
// group in focus.
void DrawSimGeometryTool(AppState& state);
void DrawSimGeometryWindow(AppState& state);

} // namespace ui
