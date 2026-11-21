// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "core/fem/FemStudy.h"

#include <string>

namespace ui {

struct AppState;

// The Model dock on a simulation tab (docs/ui/panels.md
// #simulation-tab-the-model-panel): what the simulation holds, one row per
// thing -- its subject (and an assembly's participants), and each study
// with its mesh, supports, loads, load cases, kept regions and result.
// Rows show and select; every tool is on the toolbar, and what a row is set
// to is edited in the tool window.
void DrawSimTree(AppState& state);

// A support's or a load's kind in a word ("Fixed", "Heat source").
const char* SimLoadName(fem::LoadType type);
// The command that places a support or a load of this kind ("sim.fixed"):
// its row's glyph is that command's (CommandIcon).
const char* SimLoadCommand(fem::LoadType type);
// The command that adds a study of this analysis ("sim.add_static"), whose
// glyph its row carries.
const char* SimStudyCommand(fem::AnalysisType type);
// A load's value as its row's tail: "120 N", "0.5 MPa", "free to turn".
std::string SimLoadTail(const fem::Load& load);

} // namespace ui
