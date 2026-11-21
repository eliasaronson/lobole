// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "core/fem/FemStudy.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

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

// The list selection's id of a row that has none of its own
// (SelectionItem::Kind::SimCase, SimKeep, SimKeepOut): its study and its
// place in that study's list.
inline uint64_t SimRowKey(uint64_t studyId, size_t index) {
  return (studyId << 20) | static_cast<uint64_t>(index & 0xFFFFF);
}
inline std::pair<uint64_t, size_t> SimRowOf(uint64_t key) {
  return {key >> 20, static_cast<size_t>(key & 0xFFFFF)};
}
// A row of the Geometry group (SelectionItem::Kind::SimGeometry): the
// suppressed feature, the symmetry plane or the refinement at `index` of
// the simulation's lists.
enum class GeometryRow : uint64_t {
  Suppressed = 1,
  Symmetry = 2,
  Refinement = 3
};
inline uint64_t GeometryRowKey(GeometryRow row, size_t index = 0) {
  return (static_cast<uint64_t>(row) << 40) | static_cast<uint64_t>(index);
}
inline std::pair<GeometryRow, size_t> GeometryRowOf(uint64_t key) {
  return {static_cast<GeometryRow>(key >> 40),
          static_cast<size_t>(key & ((uint64_t{1} << 40) - 1))};
}

} // namespace ui
