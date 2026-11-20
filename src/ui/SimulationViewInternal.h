// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// What the field overlay's sync (SimulationActions.cpp) and the mesh job
// share with SimulationView.cpp: the builders that turn a kept mesh or a
// result into FieldRenderer triangles and the ShownField the legend reads.

#include "render/FieldRenderer.h"
#include "ui/SimulationView.h"

#include <cstdint>
#include <string>
#include <vector>

namespace ui {

// A kept mesh from what a mesh run made (the meshes are moved in).
MeshReport MakeMeshReport(std::vector<fem::StudyBodyMesh> meshes,
                          std::vector<fem::MeshBodyReport> reports);
// The study's mesh key as it is now (its settings; on an assembly also the
// assembly and its parts).
std::string StudyMeshKeyNow(AppState& state, const StudyHost& host,
                            const fem::Study& s);
// An assembly study meshed on this thread, each mesh with its report.
// SimulationActions.cpp (it owns the assembly plan).
bool MeshAssemblyStudyWithReports(AppState& state, AssemblyDocument& asmDoc,
                                  uint64_t studyId,
                                  std::vector<fem::StudyBodyMesh>& meshes,
                                  std::vector<fem::MeshBodyReport>& reports,
                                  std::string* error);

// What the overlay is rebuilt on: the kept mesh, the view and the section
// planes; the result, the field, its case or mode and the legend's clamp.
uint64_t MeshOverlayKey(const AppState& state, const MeshReport& report);
uint64_t ResultOverlayKey(const AppState& state, const StudyHost& host,
                          const fem::StudyRuntime& rt, bool modal);
void BuildMeshOverlay(AppState& state, const MeshReport& report,
                      std::vector<FieldVertex>& verts,
                      std::vector<uint32_t>& indices, FieldStyle& style,
                      ShownField& shown);
// False when the field does not apply to the study or its result.
bool BuildResultOverlay(AppState& state, const StudyHost& host,
                        const fem::StudyRuntime& rt, const fem::Study& study,
                        std::vector<FieldVertex>& verts,
                        std::vector<uint32_t>& indices, FieldStyle& style,
                        ShownField& shown);

} // namespace ui
