// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// Linear-static FEM solver: tet10 isotropic elasticity on Eigen. GUI-free core,
// unit system mm-N-MPa (lengths/displacements mm, forces N, stresses/moduli
// MPa). No Netgen dependency — it consumes a FemMesh (plain data). Gated
// HAVE_FEM_SOLVER on Eigen availability, like the mesher is gated on Netgen.

#include "core/fem/FemMesher.h"
#include "core/fem/FemSolveOptions.h"
#include "core/fem/FemStudy.h"

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

class AssemblyDocument;
struct Body;
namespace CADMaterials {
struct MaterialProperties;
}

namespace fem {

// Isotropic linear-elastic material in solver units.
struct Material {
  double youngMPa = 0.0; // Young's modulus, MPa (E in GPa * 1000)
  double poisson = 0.0;
  // Mass density in tonne/mm^3 (the consistent mm-N-t-s unit system, so
  // frequencies come out in rad/s -> Hz). Convert the library's kg/m^3 by
  // *1e-12. 0 = unused (static without gravity).
  double densityTPerMM3 = 0.0;
  // The library's strengths, MPa (0 = none): yield, and across a printed
  // part's layers. Not read by a solve; a buckling study's yield and
  // interlayer factors are (docs/simulation/README.md#buckling).
  double yieldMPa = 0.0;
  double interlayerMPa = 0.0;
  // The library's hardening curve (docs/simulation/README.md#plasticity):
  // the von Mises stress (MPa) against the equivalent plastic strain, from
  // (yield, 0) up; empty = none, and a plastic study is refused rather than
  // assume one. Read only by a plastic run (*PLASTIC).
  std::vector<std::pair<double, double>> hardening;
  // The library's thermal numbers (docs/simulation/README.md#thermal), 0 =
  // unknown: the conductivity in W/(m K), which is mW/(mm K) in the
  // solver's units, read by a heat solve; the linear expansion coefficient,
  // 1/K, read by a thermal load. And the service temperature (degrees C)
  // with what it is, which a thermal result's margin reads; NaN = none.
  double conductivityWmK = 0.0;
  double expansionPerK = 0.0;
  double serviceMaxC = std::numeric_limits<double>::quiet_NaN();
  std::string serviceBasis;
  // The conductivity is the bulk resin's and a printed part conducts less
  // (air between its roads and layers): a thermal result warns.
  bool bulkConductivity = false;
};

// The solver's material for a library row: every number in solver units.
Material MaterialFromLibrary(const CADMaterials::MaterialProperties& mp);

// Empty when a thermal study (`needConductivity`) or a thermal load
// (`needExpansion`) can take `m` (the material of `label`); otherwise why
// not, naming the materials the library has the number for (`with`).
std::string ThermalMaterialRefusal(const Material& m, const std::string& label,
                                   bool needConductivity, bool needExpansion,
                                   const std::string& with);

// Empty when a plastic study can take `m` (the material of `label`): it has
// a yield strength and a hardening curve that starts at it, its plastic
// strain rising and its stress not falling. Otherwise why not, naming the
// materials the library has a curve for (`withCurves`).
std::string HardeningRefusal(const Material& m, const std::string& label,
                             const std::string& withCurves);

// A traction boundary condition on one B-Rep face, given as the boundary
// triangles (indices into FemMesh::boundary) that lie on it.
// Torque and Remote are fields over the whole face set `tris` covers: a face
// an assembly's imprint split is one FaceLoad over all its pieces, so the
// field is the unsplit face's.
struct FaceLoad {
  LoadType type = LoadType::Force; // Fixed is a constraint, not a FaceLoad
  std::vector<int> tris;           // indices into FemMesh::boundary
  // Total N (Force, Remote), MPa (Pressure), N*mm (Torque).
  double magnitude = 0.0;
  glm::dvec3 direction{0, 0, 0}; // Force, Remote (need not be normalized)
  // Torque: a point on the axis; Remote: the point the force acts at (mm).
  glm::dvec3 origin{0, 0, 0};
  glm::dvec3 axis{0, 0, 0}; // Torque: the axis direction (need not be unit)
};

// A load's traction as the solver takes it, its triangles left to the caller.
// The load's geometry must be complete (CompleteLoadGeometry).
FaceLoad MakeFaceLoad(const Load& load);

// The thermal loads of one mesh, in solver units.
struct HeatLoads {
  // A face held at a temperature: its nodes eliminated, as a fixed
  // support's DOF are.
  struct Held {
    uint64_t id = 0;
    std::vector<int> tris; // indices into FemMesh::boundary
    double temperatureC = 0.0;
  };
  // Heat into a face, mW: `totalMW` spread uniformly when `total`, else
  // `fluxMWmm2` per mm^2. Positive into the part.
  struct Flux {
    uint64_t id = 0;
    std::vector<int> tris;
    bool total = true;
    double totalMW = 0.0;
    double fluxMWmm2 = 0.0;
  };
  // A face cooled (or warmed) by a fluid at `ambientC`: h (T - T_amb) out,
  // h in mW/(mm^2 K).
  struct Film {
    uint64_t id = 0;
    std::vector<int> tris;
    double hMWmm2K = 0.0;
    double ambientC = 0.0;
  };
  // Heat generated through a body, `powerMW` in all, spread uniformly over
  // its volume. `tets` (indices into FemMesh::tets) are the body's; empty =
  // the whole mesh.
  struct Source {
    uint64_t id = 0;
    std::vector<int> tets;
    double powerMW = 0.0;
  };
  std::vector<Held> held;
  std::vector<Flux> fluxes;
  std::vector<Film> films;
  std::vector<Source> sources;
  bool Empty() const {
    return held.empty() && fluxes.empty() && films.empty() && sources.empty();
  }
};

// The heat through one thermal load, W, signed OUT of the part: a held
// face's from K_T T - Q at its nodes (each node counted once, the first load
// that holds it), a convecting face's integral of h (T - T_amb), a flux face
// and a source their applied power (negative: into the part).
struct HeatFlow {
  uint64_t id = 0;
  LoadType type = LoadType::Temperature;
  double watts = 0.0;
};

// A nodal stress in Voigt order [xx, yy, zz, xy, yz, zx], MPa.
using Stress = std::array<double, 6>;

// Solved fields, one entry per mesh node.
struct FemResults {
  std::vector<glm::dvec3> displacement; // mm
  std::vector<double> vonMises;         // MPa
  // The whole nodal stress, averaged to the node like vonMises: what a
  // stress other than von Mises is read from -- the largest principal
  // stress, or the stress across a printed part's layers
  // (docs/simulation/README.md#printed-layers). Empty on a modal result and
  // on one saved before v172.
  std::vector<Stress> stress;

  double maxVonMises = 0.0;
  int maxVmNode = -1;
  glm::dvec3 maxVmAt{0, 0, 0};

  double maxDisplacement = 0.0;
  int maxDispNode = -1;
  glm::dvec3 maxDispAt{0, 0, 0};

  // A plastic result (docs/simulation/README.md#plasticity): the equivalent
  // plastic strain at each node, extrapolated from the integration points
  // as the stress is, and its peak; and the deformation left once the load
  // is taken off again (mm, each node), and its peak. Empty on any other.
  std::vector<double> plasticStrain;
  double maxPlasticStrain = 0.0;
  int maxPeNode = -1;
  glm::dvec3 maxPeAt{0, 0, 0};
  std::vector<glm::dvec3> residualDisplacement;
  double maxResidualDisplacement = 0.0;
  glm::dvec3 maxResidualAt{0, 0, 0};

  // A static result: what each support carried (docs/simulation/README.md
  // #pin-supports), one entry per support of the body in the study's order.
  // `forceN` is the force the support exerts on the part (N, the part's
  // frame); a pin's `contactAreaMm2` is the area of its face in contact, of
  // `faceAreaMm2` (both 0 on a fixed support). Empty on a modal result and
  // on one saved before the pin support. `momentNmm` is the moment of the
  // support's nodal forces about the frame's origin (N mm): with `forceN`
  // the whole of what it carries, so a support that holds a face along its
  // length (a pin over a band, a clamp) shows the moment it takes, which a
  // lever rule over its middle does not. `hasMoment` false on a result
  // saved before v202.
  struct SupportReaction {
    uint64_t support = 0; // the load id
    LoadType type = LoadType::Fixed;
    glm::dvec3 forceN{0.0};
    glm::dvec3 momentNmm{0.0};
    bool hasMoment = false;
    double contactAreaMm2 = 0.0;
    double faceAreaMm2 = 0.0;
  };
  std::vector<SupportReaction> reactions;
  // The peak von Mises near a support's edge
  // (docs/simulation/README.md#a-peak-on-a-supports-edge): `flagged` with
  // the support and the largest von Mises farther than that support's reach
  // (SupportAwayMm) from every support's edge (`awayMPa` at `awayAt`; 0 when
  // no node is that far).
  struct SupportEdgePeak {
    bool flagged = false;
    uint64_t support = 0;
    LoadType type = LoadType::Fixed;
    double awayMPa = 0.0;
    glm::dvec3 awayAt{0.0};
  };
  SupportEdgePeak supportEdgePeak;
  // Transient (never kept or saved): the reaction at every node, the
  // internal force less the applied load (N), filled when a solve is asked
  // for reactions. Zero at a node nothing holds, to the residual.
  std::vector<glm::dvec3> nodeReaction;

  // Modal analysis results (empty for a static study). One entry per computed
  // mode, ascending frequency; displacement is the normalized mode shape.
  // A buckling study's modes carry their load factor (signed: a negative
  // one buckles under the reversed load) and no frequency.
  struct ModeShape {
    double frequencyHz = 0.0;
    std::vector<glm::dvec3> displacement; // normalized (max |u| = 1)
    double factor = 0.0;
  };
  std::vector<ModeShape> modes;

  // A thermal result (docs/simulation/README.md#thermal): the temperature
  // at each node (degrees C), and the heat flux -k grad T (W/m^2), averaged
  // to the node by element volume as the stress is. A static result under a
  // thermal load keeps its temperature too. Empty on any other.
  std::vector<double> temperature;
  std::vector<glm::dvec3> heatFlux;
};

// How a face moved, as a rigid body: the small-displacement fit of its
// boundary nodes' displacements, u(p) ~ translation + rotation x (p -
// centroid), each node weighted by the area it stands for. A bearing seat
// moves its bearing as a body, so this -- not the largest displacement
// anywhere -- is what carries a deflection on to whatever the seat holds
// (the lens of a camera on a shaft in two bores).
// docs/simulation/README.md#probing-a-face
struct FaceMotion {
  int nodes = 0;
  double area = 0.0;               // mm^2, of the boundary triangles
  glm::dvec3 centroid{0.0};        // mm, undeformed, area-weighted
  glm::dvec3 translation{0.0};     // mm, the centroid's displacement
  glm::dvec3 rotation{0.0};        // radians, small-angle rotation vector
  bool rotationDetermined = false; // false when the nodes lie on a line
  double rmsResidual = 0.0;        // mm, what the rigid fit leaves
  // The fitted motion carried to point `p`.
  glm::dvec3 At(const glm::dvec3& p) const {
    return translation + glm::cross(rotation, p - centroid);
  }
};
// nullopt when no boundary triangle lies on face `faceIndex` or
// `displacement` is not one entry per mesh node.
std::optional<FaceMotion>
FitFaceMotion(const FemMesh& mesh, const std::vector<glm::dvec3>& displacement,
              int faceIndex);

// The von Mises stress on one face of a solved mesh
// (docs/simulation/README.md#probing-a-face): the largest nodal value over
// the face's boundary triangles and where it is, and the area-weighted mean
// of the field the triangles interpolate. A face is where a designer reads a
// stress -- a tooth root, a fillet -- rather than the global peak, which in a
// sharp re-entrant corner grows with refinement. nullopt when no boundary
// triangle lies on face `faceIndex` or `vonMises` is not one entry per node.
struct FaceStress {
  int nodes = 0;
  double area = 0.0;     // mm^2
  double maxMPa = 0.0;   // the largest nodal value on the face
  glm::dvec3 maxAt{0.0}; // mm, where it is
  double meanMPa = 0.0;  // area-weighted over the face
};
// Any per-node scalar field reads the same way (a principal stress, the
// stress across layers), so `vonMises` is that field.
std::optional<FaceStress> FaceStressOf(const FemMesh& mesh,
                                       const std::vector<double>& vonMises,
                                       int faceIndex);

// The largest principal stress of `s` (MPa, tension positive): the
// eigenvalue of the stress tensor that a brittle material breaks on.
double MaxPrincipal(const Stress& s);
// The normal stress on the plane whose normal is `n` (need not be unit):
// n . sigma . n, tension positive. For a printed part with `n` its build
// direction, the stress pulling its layers apart.
double NormalStress(const Stress& s, const glm::dvec3& n);
// A field of one value per node of `r.stress`: its largest principal
// stress, or (AcrossLayers) its normal stress along `n`. Empty when the
// result kept no stress tensor.
std::vector<double> MaxPrincipalField(const FemResults& r);
std::vector<double> AcrossLayersField(const FemResults& r, const glm::dvec3& n);
// The largest value of a per-node field and where it is; node -1 when the
// field is empty or not one per node of `mesh`.
struct FieldPeak {
  double value = 0.0;
  int node = -1;
  glm::dvec3 at{0.0};
};
FieldPeak PeakOf(const FemMesh& mesh, const std::vector<double>& field);

// Solves K u = f with the given fixed nodes (all three DOF zeroed, eliminated
// from the system), face tractions, and an optional body force (gravity
// acceleration in mm/s^2; needs material.densityTPerMM3). Returns std::nullopt
// and sets *error on a singular system or when built without Eigen.
std::optional<FemResults>
Solve(const FemMesh& mesh, const Material& material,
      const std::vector<int>& fixedNodes, const std::vector<FaceLoad>& loads,
      std::string* error, const glm::dvec3& gravityAccel = glm::dvec3(0, 0, 0));
// The same over a mesh of several glued solids (S2): one material per
// domain of FemMesh::tetDomain. A mesh with no domain record takes the
// first; a domain beyond the list takes the last.
std::optional<FemResults>
Solve(const FemMesh& mesh, const std::vector<Material>& byDomain,
      const std::vector<int>& fixedNodes, const std::vector<FaceLoad>& loads,
      std::string* error, const glm::dvec3& gravityAccel = glm::dvec3(0, 0, 0));

// A node held in its own frame: local direction k is `frame[k]` (the three
// orthonormal), held at zero when `held[k]`. A pin holds its face's nodes
// radially and axially and leaves them free to slide round the axis.
struct HeldNode {
  int node = -1;
  std::array<glm::dvec3, 3> frame{glm::dvec3(1, 0, 0), glm::dvec3(0, 1, 0),
                                  glm::dvec3(0, 0, 1)};
  std::array<bool, 3> held{false, false, false};
};
// What holds a mesh: whole nodes fixed in all three directions, and nodes
// held in some directions of their own frames (a node in both is fixed).
// `reactions` asks the solve for FemResults::nodeReaction.
// A tie: sum_i coef_i u_i = 0 over local directions of held nodes (each
// entry a node of `held`, its frame's direction k). A pin held from turning
// ties its face's tangential displacements, so the face as a whole does not
// turn about the axis while each node may still slide.
struct Tie {
  struct Entry {
    int node = -1;
    int k = 0;
    double coef = 0.0;
  };
  std::vector<Entry> entries;
};
struct Supports {
  std::vector<int> fixedNodes;
  std::vector<HeldNode> held;
  std::vector<Tie> ties;
  bool reactions = false;
};

// Several load cases on one stiffness (docs/topopt/README.md#load-cases): K
// is assembled and factorised once, and each case is one right-hand side —
// its face loads plus the gravity every case shares. One result per case, in
// order; a failure in any case fails the whole. Above
// `options.iterativeAboveDofs` free DOF K is not factorised: each case is
// solved by preconditioned conjugate gradients, to the same residual the
// direct solve is held to (docs/simulation/README.md#the-solver).
std::optional<std::vector<FemResults>>
SolveCases(const FemMesh& mesh, const std::vector<Material>& byDomain,
           const std::vector<int>& fixedNodes,
           const std::vector<std::vector<FaceLoad>>& cases, std::string* error,
           const glm::dvec3& gravityAccel = glm::dvec3(0, 0, 0),
           const SolveOptions& options = {}, SolveReport* report = nullptr);
// The same with supports beyond fixed nodes. A held node's DOF are taken in
// its frame (K' = T^T K T), so a held direction is eliminated exactly as a
// fixed DOF is; a tie eliminates its largest entry's DOF in terms of the
// others. An external (GPU) solver declines a system with held nodes:
// it applies each element matrix in x, y, z.
std::optional<std::vector<FemResults>>
SolveCases(const FemMesh& mesh, const std::vector<Material>& byDomain,
           const Supports& supports,
           const std::vector<std::vector<FaceLoad>>& cases, std::string* error,
           const glm::dvec3& gravityAccel = glm::dvec3(0, 0, 0),
           const SolveOptions& options = {}, SolveReport* report = nullptr);

// Whether constraints leave a rigid motion free: each held direction `d` at
// a point `x` is the row [d, x x d] against the six rigid motions (a tie the
// sum of its entries' rows), and a rank under six is a mechanism. Empty when
// none is free; otherwise the free motion, in words ("turn about an axis along
// (0, 0, 1) through (10, 0, 3)" or "slide along (1, 0, 0)").
std::string FreeRigidMotion(const FemMesh& mesh, const Supports& supports,
                            glm::dvec3* axisDir = nullptr,
                            glm::dvec3* axisPoint = nullptr,
                            bool* turns = nullptr);

// The support-edge flag of a solved field: `supportTris` are each support's
// boundary triangles (indices into mesh.boundary) with its id and type.
struct SupportFacets {
  uint64_t id = 0;
  LoadType type = LoadType::Fixed;
  std::vector<int> tris;
  // How far from its edge a node still counts as ON it, mm (SupportAwayMm);
  // 0 = one element.
  double awayMm = 0.0;
};
FemResults::SupportEdgePeak
FindSupportEdgePeak(const FemMesh& mesh, const FemResults& results,
                    const std::vector<SupportFacets>& supports);

// How far a support's edge singularity reaches, in mm, derived from the
// support (docs/simulation/README.md#a-peak-on-a-supports-edge): a pin
// (`axis` its axis, origin and unit direction) a quarter of its bore's
// radius; a fixed face a quarter of its facets' largest extent.
double SupportAwayMm(const FemMesh& mesh, const std::vector<int>& tris,
                     LoadType type, const glm::dvec3& axisOrigin,
                     const glm::dvec3& axisDirection);
// Per node, the index (into `supports`, which is also the result's
// `reactions` order) of the support whose edge it lies near, else -1: within
// the support's awayMm of an edge node, and never less than one element (a
// node of a tet that has an edge node). Needs the tets; a solved mesh has
// them.
std::vector<int> SupportEdgeZones(const FemMesh& mesh,
                                  const std::vector<SupportFacets>& supports);
// A field's peak against those zones: flagged when the node `peakNode`
// lies in one, with the support (`supports[zone]`, a result's reactions) and
// the field's largest value outside every zone. Unflagged when the zones are
// empty or do not match the field.
FemResults::SupportEdgePeak
PeakAwayFromEdges(const FemMesh& mesh, const std::vector<double>& field,
                  int peakNode, const std::vector<int>& zones,
                  const std::vector<FemResults::SupportReaction>& supports);

// Solves the generalized eigenproblem K phi = omega^2 M phi for the lowest
// `numModes` natural frequencies + mode shapes (consistent mass matrix). Needs
// at least one fixed support (v1) and material.densityTPerMM3 > 0. Returns
// std::nullopt and sets *error on failure or when built without Spectra.
std::optional<std::vector<FemResults::ModeShape>>
SolveModal(const FemMesh& mesh, const Material& material,
           const std::vector<int>& fixedNodes, int numModes,
           std::string* error);
std::optional<std::vector<FemResults::ModeShape>>
SolveModal(const FemMesh& mesh, const std::vector<Material>& byDomain,
           const std::vector<int>& fixedNodes, int numModes,
           std::string* error);

// Linear buckling (docs/simulation/README.md#buckling) about the base state
// `baseDisplacement` (the static answer under `supports`, one per node):
// the geometric stiffness K_G is assembled from its stress at each tet10's
// four Gauss points, and -K_G phi = mu K phi solved for the largest |mu|
// (K is factored by Cholesky; K_G may be indefinite), lambda = 1 / mu. Up
// to `numFactors` modes, ascending by |factor|, each with its signed factor
// and its shape normalized to max |u| = 1. When every one found is negative
// the smallest positive one is looked for on its own. Empty when the load
// has no positive factor (nothing in it buckles) -- or only negative ones.
std::optional<std::vector<FemResults::ModeShape>>
SolveBuckling(const FemMesh& mesh, const std::vector<Material>& byDomain,
              const Supports& supports,
              const std::vector<glm::dvec3>& baseDisplacement, int numFactors,
              std::string* error);
// The memory a buckling solve of `freeDofs` free DOF is estimated to take,
// bytes: K, K_G and the Cholesky factor of K, which fills in steeply.
// Calibrated from measured runs: the comment at its definition says which.
uint64_t BucklingMemoryEstimate(size_t freeDofs);

// The fields of a large-deformation answer `displacement` (one per node):
// the Cauchy stress at each node, from the Green-Lagrange strain through
// the material's St Venant-Kirchhoff law -- what CalculiX's linear elastic
// material is under NLGEOM -- pushed forward to the deformed part, and
// averaged to the node by element volume as the linear recovery is; its von
// Mises, and the maxima. No reactions.
// Under a thermal load (`thermal`, null: none) the strain the stress is
// read from is less the thermal strain alpha (T - T0) I at each node.
FemResults LargeDeformationFields(const FemMesh& mesh,
                                  const std::vector<Material>& byDomain,
                                  const std::vector<glm::dvec3>& displacement,
                                  const NodalTemperature* thermal = nullptr);
// The same for a small-deformation answer: the linear strain and stress
// SolveCases recovers, averaged to the node by element volume. A contact
// solved without large deformation (docs/simulation/README.md#contact) is
// read through this.
FemResults SmallDeformationFields(const FemMesh& mesh,
                                  const std::vector<Material>& byDomain,
                                  const std::vector<glm::dvec3>& displacement,
                                  const NodalTemperature* thermal = nullptr);

// The fields of an answer whose stress was not computed from `displacement`
// but given at each tet's four Gauss points (kTetGauss's, point k nearest
// corner k, in the tet's own corner order): a plastic run's, whose stress
// depends on the path. Each tet's points are fitted with the linear field
// through them, which is exact for a tet10's elastic stress, read at its
// corners (its midsides the mean of their two), and averaged to the node
// by element volume as the linear recovery is. `ipScalar` (empty: none) is
// read the same way into plasticStrain, not below 0. Von Mises and the
// maxima follow.
FemResults
IntegrationPointFields(const FemMesh& mesh,
                       const std::vector<glm::dvec3>& displacement,
                       const std::vector<std::array<Stress, 4>>& ipStress,
                       const std::vector<std::array<double, 4>>& ipScalar);

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

// A static run's convergence check (docs/simulation/README.md
// #a-peak-that-has-settled). Step 0 is the first solve; each later step
// halved the element size on the faces its governing peaks lie on, re-meshed
// and re-solved. A governing peak is von Mises and, for a printed material,
// the stress across the layers (along the run's layer normal; without one,
// the largest principal stress, which bounds it in every direction); a peak
// on a support's edge is read as its away value.
struct ConvergencePeak {
  std::string body;
  std::string quantity; // von_mises | across_layers | max_principal
  bool away = false;    // the value away from a support's edge
  double mpa = 0.0;
};
struct ConvergenceRefinement {
  std::string body;
  int face = -1; // the body's face index (TopExp_Explorer order)
  double sizeMm = 0.0;
};
struct ConvergenceStep {
  std::vector<ConvergenceRefinement> refined; // empty on step 0
  std::size_t nodes = 0;                      // over every body
  double seconds = 0.0;                       // meshing and solving
  std::vector<ConvergencePeak> peaks;
};
struct Convergence {
  // false: the loop did not run, and `why` says why ("not asked" for
  // converge: never, a modal study, a shape study, ...).
  bool checked = false;
  std::string why;
  bool converged = false;
  // Why it ended unconverged: "time" (the next step would run past
  // converge_auto_s; `stoppedSeconds` is the step that set that), "steps"
  // (four refinements and still moving), "failed" (a step did not mesh or
  // solve; `why` says what, and the result is the last step that did).
  std::string stopped;
  double stoppedSeconds = 0.0;
  double lastChangePct = 0.0; // the largest peak's change over the last step
  // The direction the across-layers peak was read along, when one was given.
  bool hasLayerNormal = false;
  glm::dvec3 layerNormal{0.0};
  std::vector<ConvergenceStep> steps;
};

// One body of a study: its mesh and solved fields (all derived/transient).
// For a topology study `results` is empty and `density` carries the optimized
// per-tet density (parallel to mesh.tets); for static/modal `density` is empty.
struct StudyBodyResult {
  int bodyIndex = -1;
  std::string bodyName;
  FemMesh mesh;
  FemResults results;
  std::vector<double> density; // topology only: one entry per mesh.tets
  // A static study with load cases: one result per case, in the study's
  // order; `results` is a copy of the first. Empty without cases.
  std::vector<FemResults> cases;
  // A static body: per node, the support (an index into `edgeOwners`, its
  // supports in the study's order with their load id and type -- only those
  // two filled) whose edge the node lies near, else -1 (SupportEdgeZones),
  // so any field read later -- the stress across layers along a direction
  // given then -- is flagged the way von Mises was. Empty on a result saved
  // before v206.
  std::vector<int> supportEdgeZone;
  std::vector<FemResults::SupportReaction> edgeOwners;
  // A buckling body (docs/simulation/README.md#buckling): `results` is the
  // base state and its modes the buckling modes; this is what was read off
  // them.
  struct Buckling {
    bool solved = false;
    // The smallest positive factor; none (`buckles` false, `none` the
    // reason) when nothing in the load buckles.
    bool buckles = false;
    double factor = 0.0;
    std::string none;
    // The imperfection knockdown (NASA SP-8007) of the face the first
    // mode's peak motion lies on: its class (flat, member, cylinder, cone,
    // sphere), radius and wall there; 1 on a flat face or a member.
    std::string shapeClass;
    double knockdown = 1.0;
    int face = -1;
    double radiusMm = 0.0;
    double wallMm = 0.0;
    // yield / peak von Mises, and interlayer strength / peak tension (the
    // largest principal stress, which bounds the stress across the layers
    // in every direction) of the base state; 0 = no such strength.
    double yieldFactor = 0.0;
    double interlayerFactor = 0.0;
    // 1/governing = 1/(knockdown x factor) + 1/min(yield, interlayer).
    double governingFactor = 0.0;
    // The first mode's peak motion lies at a support's or load's edge, and
    // the mode is local there: crippling where the load enters.
    bool atLoadEdge = false;
    uint64_t edgeLoad = 0;
    std::string edgeType;
    int edgeFace = -1;
    // Elements through the wall the first mode bends, where its motion
    // peaks (round(wall / element size)); 0 = not measured. One is a
    // warning, not a refinement: measured on a 1 mm plate, one element
    // through it reads within 1.3 % of two.
    int wallLayers = 0;
  };
  Buckling buckling;
  // A nonlinear static body (docs/simulation/README.md#nonlinear-static):
  // `results` is the state at the factor reported (1 on a large-deformation
  // run, the peak on a snap-through one), its stress the Cauchy stress of
  // the deformed part; this is how the run went.
  struct Nonlinear {
    bool solved = false;
    bool snapThrough = false;
    // The factor the fields are at, and the load the run was asked to reach
    // (1, or a snap-through run's cap: twice the linear factor).
    double factor = 0.0;
    double maxFactor = 1.0;
    int increments = 0; // converged increments of the run reported
    int ccxRuns = 0;    // a snap's peak is bracketed by a second run
    // The path: the factor and the largest motion (mm) at each converged
    // increment.
    std::vector<std::pair<double, double>> curve;
    // Snap-through: the first linear factor and the seed, the motion limit,
    // and how the run ended -- limit_point (the solve stopped converging
    // as the stiffness ran out), snap (a converged increment jumped to
    // another branch), deflection_limit, or none (it reached the cap). The
    // peak lies in [peakLow, peakHigh]; `peakFactor` is the low end (the
    // deflection limit's: interpolated between the increments either side).
    double linearFactor = 0.0;
    double imperfectionMm = 0.0;
    bool imperfectionGiven = false;
    double deflectionLimitMm = 0.0;
    std::string peakAt;
    double peakFactor = 0.0;
    double peakLow = 0.0, peakHigh = 0.0;
    std::string none; // why there is no peak (peakAt none)
    // The factor at which the peak von Mises reached yield (0 = not reached
    // by the factor reported, or no yield strength).
    double firstYieldFactor = 0.0;
    // A large-deformation run that jumped between two factors on its way
    // to the load: the part snapped through, and the fields are of the
    // branch it landed on.
    bool snapped = false;
    double snappedFrom = 0.0, snappedTo = 0.0;
    // The equilibrium check: ccx's residual criterion (largest residual
    // over the mean nodal force), and the supports' total reaction against
    // the applied load at the factor reported, relative to the load.
    double residualCriterion = 0.0;
    double imbalance = 0.0;
    // A contact run (docs/simulation/README.md#contact): whether it was
    // solved under large deformation, and what the weak springs that hold
    // a part with no support of its own while its contact closes carried at
    // the factor reported, over the load (0 = none were needed).
    bool largeDeformation = true;
    double stabilization = 0.0;
    // A plastic run (docs/simulation/README.md#plasticity). `collapsed`:
    // the solve stopped converging as the part's stiffness ran out before
    // the load, at `factor`, the collapse load in [factor, collapseHigh].
    // `unloaded`: the load was taken off again, and the result's residual
    // displacement is what it left. `strainLimit` is where the hardening
    // data ends (the smallest last strain of the body's curves), past which
    // CalculiX holds the stress at the last point.
    bool plasticity = false;
    bool collapsed = false;
    double collapseHigh = 0.0;
    bool unloaded = false;
    double strainLimit = 0.0;
  };
  Nonlinear nonlinear;
  // Each contact of the body (docs/simulation/README.md#contact): a contact
  // support or a contact participant, at the factor reported. `areaMm2` is
  // the area of the pair's faces on the body's side, `closedMm2` the part of
  // it in contact; the force is what the partner exerts on that side (N,
  // the frame's), split into its normal and shear parts; pressures in MPa.
  struct ContactResult {
    uint64_t id = 0; // the support's load id, or the participant's occurrence
    bool participant = false;
    std::string name; // "ground", or the partner occurrence's name
    double friction = 0.0;
    double penaltyMPaPerMm = 0.0;
    double areaMm2 = 0.0;
    double closedMm2 = 0.0;
    glm::dvec3 forceN{0.0};
    glm::dvec3 momentNmm{0.0}; // about the frame's origin
    double normalN = 0.0;
    double shearN = 0.0;
    double maxPressureMPa = 0.0;
    double meanPressureMPa = 0.0; // normal force over the closed area
  };
  std::vector<ContactResult> contacts;
  // A thermal body (docs/simulation/README.md#thermal): what was read off
  // its field. A static body under a thermal load fills the field range,
  // its stress-free temperature and its source, and the service limit.
  struct Thermal {
    bool solved = false; // a thermal study's body
    bool load = false;   // a static body under a thermal load
    std::vector<HeatFlow> flows;
    double inW = 0.0, outW = 0.0, imbalanceW = 0.0;
    double maxC = 0.0, minC = 0.0;
    glm::dvec3 maxAt{0.0}, minAt{0.0};
    double maxFluxWm2 = 0.0;
    glm::dvec3 maxFluxAt{0.0};
    // The lowest held or ambient temperature: what the peak's rise, which
    // the converge loop settles, is over.
    double baseC = 0.0;
    // The material's service temperature and what it is; `hasService`
    // false where it has none. margin = serviceMaxC - maxC.
    bool hasService = false;
    double serviceMaxC = 0.0;
    std::string serviceBasis;
    double marginK = 0.0;
    // The conductivity is a bulk resin's on a printed material.
    bool bulkConductivity = false;
    // A static body's thermal load.
    double stressFreeC = 0.0;
    uint64_t fromStudy = 0;
  };
  Thermal thermal;
  // The results of case `k` (the only ones when there are no cases).
  const FemResults& Case(int k) const {
    return k >= 0 && k < static_cast<int>(cases.size()) ? cases[k] : results;
  }
};

// What the connectivity filter took out of a topology result
// (docs/topopt/README.md#connectivity): the components with no support and no
// load at the volume-matched threshold, their tets and volume, the keep faces
// (indices into Study::keepFaces) they held, and whether a load sits in a
// component no support reaches.
struct TopoIslands {
  double threshold = 0.0;
  int components = 0;
  int tets = 0;
  double volume = 0.0; // mm^3
  std::vector<int> keepFaces;
  bool loadPathBroken = false;
};

// What one keep-out took from the design domain.
struct KeepOutReport {
  std::string what; // "body 'worm block'" or "box [..]-[..]"
  int tets = 0;
  double volume = 0.0; // mm^3
};

// A study's derived mesh + results, cached on the Document. Stale when
// solvedAtVersion != Document::Version(). Maxima are aggregated across all
// solved bodies for the summary an engineer reads.
//
// Every runtime is saved with the project beside its study, with the input
// hash that produced it, and reloaded into this same cache slot
// (DocumentIO::StudyResultToJson; topology since v33, static and modal v153):
// a topology result is minutes of compute over a mesh that is not
// reproducible across Netgen versions, and a static one of a big part is
// minutes too.
struct StudyRuntime {
  uint64_t solvedAtVersion = 0;
  // The input hash of what produced it (fem::StudyInputKey,
  // docs/simulation/README.md#studies-as-jobs); 0 = unknown, never matched.
  // Saved with the result, so a reopened project can reuse it.
  uint64_t inputHash = 0;
  // Assembly studies: the version of every PART the solve read, beside the
  // assembly's own above. Stale when any of them moved — a panel regenerated
  // behind a fresh-looking result is the stale drawing's lie. Empty on a
  // part study.
  std::vector<std::pair<uint64_t, uint64_t>> solvedPartVersions;
  std::vector<StudyBodyResult> bodies;
  double maxVonMises = 0.0;
  glm::dvec3 maxVmAt{0, 0, 0};
  std::string maxVmBody;
  double maxDisplacement = 0.0;
  glm::dvec3 maxDispAt{0, 0, 0};
  std::string maxDispBody;

  // Topology summary (empty/zero for static & modal runtimes). `topology` is
  // the flag that this runtime is a persisted generative result.
  bool topology = false;
  int iterations = 0;
  bool converged = false;
  bool cancelled = false;
  double volumeFractionAchieved = 0.0;
  std::vector<double> complianceHistory;

  // A static study's convergence check (docs/simulation/README.md
  // #a-peak-that-has-settled): the refinements the run added round its
  // governing peaks and what each step read. Saved with the result.
  Convergence convergence;

  // Provenance of the solve — transient (NOT serialized; a reloaded result
  // reads empty/zero): which linear solver ran ("gpu: <device>" /
  // "cpu (...)") and the wall time of the whole optimization. The one
  // exception is a CalculiX answer's "calculix 2.NN", which is saved: its
  // receipt names that oracle after a reopen.
  std::string solver;
  double solveSeconds = 0.0;
  // A CalculiX run's check against our K (docs/simulation/README.md
  // #calculix): the worst |K u - f| / |f| over bodies and cases, and the
  // part of it the answer's printed precision alone accounts for. Transient
  // like `solver`; 0 for an in-house run.
  double residual = 0.0;
  double residualFloor = 0.0;
  double energyResidual = 0.0; // |u . (K u - f)| / |u . f|

  // Load cases (static and topology), in the study's order; empty without.
  std::vector<std::string> caseNames;
  // Topology: each case's weight and compliance at the last iteration (the
  // objective is their weighted sum, `complianceHistory`).
  std::vector<double> caseWeights;
  std::vector<double> caseCompliance;
  TopoIslands islands;
  std::vector<KeepOutReport> keepOuts;
  // Topology: a digest of the mesh and density, which a static study of the
  // shape (ShapeSource) records as `sourceDigest`; it reads stale once the
  // two differ. Computed when the result is made or loaded, never saved.
  uint64_t densityDigest = 0;
  uint64_t sourceDigest = 0;
  // A shape study: the tets the solid kept, and what was dropped as not
  // connected to a support at its threshold.
  double shapeThreshold = 0.0;
  int shapeIslandTets = 0;
  double shapeVolume = 0.0; // mm^3, of the solid that was solved
  // When the volume-matched level left a loaded face unconnected to a
  // support, that level; `shapeThreshold` is then the highest one that
  // carries every load (0 = not lowered).
  double shapeThresholdLoweredFrom = 0.0;
};

// Reduces a solved runtime to what is read back from it
// (docs/simulation/README.md#what-a-saved-result-keeps), which is also what
// the project saves: every stored result passes through here
// (Document::StoreStudyResults, AssemblyDocument::StoreStudyResults), so a
// result reopened from disk equals the one the run left in memory.
//
// - Fields are float, as derived bulk data is (docs/conventions.md
//   #the-model-is-double): node positions, displacements, stresses, mode
//   shapes and densities are rounded to float and held in the double fields.
// - A static or modal body keeps the boundary surface: its triangles, the
//   nodes they use (and the peak nodes), and those nodes' fields, renumbered
//   in their old order. Every panel and RPC reads a static result there --
//   the contour, the deformed shape, a face's motion, the peak markers;
//   nothing reads the interior back. The tets go; `mesh.stats` keeps the
//   solved mesh's counts.
// - A topology body keeps its whole mesh: the density is per tet, and the
//   iso-surface, the STL export and a static study of the shape cut tets.
// - A domain record that names one domain is dropped (empty means one).
// Idempotent: a reduced runtime passes through unchanged.
void KeepWhatIsRead(StudyRuntime& rt);

// (Document is forward-declared at global scope by FemStudy.h.)

// Meshes and solves the study across every body it loads: evaluates the
// expressions, resolves refs, validates (material has stiffness data, each
// loaded body has a fixed support), then meshes and solves. On any failure
// sets study.error and *error and returns false. Fills `rt` (not stamped —
// the caller stamps solvedAtVersion and stores it in the Document cache).
bool RunStudy(const Document& doc, Study& study, StudyRuntime& rt,
              std::string* error);
// The same under explicit convergence options (the study's `converge` is
// what the plain form uses). ConvergeOptions is declared below.
struct ConvergeOptions;
bool RunStudy(const Document& doc, Study& study, StudyRuntime& rt,
              const ConvergeOptions& converge, std::string* error);

// RunStudy, split at the thread boundary (the TopoInput pattern): everything
// Document-bound — expression evaluation, face resolution, validation, and
// meshing — happens in PrepareStudy on the main thread; SolvePreparedStudy is
// pure fem data and safe on a worker. RunStudy == Prepare + SolvePrepared.
struct StudyJobBody {
  int bodyIndex = 0;
  std::string bodyName;
  FemMesh mesh;
  Material mat;
  // A bonded group (S2): one material per mesh domain, derived by
  // MeshPlannedStudy from the plan's members. Empty = `mat` throughout.
  std::vector<Material> materials;
  std::vector<int> fixedNodes;
  // Every support's facets (fixed and pin), for the reactions and the
  // support-edge flag, and each pin's axis (docs/simulation/README.md
  // #pin-supports).
  std::vector<SupportFacets> supports;
  struct PinSupport {
    uint64_t id = 0;
    std::vector<int> tris; // indices into mesh.boundary
    glm::dvec3 axisOrigin{0.0};
    glm::dvec3 axisDirection{0.0, 0.0, 1.0};
    bool fixRotation = false;
  };
  std::vector<PinSupport> pins;
  // Contact (docs/simulation/README.md#contact). Each pair is the body's
  // facets on one side (`slaveTris`) against a partner's: a rigid one's
  // (`partner`, an index into `rigidPartners`, whose every node is fixed;
  // `masterTris` index its boundary) or a contact participant's, meshed
  // into this mesh as domains of its own (`partner` -1; `masterTris` index
  // mesh.boundary). `penalty` is the pressure per overclosure (MPa/mm).
  struct ContactPair {
    uint64_t id = 0;
    bool participant = false;
    std::string name;
    std::vector<int> slaveTris;
    std::vector<int> masterTris;
    int partner = -1;
    double friction = 0.0;
    double penalty = 0.0;
  };
  std::vector<ContactPair> contacts;
  std::vector<FemMesh> rigidPartners;
  // A part of the mesh with no support of its own -- held only by a
  // contact -- is held while its contact closes by weak springs to the
  // ground at every node, `kPerNode` (N/mm) in each direction.
  struct Stabilized {
    std::vector<int> nodes;
    double kPerNode = 0.0;
  };
  std::vector<Stabilized> stabilized;
  std::vector<FaceLoad> faceLoads;
  // With load cases: each case's face loads (`faceLoads` is then empty).
  std::vector<std::vector<FaceLoad>> caseLoads;
  // A buckling study: each curved face of the meshed shape (its index),
  // for the knockdown of the face a mode peaks on. Read off the shape where
  // it is meshed.
  struct CurvedFace {
    std::string kind;       // cylinder, cone, sphere
    glm::dvec3 origin{0.0}; // a point on the axis, or the sphere's centre
    glm::dvec3 axis{0.0, 0.0, 1.0};
    double radius = 0.0;    // cylinder, sphere
    double semiAngle = 0.0; // cone, radians
  };
  std::map<int, CurvedFace> curvedFaces;
  // A thermal study's loads; on a static study under a from_study thermal
  // load, the thermal study's loads on this mesh, solved first for the
  // temperature the static solve takes.
  HeatLoads heat;
  // The static study's thermal load, on this body (Kind None: none).
  ThermalLoad thermalLoad;
  double baseC = 0.0; // the lowest held or ambient temperature of `heat`
};
struct StudyJobInput {
  AnalysisType type = AnalysisType::Static;
  Solver solver = Solver::Auto;
  Nonlinear nonlinear;
  ThermalLoad thermalLoad;
  std::vector<std::string> caseNames;
  int numModes = 8;
  glm::dvec3 gravity{0.0};
  std::vector<StudyJobBody> bodies;
  // Carried from the plan of a shape study into its runtime.
  uint64_t sourceDigest = 0;
  double shapeThreshold = 0.0;
  int shapeIslandTets = 0;
  double shapeVolume = 0.0;
  double shapeThresholdLoweredFrom = 0.0;
};
bool PrepareStudy(const Document& doc, Study& study, StudyJobInput& out,
                  std::string* error);

// PrepareStudy split one step further, at the meshing
// (docs/ui/long-operations.md#fem-meshing-and-solving): PlanStudy keeps
// everything that needs the Document or the material library on the main thread
// and hands over deep copies of the shapes plus plain loads; MeshPlannedStudy
// meshes them and derives the fixed nodes and face tractions from the mesh it
// made, and is safe on a worker. PrepareStudy == PlanStudy + MeshPlannedStudy.
struct StudyBodyPlan {
  int bodyIndex = 0;
  std::string bodyName;
  TopoDS_Shape shape; // a copy, owned here
  MeshSettings settings;
  Material mat;
  std::vector<std::pair<int, Load>> loads; // (face index, the load)
  // A static study under a from_study thermal load: the thermal study's
  // loads on this body, meshed with it (face -1: a heat_generation, through
  // the body or, in a bonded group, the member `Load::body` names).
  std::vector<std::pair<int, Load>> heatLoads;
  // A bonded group (S2): `shape` is then a compound of these imprinted
  // solids, which share the faces where they touch so one mesh spans them,
  // and each carries its own material. Empty for a single solid.
  struct Member {
    TopoDS_Shape solid;
    Material mat;
    std::string name;
  };
  std::vector<Member> members;
  // A static study of a topology result (ShapeSource): the mesh to solve,
  // already cut from the result — nothing is meshed. `shape` is then null.
  std::shared_ptr<const FemMesh> premeshed;
  // The material's strength across printed layers (MPa; 0 = not a printed
  // material): a convergence check then also settles the stress across the
  // layers.
  double interlayerMPa = 0.0;
  // Contact (docs/simulation/README.md#contact). A partner is a rigid body
  // (the ground under a flat face, or a rigid occurrence's body) meshed
  // coarse with every node fixed, or a contact participant meshed into this
  // body's mesh with its own material, its faces numbered from
  // `faceOffset` (its loads are in `loads` under those numbers). `faces`
  // are the partner's own faces the pair holds.
  struct ContactPartner {
    bool rigid = true;
    std::string name;
    TopoDS_Shape shape; // a copy, owned here
    MeshSettings settings;
    std::vector<int> faces;
    Material mat;
    int faceOffset = 0;
  };
  std::vector<ContactPartner> partners;
  struct Contact {
    uint64_t id = 0; // the support's load id, or the participant occurrence
    bool participant = false;
    std::vector<int> faces; // this body's faces
    int partner = 0;        // index into `partners`
    double friction = 0.0;
  };
  std::vector<Contact> contacts;
};
// Where a meshed body touches a rigid support, as the imprint found it: the
// faces of the meshed body's imprinted shape that it shares with the rigid
// body (indices into the shape MeshPlannedStudy meshes), and the rigid
// body's ORIGINAL faces that carry the patch, which is what a picture can
// tint. Assembly studies only.
struct ContactPatch {
  uint64_t rigidOccurrenceId = 0;
  std::string rigidName;
  int meshedBody = 0; // index into StudyPlan::bodies
  std::vector<int> meshedFaces;
  int rigidBody = 0; // index into the rigid occurrence's placed bodies
  std::vector<int> rigidFaces;
};
// Where two bonded members share a face (S2): the ORIGINAL faces on each
// side that carry the seam, for the picture.
struct BondPatch {
  uint64_t occurrenceA = 0, occurrenceB = 0;
  int bodyA = 0, bodyB = 0; // indices into each occurrence's placed bodies
  std::vector<int> facesA, facesB;
};
struct StudyPlan {
  AnalysisType type = AnalysisType::Static;
  Solver solver = Solver::Auto; // EffectiveSolver: a nonlinear auto is calculix
  Nonlinear nonlinear;
  ThermalLoad thermalLoad;
  std::vector<std::string> caseNames; // empty = one case
  // A shape study (ShapeSource): what the runtime reports of the cut.
  uint64_t sourceDigest = 0;
  double shapeThreshold = 0.0;
  int shapeIslandTets = 0;
  double shapeVolume = 0.0;
  double shapeThresholdLoweredFrom = 0.0;
  int numModes = 8;
  glm::dvec3 gravity{0.0};
  std::vector<StudyBodyPlan> bodies;
  std::vector<ContactPatch> contacts;
  std::vector<BondPatch> bonds;
};
bool PlanStudy(const Document& doc, Study& study, StudyPlan& out,
               std::string* error);
// The ground a flat `face` rests on (docs/simulation/README.md#contact): a
// rigid box on the face's outer side whose face against the part lies on
// the face's plane, past the face on every side. Refused for a face that is
// not flat, naming the alternative.
bool GroundUnder(const TopoDS_Shape& face, StudyBodyPlan::ContactPartner& out,
                 std::string* error);
bool MeshPlannedStudy(const StudyPlan& plan, StudyJobInput& out,
                      std::string* error);

// PlanStudy for a study on an ASSEMBLY
// (docs/simulation/README.md#assembly-studies): one occurrence meshed, the
// rigid ones clamped where they touch it. The caller supplies the placed
// geometry, because the assembly document does not own its parts:
// `placedBodies` returns an occurrence's bodies in WORLD space (reference
// bodies excluded), element maps intact. What it does, in order: refuses more
// or fewer than one meshed occurrence; refuses a rigid support that
// INTERPENETRATES the meshed part (a press fit fuses into slivers); imprints
// every rigid body onto the meshed body with a fuzzy general fuse (parts sit
// microns apart at float32) WITHOUT touching either document; maps each load's
// face onto the imprinted shape (a face the imprint split becomes several, a
// force's total spread by area); and turns each contact anchor into the shared
// faces, refusing one whose support does not touch. The result meshes and
// solves through MeshPlannedStudy / SolvePreparedStudy unchanged, in world
// coordinates.
struct AssemblyStudyContext {
  std::function<std::vector<Body>(uint64_t occurrenceId)> placedBodies;
  const SymbolTable* symbols = nullptr; // the project's variables; may be null
};
bool PlanAssemblyStudy(const AssemblyDocument& asmDoc, Study& study,
                       const AssemblyStudyContext& ctx, StudyPlan& out,
                       std::string* error);

// The input hash of a run of `study` (docs/simulation/README.md
// #studies-as-jobs): the study with its expressions evaluated (its name and
// id aside), and the bodies it reads -- their exact B-Rep, names and
// materials with the library's numbers. On an assembly, each taking part
// occurrence as saved and its placed bodies. 0 when an expression does not
// evaluate (the run would refuse; nothing is reused for it). The solver a
// topology run picks (GPU or CPU) is not an input: both answer the same
// problem.
uint64_t StudyInputKey(const Document& doc, const Study& study);
// Every stored result of `doc` whose input hash is what a run would hash now
// is stamped current (Document::MarkStudyResultsCurrent): a reopened project
// whose parts were regenerated again after loading (the project scope, a
// variable edit) still holds the answers its inputs have. One that does not
// match keeps its stamp. Shape studies after the results they are cut from.
void RestampByInputs(Document& doc);
uint64_t AssemblyStudyInputKey(const AssemblyDocument& asmDoc,
                               const Study& study,
                               const AssemblyStudyContext& ctx);

// The derived results of an assembly's studies, shared between the document
// and its undo snapshots (a full-state copy must not duplicate meshes).
struct StudyRuntimeCache {
  std::map<uint64_t, StudyRuntime> byStudy;
  uint64_t epoch = 0;
};
// Solves every body of a meshed study; a static one through SolveCases with
// `options`, and rt.solver says which solver ran.
bool SolvePreparedStudy(StudyJobInput& in, StudyRuntime& rt, std::string* error,
                        const SolveOptions& options = {});

// How a run checks that its peak has settled (docs/simulation/README.md
// #a-peak-that-has-settled): the study's `converge` or a run's override,
// the converge_auto_s preference, and the build direction to settle the
// stress across a printed part's layers along.
struct ConvergeOptions {
  ConvergeMode mode = ConvergeMode::Auto;
  double autoSeconds = kConvergeAutoSecondsDefault;
  bool hasLayerNormal = false;
  glm::dvec3 layerNormal{0.0, 0.0, 1.0};
};
// MeshPlannedStudy + SolvePreparedStudy, then, on a static study, the
// convergence loop: the governing peaks' faces halved, re-meshed and
// re-solved until every peak moves under 2 % or four refinements are spent
// (or, on auto, the next step would run past autoSeconds). `rt.convergence`
// records it; the refinements belong to this run and never to the study.
// `in` is left holding the last step's input. Fails as the two do; a step
// after the first that fails ends the loop with the last good result.
bool SolveConverged(const StudyPlan& plan, StudyJobInput& in, StudyRuntime& rt,
                    const ConvergeOptions& converge, std::string* error,
                    const SolveOptions& options = {});
// The input hash of a run of `study` under `converge`: the study hashed with
// the run's mode in place of its own (StudyInputKey of `doc`, or of the
// assembly through `assemblyKey`), the layer normal mixed in when the loop
// reads one. A run's override is an input like any other.
uint64_t RunInputKey(const Document& doc, const Study& study,
                     const ConvergeOptions& converge);
uint64_t RunInputKey(uint64_t studyKeyWithMode,
                     const ConvergeOptions& converge);

// Meshes the study's single loaded body (v1) and runs SIMP topology
// optimization on it (the study's Force/Pressure loads + fixed supports; keep
// faces frozen solid; gravity ignored). Fills `rt` with the optimized per-tet
// density + convergence summary (rt.topology = true); not stamped — the caller
// sets solvedAtVersion. On any failure sets study.error and *error, returns
// false. Synchronous; the GUI's background run instead calls PrepareTopology
// and TopologyOptimize on a worker thread (FemTopo.h).
bool RunTopology(const Document& doc, Study& study, StudyRuntime& rt,
                 std::string* error);

} // namespace fem
