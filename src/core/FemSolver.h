#pragma once

// Linear-static FEM solver: tet10 isotropic elasticity on Eigen. GUI-free core,
// unit system mm-N-MPa (lengths/displacements mm, forces N, stresses/moduli
// MPa). No Netgen dependency — it consumes a FemMesh (plain data). Gated
// HAVE_FEM_SOLVER on Eigen availability, like the mesher is gated on Netgen.

#include "core/FemMesher.h"
#include "core/FemStudy.h"

#include <glm/glm.hpp>

#include <array>
#include <optional>
#include <string>
#include <vector>

namespace fem {

// Isotropic linear-elastic material in solver units.
struct Material {
  double youngMPa = 0.0;  // Young's modulus, MPa (E in GPa * 1000)
  double poisson = 0.0;
  // Mass density in tonne/mm^3 (the consistent mm-N-t-s unit system, so
  // frequencies come out in rad/s -> Hz). Convert the library's kg/m^3 by
  // *1e-12. 0 = unused (static without gravity).
  double densityTPerMM3 = 0.0;
};

// A traction boundary condition on one B-Rep face, given as the boundary
// triangles (indices into FemMesh::boundary) that lie on it.
struct FaceLoad {
  LoadType type = LoadType::Force;  // Force or Pressure (Fixed is a constraint)
  std::vector<int> tris;            // indices into FemMesh::boundary
  double magnitude = 0.0;           // total N (Force) or MPa (Pressure)
  glm::dvec3 direction{0, 0, 0};    // Force only (need not be normalized)
};

// Solved fields, one entry per mesh node.
struct FemResults {
  std::vector<glm::dvec3> displacement;  // mm
  std::vector<double> vonMises;          // MPa

  double maxVonMises = 0.0;
  int maxVmNode = -1;
  glm::dvec3 maxVmAt{0, 0, 0};

  double maxDisplacement = 0.0;
  int maxDispNode = -1;
  glm::dvec3 maxDispAt{0, 0, 0};

  // Modal analysis results (empty for a static study). One entry per computed
  // mode, ascending frequency; displacement is the normalized mode shape.
  struct ModeShape {
    double frequencyHz = 0.0;
    std::vector<glm::dvec3> displacement;  // normalized (max |u| = 1)
  };
  std::vector<ModeShape> modes;
};

// Solves K u = f with the given fixed nodes (all three DOF zeroed, eliminated
// from the system), face tractions, and an optional body force (gravity
// acceleration in mm/s^2; needs material.densityTPerMM3). Returns std::nullopt
// and sets *error on a singular system or when built without Eigen.
std::optional<FemResults> Solve(const FemMesh& mesh, const Material& material,
                                const std::vector<int>& fixedNodes,
                                const std::vector<FaceLoad>& loads, std::string* error,
                                const glm::dvec3& gravityAccel = glm::dvec3(0, 0, 0));

// Solves the generalized eigenproblem K phi = omega^2 M phi for the lowest
// `numModes` natural frequencies + mode shapes (consistent mass matrix). Needs
// at least one fixed support (v1) and material.densityTPerMM3 > 0. Returns
// std::nullopt and sets *error on failure or when built without Spectra.
std::optional<std::vector<FemResults::ModeShape>> SolveModal(
    const FemMesh& mesh, const Material& material,
    const std::vector<int>& fixedNodes, int numModes, std::string* error);

// True when the build links Eigen (static solver) / Spectra (modal).
bool SolverAvailable();
bool ModalAvailable();

// ---- Element-level hooks (unit tests + diagnostics) -----------------------
// These expose the tet10 element math directly so it can be pinned without a
// mesher. `nodes` follow the canonical FemMesh ordering (corners 0..3, then
// edges (0,1)(1,2)(0,2)(0,3)(1,3)(2,3)); material is (Young's modulus MPa,
// Poisson). Available only under HAVE_FEM_SOLVER.

// Element stiffness of one tet10 as a row-major 30x30 matrix; DOF order is
// node0.x, node0.y, node0.z, node1.x, ...
std::array<double, 900> Tet10Stiffness(const std::array<glm::dvec3, 10>& nodes,
                                       double youngMPa, double poisson);

// Voigt stress [xx,yy,zz,xy,yz,zx] (MPa) at natural coords (r,s,t) for a tet10
// with the given per-node displacements (mm).
std::array<double, 6> Tet10Stress(const std::array<glm::dvec3, 10>& nodes,
                                  const std::array<glm::dvec3, 10>& disp,
                                  double youngMPa, double poisson, double r,
                                  double s, double t);

// ---- Study orchestration --------------------------------------------------

// One body of a study: its mesh and solved fields (all derived/transient).
struct StudyBodyResult {
  int bodyIndex = -1;
  std::string bodyName;
  FemMesh mesh;
  FemResults results;
};

// A study's derived mesh + results, cached on the Document (never serialized).
// Stale when solvedAtVersion != Document::Version(). Maxima are aggregated
// across all solved bodies for the summary an engineer reads.
struct StudyRuntime {
  uint64_t solvedAtVersion = 0;
  std::vector<StudyBodyResult> bodies;
  double maxVonMises = 0.0;
  glm::dvec3 maxVmAt{0, 0, 0};
  std::string maxVmBody;
  double maxDisplacement = 0.0;
  glm::dvec3 maxDispAt{0, 0, 0};
  std::string maxDispBody;
};

// (Document is forward-declared at global scope by FemStudy.h.)

// Meshes and solves the study across every body it loads: evaluates the
// expressions, resolves refs, validates (material has stiffness data, each
// loaded body has a fixed support), then meshes and solves. On any failure
// sets study.error and *error and returns false. Fills `rt` (not stamped —
// the caller stamps solvedAtVersion and stores it in the Document cache).
bool RunStudy(const Document& doc, Study& study, StudyRuntime& rt, std::string* error);

}  // namespace fem
