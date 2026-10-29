// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// CalculiX (`ccx`) as a study solver (docs/simulation/README.md#calculix).
//
// A solver behind the study API, not a second API: the study is planned,
// meshed and loaded by our own pipeline, and the deck carries what that
// pipeline made -- the Netgen tet10 mesh, a material per domain, the
// assembled nodal forces, the supports as boundaries and transforms. ccx
// solves it in a directory of its own; we read the displacement back, hold
// it to the residual rule against our own K, and compute everything else
// (stresses, reactions, the support-edge flag) with the code an in-house
// answer goes through. A study that needs ccx where there is none is
// refused, saying how to install it; a ccx failure is refused with ccx's
// own words. Nothing falls back to the in-house solver.
//
// ccx is GPL-2.0-or-later and runs as a separate process that talks to us
// only through files: aggregation, not a combined work.

#include "core/fem/FemSolveOptions.h"
#include "core/fem/FemSolver.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <functional>
#include <optional>
#include <ostream>
#include <string>
#include <vector>

namespace fem {

// The oldest ccx the suites run (Ubuntu 24.04's 2.21); anything older is
// refused rather than trusted.
inline constexpr int kCalculixOldestMinor = 21;

struct CalculixInstall {
  std::string path; // the executable
  int major = 0;
  int minor = 0;
  std::string Version() const; // "2.23"
  std::string Name() const;    // "calculix 2.23", as a result names it
};

// The sentence a run without ccx is refused with: every way to get one.
std::string CalculixInstallSentence();

// Finds ccx, in this order: `settingPath` (the calculix_path setting),
// LOBOLE_CCX, a `ccx` beside the executable (<prefix>/lib/lobole/ccx, or
// ccx.exe beside lobole.exe), then PATH. A path the setting or LOBOLE_CCX
// names is the one asked for: when it is not a ccx this build can run it is
// refused by name, never passed over for the next. nullopt with `why` (which
// ends with CalculixInstallSentence) when there is none.
std::optional<CalculixInstall> FindCalculix(const std::string& settingPath,
                                            std::string* why);
// Runs `ccx -v` on `path` -- once per path, size and modification time --
// and reads "This is Version 2.NN"; refuses one older than 2.21.
std::optional<CalculixInstall> ProbeCalculix(const std::string& path,
                                             std::string* why);

// What a run needs beyond the study, made where the app is (the job's
// thread) and read on the worker.
struct CalculixRun {
  CalculixInstall ccx;
  // The run directories' parent: $XDG_CACHE_HOME/lobole/calculix.
  std::string runRoot;
  int threads = 1; // OMP_NUM_THREADS and CCX_NPROC_*
  // Past it the process is killed and the run refused (0 = none).
  double timeoutSeconds = 0.0;
  // What the machine has free (0 = read MemAvailable now). A test sets it.
  uint64_t availableBytes = 0;
  // Told each run's memory estimate (bytes) before ccx starts.
  std::function<void(uint64_t)> estimated;
};

// The memory a linear static ccx run of `dofs` degrees of freedom is
// estimated to take, in bytes (SPOOLES factorises in core and has no
// out-of-core mode). Calibrated from measured runs: the comment at its
// definition says which.
uint64_t CalculixStaticMemoryEstimate(size_t dofs);
// MemAvailable, in bytes; 0 when the platform does not say.
uint64_t AvailableMemoryBytes();

// The forces SolveCases applies for one case, at every node in x, y, z
// (N): the gravity body force (`gravityAccel` mm/s^2) and the face
// tractions, integrated the same way.
bool NodalForces(const FemMesh& mesh, const std::vector<Material>& byDomain,
                 const std::vector<FaceLoad>& loads,
                 const glm::dvec3& gravityAccel, std::vector<glm::dvec3>& out,
                 std::string* error);

// The deck for one case: nodes, C3D10 elements in one ELSET per mesh domain,
// a material and section per domain, fixed nodes as *BOUNDARY, nodes held in
// a pin's frame as a cylindrical *TRANSFORM about the pin's axis with their
// held directions as *BOUNDARY in it, a tie as one *EQUATION in those
// frames, and `nodalForces` as *CLOAD (in a transformed node's frame). One
// *STATIC step printing U to the .dat (to the precision ReadDatDisplacements
// reads) and the .frd. A held node's frame is the pin's of `pins` whose
// facets have it. Refused, before anything is written, for what it cannot
// express (a node of two pins' faces held in a frame).
bool WriteDeck(std::ostream& out, const FemMesh& mesh,
               const std::vector<Material>& byDomain, const Supports& supports,
               const std::vector<StudyJobBody::PinSupport>& pins,
               const std::vector<glm::dvec3>& nodalForces,
               const std::string& title, std::string* error);

// The relative precision of a displacement ReadDatDisplacements reads: ccx
// prints U as E13.6, seven significant digits.
inline constexpr double kCalculixDatPrecision = 5e-7;
// Every node's displacement from the first U block of a ccx .dat (*NODE
// PRINT), nodes numbered 1..nNodes. False with `error` when the file lacks
// one or a node.
bool ReadDatDisplacements(const std::string& path, size_t nNodes,
                          std::vector<glm::dvec3>& out, std::string* error);
// The first line of `text` that carries "*ERROR", trimmed; empty when none.
std::string FirstCalculixError(const std::string& text);
// Removes the run directories under `root` whose process (<pid>-<n>) is no
// longer running: a failed run's directory is kept for its owner to read,
// and swept once that owner has gone.
void SweepDeadRunDirs(const std::string& root);

// The supports a static solve of `jb` holds, one per case of `rhs`: its
// fixed nodes, and with pins the contact set the in-house iteration
// (SolveWithPins) settles on, held as it is in that iteration's last solve.
// `pinRows` gets each case's pin reaction rows (contact area), which a
// result solved under those supports reports. Defined in FemStudy.cpp.
bool StaticSupportsOf(
    const StudyJobBody& jb, const std::vector<Material>& mats,
    const std::vector<std::vector<FaceLoad>>& rhs,
    const glm::dvec3& gravityAccel, const SolveOptions& options,
    std::vector<Supports>& supports,
    std::vector<std::vector<FemResults::SupportReaction>>& pinRows,
    std::string* error);

// One body of a static study solved on CalculiX, case by case: the deck
// written from `jb`, ccx run in <runRoot>/<pid>-<n> under `options.calculix`
// (cancelled by `options.cancelled`, killed past the timeout), U read back
// and handed to SolveCases as a given answer. One result per case, with the
// in-house rows a pin's contact gives. The run directory is removed on
// success and kept, and named, on failure.
bool SolveBodyOnCalculix(const StudyJobBody& jb,
                         const std::vector<Material>& mats,
                         const std::vector<std::vector<FaceLoad>>& rhs,
                         const glm::dvec3& gravityAccel,
                         const SolveOptions& options,
                         std::vector<FemResults>& out, SolveReport& report,
                         std::string* error);

// The deck the study would run for body `body` and case `caseIndex` of a
// meshed study (export_study_inp): what SolveBodyOnCalculix writes, with no
// ccx needed.
bool WriteStudyDeck(const StudyJobInput& in, size_t body, size_t caseIndex,
                    std::ostream& out, const std::string& title,
                    std::string* error);

} // namespace fem
