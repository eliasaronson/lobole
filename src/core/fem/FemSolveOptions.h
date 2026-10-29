// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// How a static solve solves K u = f (docs/simulation/README.md#the-solver):
// which linear solver, and how a caller stops one in flight. Its own header
// so a job runner can fill one in without the solver's declarations.

#include <glm/glm.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace fem {

// The converge_auto_s preference's default, seconds
// (docs/simulation/README.md#a-peak-that-has-settled): on `auto`, a static
// run refines round its peak while a solve takes less than this.
inline constexpr double kConvergeAutoSecondsDefault = 10.0;

class TopoLinearSolver;
struct CalculixRun;

// Free DOF above which a static study is solved by preconditioned conjugate
// gradients instead of a sparse LDLT factorisation. A direct factor of a
// tet10 mesh fills in steeply. Measured on a 100 x 20 x 20 beam (one core):
// 12.5k DOF, LDLT 0.29 s and PCG 0.38 s; 33.5k, 4.9 s against 1.7 s; 74k,
// 34 s against 5.6 s; 202k, 423 s against 26 s. Below the crossover LDLT
// is quicker and is the oracle PCG is tested against.
inline constexpr int kIterativeAboveDofs = 20000;

struct SolveOptions {
  // Solve iteratively above this many free DOF (0 = always, a large number
  // = never).
  int iterativeAboveDofs = kIterativeAboveDofs;
  // Polled during an iterative solve; true stops it, and the solve fails
  // "cancelled". A factorisation has no interruption points, so a direct
  // solve only sees it before it starts.
  std::function<bool()> cancelled;
  // Told which solver was chosen once K's size is known (a static string,
  // CpuSolverName's kind), so a running job can say what it is waiting on.
  std::function<void(const char*)> solving;
  // Makes the external (GPU) solver an iterative solve tries first; null, or
  // a null answer with `why` set, solves on the CPU. Called on the solving
  // thread, only once the solve is known to be iterative, so a small study
  // never pays for a device.
  std::function<std::unique_ptr<TopoLinearSolver>(std::string* why)>
      makeExternal;
  // A static study whose solver is CalculiX solves through this run
  // (docs/simulation/README.md#calculix); null, such a study is refused
  // rather than solved in-house.
  std::shared_ptr<const CalculixRun> calculix;
  // An answer solved elsewhere: every node's displacement (mm, x y z), one
  // list per case. SolveCases then factors nothing. It holds the answer to
  // the residual rule against its own K, with the floor the answer's
  // printed precision puts under it (`givenPrecision`, relative, per
  // value), and computes the stresses and reactions from it.
  std::shared_ptr<const std::vector<std::vector<glm::dvec3>>> given;
  double givenPrecision = 0.0;
  std::string givenName; // the solver it came from, as the report names it
  // What the machine has free, bytes, for a run that refuses one that will
  // not fit (a buckling study's factor); 0 = read MemAvailable then. A test
  // sets it.
  uint64_t availableBytes = 0;
};

// What a solve did, for the study's result and its status.
struct SolveReport {
  std::string solver;
  int iterations = 0;         // conjugate-gradient iterations, all cases
  double worstResidual = 0.0; // max over cases of |K u - f| / |f|
  // A given answer's: the largest residual its printed precision alone
  // can leave, |K| |u| x precision / |f|, over the cases (0 otherwise).
  double worstResidualFloor = 0.0;
  // A given answer's energy balance, |u . (K u - f)| / |u . f|, worst over
  // the cases: what the printed precision barely touches (it moves it by
  // delta . f, first order in the precision), where a deck that differs
  // from the study moves it first order in the difference.
  double worstEnergyResidual = 0.0;
  // Why the external solver was not used, when one was offered and an
  // iterative solve ran on the CPU instead. Empty otherwise.
  std::string externalDeclined;
};

// The names a static solve reports.
inline constexpr const char* kDirectSolverName = "cpu (Eigen SimplicialLDLT)";
inline constexpr const char* kIterativeSolverName =
    "cpu (PCG, incomplete Cholesky)";
inline constexpr const char* kGpuSolverName =
    "gpu (Vulkan, mixed-precision PCG)";

} // namespace fem
