#pragma once

// Linear-static FEM solver: tet10 isotropic elasticity on Eigen. GUI-free core,
// unit system mm-N-MPa (lengths/displacements mm, forces N, stresses/moduli
// MPa). No Netgen dependency — it consumes a FemMesh (plain data). Gated
// HAVE_FEM_SOLVER on Eigen availability, like the mesher is gated on Netgen.

#include "core/fem/FemMesher.h"
#include "core/fem/FemStudy.h"

#include <glm/glm.hpp>

#include <array>
#include <functional>
#include <map>
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
struct FaceLoad {
  LoadType type = LoadType::Force; // Force or Pressure (Fixed is a constraint)
  std::vector<int> tris;           // indices into FemMesh::boundary
  double magnitude = 0.0;          // total N (Force) or MPa (Pressure)
  glm::dvec3 direction{0, 0, 0};   // Force only (need not be normalized)
};

// Solved fields, one entry per mesh node.
struct FemResults {
  std::vector<glm::dvec3> displacement; // mm
  std::vector<double> vonMises;         // MPa

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
    std::vector<glm::dvec3> displacement; // normalized (max |u| = 1)
  };
  std::vector<ModeShape> modes;
};

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

// One body of a study: its mesh and solved fields (all derived/transient).
// For a topology study `results` is empty and `density` carries the optimized
// per-tet density (parallel to mesh.tets); for static/modal `density` is empty.
struct StudyBodyResult {
  int bodyIndex = -1;
  std::string bodyName;
  FemMesh mesh;
  FemResults results;
  std::vector<double> density; // topology only: one entry per mesh.tets
};

// A study's derived mesh + results, cached on the Document. Stale when
// solvedAtVersion != Document::Version(). Maxima are aggregated across all
// solved bodies for the summary an engineer reads.
//
// FEM (static/modal) runtimes are never serialized (recompute = seconds).
// A TOPOLOGY runtime IS serialized (topology != 0): its result is minutes of
// compute over a mesh that is not reproducible across Netgen versions, so it is
// persisted into the document and reloaded into this same cache slot.
struct StudyRuntime {
  uint64_t solvedAtVersion = 0;
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

  // Provenance of the solve — transient (NOT serialized; a reloaded result
  // reads empty/zero): which linear solver ran ("gpu: <device>" /
  // "cpu (...)") and the wall time of the whole optimization.
  std::string solver;
  double solveSeconds = 0.0;
};

// (Document is forward-declared at global scope by FemStudy.h.)

// Meshes and solves the study across every body it loads: evaluates the
// expressions, resolves refs, validates (material has stiffness data, each
// loaded body has a fixed support), then meshes and solves. On any failure
// sets study.error and *error and returns false. Fills `rt` (not stamped —
// the caller stamps solvedAtVersion and stores it in the Document cache).
bool RunStudy(const Document& doc, Study& study, StudyRuntime& rt,
              std::string* error);

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
  std::vector<FaceLoad> faceLoads;
};
struct StudyJobInput {
  AnalysisType type = AnalysisType::Static;
  int numModes = 8;
  glm::dvec3 gravity{0.0};
  std::vector<StudyJobBody> bodies;
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

// The derived results of an assembly's studies, shared between the document
// and its undo snapshots (a full-state copy must not duplicate meshes).
struct StudyRuntimeCache {
  std::map<uint64_t, StudyRuntime> byStudy;
  uint64_t epoch = 0;
};
bool SolvePreparedStudy(StudyJobInput& in, StudyRuntime& rt,
                        std::string* error);

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
