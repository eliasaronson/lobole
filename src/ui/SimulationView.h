// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// What the simulation viewport shows and the lists beside it
// (docs/simulation/README.md#the-mesh-you-look-at,
// docs/simulation/README.md#results-display,
// docs/simulation/README.md#checking-a-study): the mesh the Mesh action
// kept, the field on show with its range and extremes, the whole result a
// VTU export writes, and the study check. The panel (StudyPanel.cpp) and
// the RPCs (RemoteSimulation.cpp) both read these, so a row in one is a row
// in the other.

#include "core/fem/FemCheck.h"
#include "core/fem/FemFields.h"
#include "core/fem/SimGeometry.h"
#include "ui/AppState.h"
#include "ui/StudyHost.h"

#include <glm/glm.hpp>

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ui {

// --- The mesh ---------------------------------------------------------------

// A study's mesh settings as text: what a kept mesh is compared with to
// say whether it is still the study's.
std::string MeshSettingsKey(const fem::Study& s);
// The mesh the Mesh action last made for the study on the ACTIVE tab (a
// part's or an assembly's); null when there is none.
const MeshReport* StudyMesh(const AppState& state, uint64_t studyId);
// Whether it is no longer the study's: the geometry or the mesh settings
// moved since. A load edit does not make it stale.
bool StudyMeshStale(const AppState& state, uint64_t studyId);
// Keeps `report` as the study's mesh on the active tab (stamping its epoch).
void KeepStudyMesh(AppState& state, uint64_t slotId, uint64_t studyId,
                   MeshReport report);
// Meshes the study now, on this thread, and keeps the result: the route an
// assembly study takes (its plan is made of the placed parts, on the main
// thread) and the synchronous mesh_study's. False with *error on a refusal.
bool MeshStudyNow(AppState& state, uint64_t studyId, std::string* error);
// The name of face `faceIndex` of the meshed body, for a report row: its
// element name on a part study, "face N" where there is none (an
// assembly's imprinted shape).
std::string MeshFaceName(const AppState& state, const MeshReport::Body& body,
                         int faceIndex);

// One row of the mesh report: what it says, how bad it is, and the place it
// frames when clicked.
struct MeshFinding {
  enum class Level { Info, Warning };
  Level level = Level::Info;
  std::string code; // worst_element | thin_wall | straightened |
                    // refinement | volume | interface | unshared | loose
  std::string text;
  std::string body;
  bool hasPlace = false;
  glm::dvec3 at{0.0};
  double radiusMm = 0.0; // how much to frame round `at`
  int tet = -1;          // worst_element: its index in the body's mesh
};
std::vector<MeshFinding> MeshFindings(const AppState& state,
                                      const MeshReport& report);
// Frames `at` (mm, the tab's world) with `radiusMm` round it.
void FrameSimulationPlace(AppState& state, const glm::dvec3& at,
                          double radiusMm);

// --- The field on show ------------------------------------------------------

// fem's field for a SimulationState::Field that is one (the stress and
// thermal fields); nullopt for None, Shape, Density and the mesh views.
std::optional<fem::ResultField> ResultFieldOf(SimulationState::Field f);
SimulationState::Field FieldOfResult(fem::ResultField f);

struct ShownField {
  uint64_t key = 0;
  bool mesh = false;  // the mesh view
  bool plain = false; // nothing coloured: no colour bar
  std::string title;
  std::string unit;
  // The field's own extent over what is drawn, and where its ends are (in
  // the tab's world, undeformed).
  double lo = 0.0, hi = 0.0;
  bool hasExtremes = false;
  glm::dvec3 loAt{0.0}, hiAt{0.0};
  glm::dvec3 loDisp{0.0}, hiDisp{0.0}; // the displacement there, for the warp
  std::string loBody, hiBody;
  // What the colour bar spans: lo/hi unless a clamp set an end.
  double barLo = 0.0, barHi = 0.0;
  bool under = false, over = false; // values lie past that end
  // A factor's own bar stops at a few times its least value, and what is
  // safer than that takes the top colour: the top tick reads "and above".
  bool topOpen = false;
  bool reversed = false;            // the low end is the hot one
  bool normalized = false;          // a mode shape, a density: no unit
};

// Why `field` cannot be shown on the active study's result; empty when it
// can. The panel disables the choice with this as its reason and
// set_result_display refuses with it.
std::string FieldRefusal(AppState& state, uint64_t studyId,
                         SimulationState::Field field);
// The clamp that puts the over colour at the material's yield: the least
// yield strength over the solved bodies (MPa); nullopt when none has one.
std::optional<double> YieldClampMPa(AppState& state, uint64_t studyId);

// --- The whole result -------------------------------------------------------

// The whole result behind the study's stored one on the active tab -- its
// elements and interior, as it was solved, which the session remembers by
// input hash (ui::RememberStudyResult) -- or null when it holds only the
// surface the project stores (a reopened result).
std::shared_ptr<const fem::StudyRuntime> FullStudyResult(const AppState& state,
                                                         uint64_t studyId);
// What export_study_result and the panel's Export write
// (docs/simulation/README.md#vtu-export): the study's whole result as a
// .vtu at `path`, `caseIndex` picking a load case (-1: the first). Checks
// everything before the file is touched; false with *error on a refusal.
struct VtuExport {
  size_t points = 0, cells = 0, bytes = 0;
  std::vector<std::string> pointData, cellData;
};
bool ExportStudyResultVtu(AppState& state, uint64_t studyId,
                          const std::string& path, int caseIndex,
                          VtuExport* out, std::string* error);

// --- The check --------------------------------------------------------------

struct StudyCheck {
  uint64_t key = 0;
  uint64_t studyId = 0;
  std::vector<fem::StudyProblem> problems;
};
// Every problem a run of the study would refuse
// (docs/simulation/README.md#checking-a-study): fem::CheckStudy on a part,
// with the kept mesh when it is current; on an assembly what the planner
// refuses (one row: its two planners become one with the simulation
// document); and on either a kept mesh that is stale, and a CalculiX study
// with no ccx to run it. Read-only.
// `thorough` also runs the planner where nothing itemised is wrong (the
// RPC's answer); the panel's cached list leaves that to Run.
std::vector<fem::StudyProblem> CheckStudyNow(AppState& state, uint64_t studyId,
                                             bool thorough = true);
// The same, cached against the study's text and the document's version:
// what the panel draws each frame.
const StudyCheck& CachedStudyCheck(AppState& state, uint64_t studyId);

// --- The geometry step -------------------------------------------------------

// The geometry check of the active study (fem::CheckSimGeometry against the
// project now), cached against the simulation's version and its subject's
// stamps: what the tool window lists. `error` when the study is refused.
struct SimGeometryCheck {
  std::string key;
  uint64_t studyId = 0;
  fem::GeometryCheck check;
  std::string error;
};
const SimGeometryCheck& CachedGeometryCheck(AppState& state, uint64_t studyId);

// What the simulation in front keeps on disk with the project
// (docs/simulation/README.md#how-a-simulation-is-saved), in bytes as a save
// writes them: each stored result's, each stored mesh's, and the sum.
// Cached against the store's epoch.
struct SimStoredBytes {
  std::map<uint64_t, size_t> results; // by study id
  std::map<uint64_t, size_t> meshes;  // by mesh key
  size_t total = 0;
};
const SimStoredBytes& SimStoredBytesOf(AppState& state);
// "4.2 MB", "380 kB", "120 B".
std::string ByteSize(size_t bytes);

} // namespace ui
