// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// What the field overlay's sync (SimulationActions.cpp) and the mesh job
// share with SimulationView.cpp: the builders that turn a kept mesh or a
// result into FieldRenderer triangles and the ShownField the legend reads.

#include "render/FieldRenderer.h"
#include "ui/BackgroundJobs.h"
#include "ui/SimulationView.h"
#include "ui/TopoJob.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace ui {

// A kept mesh from what a mesh run made (the meshes are moved in).
MeshReport MakeMeshReport(std::vector<fem::StudyBodyMesh> meshes,
                          std::vector<fem::MeshBodyReport> reports);
// The study's mesh key as it is now (SimMeshKeyNow).
std::string StudyMeshKeyNow(AppState& state, const StudyHost& host,
                            const fem::Study& s);
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

// The run helpers a simulation's runs share with the part's and the
// assembly's (SimulationActions.cpp; SimRunActions.cpp uses them).
// The GPU offered to a solve large enough to go iterative.
void OfferGpuSolver(const AppState& state, fem::SolveOptions& o);
// What a study job's worker solves with: the job's cancel, its status, the
// GPU offered.
fem::SolveOptions JobSolveOptions(const AppState& state, BackgroundJob* raw,
                                  StudyJob* job);
// A CalculiX run that reports its memory estimate and progress to `job`.
std::shared_ptr<const fem::CalculixRun>
WithEstimateTo(const std::shared_ptr<const fem::CalculixRun>& run,
               StudyJob* job, BackgroundJob* raw = nullptr);
// The solver a result records: the one that ran, else the type's.
std::string SolverOf(const fem::StudyRuntime& rt, fem::AnalysisType type);
// A topology job's linear solver, picked on the main thread.
void PickTopologySolver(const AppState& state, TopoJob& job);
// Starts `job`'s optimisation on `bg`'s thread.
void LaunchTopologyWorker(BackgroundJob& bg,
                          const std::shared_ptr<TopoJob>& job);
// A landed topology job's result, its solver named as it ran.
fem::StudyRuntime TopologyRuntimeOf(TopoJob& j);

} // namespace ui
