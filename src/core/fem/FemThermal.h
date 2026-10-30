// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// Steady heat conduction on the tet10 mesh a static study makes
// (docs/simulation/README.md#thermal): K_T T = Q, the scalar Laplace problem,
// solved in-house on the same linear solvers SolveCases uses, with CalculiX's
// *HEAT TRANSFER as its oracle. And the thermal strain a temperature field
// puts into a static solve.
//
// Units are the solver's mm-N-s with power in mW: a conductivity in W/(m K)
// is the same number in mW/(mm K), a flux or a film coefficient per m^2 is
// 1e-3 of it per mm^2, and a watt is 1e3 mW. Temperatures are degrees C.
// A Load carries what the RPC and the file give (W, W/m^2, W/(m^2 K)); it is
// converted once, where it is meshed.

#include "core/fem/FemMesher.h"
#include "core/fem/FemSolveOptions.h"
#include "core/fem/FemSolver.h"
#include "core/fem/FemStudy.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace fem {

// What a heat solve answers, per node of the mesh.
struct HeatSolution {
  std::string solver; // which linear solver ran (or the given answer's)
  std::vector<double> temperature; // degrees C
  // -k grad T, W/m^2, averaged to the node by element volume as the stress
  // is.
  std::vector<glm::dvec3> heatFlux;
  std::vector<HeatFlow> flows;
  double inW = 0.0;  // the sum of the flows into the part
  double outW = 0.0; // and out of it
  // in - out, W, and the most of it the read-back precision of a given
  // answer accounts for (0 for an in-house one).
  double imbalanceW = 0.0;
  double imbalanceFloorW = 0.0;
  // |K_T T - Q| / |Q| over the free nodes, and for a given answer the floor
  // its printed precision puts under it and the energy balance
  // |T . (K_T T - Q)| / |T . Q| (both 0 for an in-house answer).
  double residual = 0.0;
  double residualFloor = 0.0;
  double energyResidual = 0.0;
};

// An answer solved elsewhere (CalculiX): every node's temperature, and the
// relative precision each was printed to.
struct GivenTemperatures {
  const std::vector<double>* temperature = nullptr;
  double precision = 0.0;
  std::string name; // the solver it came from
};

// Solves the steady heat problem on `mesh` (conductivity per domain of
// `byDomain`, in mW/(mm K)), or, with `given`, checks that answer against
// K_T and Q instead of solving: a held node it moves, a residual above 1e-6
// plus the floor its precision accounts for, or an energy balance off by
// more, is refused, as SolveCases refuses a given displacement. Then the
// flux, the flows and the balance, whoever solved it; an in-house balance off
// by more than kHeatBalance of the throughput is refused. Refused before
// solving: a material without a conductivity, and a mesh whose heat has
// nowhere to go (no held face and no convection).
inline constexpr double kHeatBalance = 1e-6;
bool SolveHeat(const FemMesh& mesh, const std::vector<Material>& byDomain,
               const HeatLoads& loads, const SolveOptions& options,
               HeatSolution& out, std::string* error,
               const GivenTemperatures* given = nullptr);

// The heat Q puts into each node, mW, from the flux faces and the sources
// alone (the film's h T_amb term aside): what a CalculiX deck writes as
// *CFLUX. Fails on a face meshed to zero area or a source with no volume.
bool HeatNodalLoads(const FemMesh& mesh, const HeatLoads& loads,
                    std::vector<double>& out, std::string* error);

// A thermal load's quantities, from what the RPC and the file give to the
// solver's units: W -> mW, W/m^2 -> mW/mm^2, W/(m^2 K) -> mW/(mm^2 K).
// Converted once, where a load is meshed (MeshPlannedStudy).
inline constexpr double kMilliwattsPerWatt = 1e3;
inline constexpr double kWm2ToMWmm2 = 1e-3;

} // namespace fem
