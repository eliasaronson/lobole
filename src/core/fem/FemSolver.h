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
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

class AssemblyDocument;
struct Body;

namespace fem {

// Isotropic linear-elastic material in solver units.
struct Material {
  double youngMPa = 0.0; // Young's modulus, MPa (E in GPa * 1000)
  double poisson = 0.0;
  // Mass density in tonne/mm^3 (the consistent mm-N-t-s unit system, so
  // frequencies come out in rad/s -> Hz). Convert the library's kg/m^3 by
  // *1e-12. 0 = unused (static without gravity).
  double densityTPerMM3 = 0.0;
};

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
  struct ModeShape {
    double frequencyHz = 0.0;
    std::vector<glm::dvec3> displacement; // normalized (max |u| = 1)
  };
  std::vector<ModeShape> modes;
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
  // before vNEXT.
  std::vector<int> supportEdgeZone;
  std::vector<FemResults::SupportReaction> edgeOwners;
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
  std::vector<FaceLoad> faceLoads;
  // With load cases: each case's face loads (`faceLoads` is then empty).
  std::vector<std::vector<FaceLoad>> caseLoads;
};
struct StudyJobInput {
  AnalysisType type = AnalysisType::Static;
  Solver solver = Solver::Auto;
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
  Solver solver = Solver::Auto;
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
