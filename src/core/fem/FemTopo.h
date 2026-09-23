#pragma once

// Topology optimization (SIMP density method + optimality-criteria update), the
// generative-design core. GUI-free, namespace fem, gated HAVE_FEM_SOLVER (it
// reuses the tet10 assembly math from FemSolver). Consumes a FemMesh + face
// loads/fixtures (the same inputs the static solver takes) and produces a
// per-element density field that minimizes compliance at a target volume
// fraction. No Netgen dependency, no Document access — plain data in, plain
// data out — so it is unit-testable headless and, later, driveable from a
// worker thread over an immutable snapshot (the progress/cancel callback is
// that seam).
//
// This is the numerics of topology optimization; the Study, serialization,
// RPC and GUI surface are elsewhere (see docs/topopt/README.md).

#include "core/fem/FemMesher.h"
#include "core/fem/FemSolver.h" // Material, FaceLoad

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace fem {

// SIMP + optimality-criteria controls. Defaults are the classic textbook
// values (p=3, move limit 0.2, damping 0.5) that converge on the benchmark
// problems; the caller overrides volumeFraction/filterRadius per study.
struct TopoParams {
  double volumeFraction = 0.3; // target solid fraction of the DESIGN volume
  double filterRadius = 0.0;   // mm; 0 = auto (2.5x the mean element size)
  int maxIterations = 60;
  double penalty = 3.0; // final SIMP penalization exponent p
  // Penalty continuation: p ramps linearly from 1 (convex, no penalization) to
  // `penalty` over the first `penaltyRampIters` iterations, then holds. This is
  // the standard robustness device — the convex early phase finds the global
  // material layout, the ramp then drives it black/white without trapping the
  // optimizer in a gray local minimum. Convergence is only declared once the
  // ramp has finished. 0 (default) disables continuation — sensitivity
  // filtering already yields a discrete design at constant p.
  double penaltyRampIters = 0.0;
  double moveLimit = 0.2;  // max per-iteration change in a design variable
  double damping = 0.5;    // OC update exponent eta
  double changeTol = 0.01; // converge when max |dx| < this
};

// Reported once per iteration to the progress callback (and accumulated into
// TopoResult::complianceHistory).
struct TopoProgress {
  int iteration = 0;
  double compliance = 0.0;     // u^T K u at this iteration (lower = stiffer)
  double maxChange = 0.0;      // max |dx| this iteration
  double volumeFraction = 0.0; // achieved solid fraction this iteration
};

// The optimized density field + convergence history. `density` is the physical
// (filtered) element density in [0,1], one entry per mesh tet, in tet order.
struct TopoResult {
  std::vector<double> density;
  std::vector<double> complianceHistory;
  int iterations = 0;
  bool converged = false; // true if it hit changeTol before the cap
  bool cancelled = false; // true if the progress callback stopped it
  double volumeFractionAchieved = 0.0;
  // Empty when the injected solver carried the whole run. Otherwise: why it
  // was dropped mid-run for the built-in path. A caller that names its solver
  // in the UI must say so, or it reports a GPU run that the CPU finished.
  std::string solverFallback;
};

// ---- Pluggable linear solver (the GPU seam) -------------------------------
// TopologyOptimize's inner loop solves K(x) u = F once per SIMP iteration,
// with K = sum_e scale_e * Ke0_e over the reduced DOF map. A TopoLinearSolver
// replaces the built-in Eigen CG-IC with an external implementation — the
// Vulkan matrix-free CG in fem_gpu (docs/topopt/README.md#the-gpu-solver) —
// without pulling any GPU dependency into designer_core. Null = the built-in
// CPU path, which remains the fallback and the correctness oracle.
class TopoLinearSolver {
public:
  virtual ~TopoLinearSolver() = default;
  // Called once per optimization run, before any Solve. `dofMap` is the
  // reduced-DOF map (3 per node, -1 = fixed), `material` gives E0/nu for the
  // full-modulus element matrices.
  virtual bool Setup(const FemMesh& mesh, const Material& material,
                     const std::vector<int>& dofMap, int nFree,
                     std::string* error) = 0;
  // Solve sum_e scale_e Ke0_e u = F on the reduced system. `u` carries the
  // warm start in and the solution out (size nFree). Called once per SIMP
  // iteration; `scale` (per-element E(rho)/E0, spanning ~1e-6..1) changes
  // between calls, F does not.
  virtual bool Solve(const std::vector<double>& scale,
                     const std::vector<double>& F, std::vector<double>& u,
                     std::string* error) = 0;
  // Optional capability (GPU4): per-element strain energies at FULL modulus,
  // se_e = ue^T Ke0_e ue, for the reduced displacement `u` (size nFree,
  // fixed DOFs implicitly zero). Returning false (the default) makes the
  // caller run its own per-element CPU loop — which remains the oracle — so
  // implementations may decline at any time.
  virtual bool ElementEnergies(const std::vector<double>& u,
                               std::vector<double>& se, std::string* error) {
    (void)u;
    (void)se;
    (void)error;
    return false;
  }
};

// Minimizes compliance u^T K u subject to a volume-fraction constraint over the
// mesh, treating each tet's density as a design variable. Every tet touching a
// fixed node or a loaded boundary triangle is frozen solid (density 1) so
// supports and load-application regions never vanish. `fixedNodes` and `loads`
// are exactly the static-solve inputs (gravity/body loads are intentionally
// excluded — self-weight is a design-dependent load that changes the
// sensitivity). Returns nullopt + *error on bad input (empty mesh, no
// stiffness data, degenerate volume fraction) or when built without Eigen.
//
// `progress`, if set, is called after each iteration; returning false cancels
// the run and returns the best-so-far densities with cancelled=true. This is
// the hook TO2's worker thread drives; in the spike it is used by the
// cancel test and for timing logs.
// `keepNodes` are frozen solid in addition to the auto-frozen support/load
// skin (user "keep" faces — bolt bosses, bearing seats that must not vanish).
// `solver`, if set, replaces the built-in Eigen CG-IC for the per-iteration
// linear solves (see TopoLinearSolver above); a Solve failure fails the run.
std::optional<TopoResult> TopologyOptimize(
    const FemMesh& mesh, const Material& material,
    const std::vector<int>& fixedNodes, const std::vector<FaceLoad>& loads,
    const TopoParams& params, std::string* error,
    const std::function<bool(const TopoProgress&)>& progress = {},
    const std::vector<int>& keepNodes = {}, TopoLinearSolver* solver = nullptr);

// True when the build links Eigen (same gate as the static solver).
bool TopoAvailable();

// ---- Result display (iso-surface extraction) ------------------------------
// These are pure geometry (no Eigen), so a persisted topology result can be
// displayed on a build without the solver/mesher.

// Marching-tetrahedra iso-surface of a density field at level `threshold`
// (in [0,1]): per-tet density is averaged to the corner nodes, then each tet's
// linear tetrahedron is polygonised (the midside nodes are ignored). Triangles
// are appended to the outputs with flat facet normals oriented outward (toward
// lower density). Call once per body; the outputs accumulate.
void ExtractIsoSurface(const FemMesh& mesh, const std::vector<double>& density,
                       double threshold, std::vector<glm::vec3>& positions,
                       std::vector<glm::vec3>& normals,
                       std::vector<uint32_t>& indices);

// The iso-level whose thresholded solid volume matches `targetFraction` of the
// mesh volume (bisection over per-element density). The default when the user
// has not set an explicit threshold.
double AutoIsoThreshold(const FemMesh& mesh, const std::vector<double>& density,
                        double targetFraction);

// Extracts the iso-surface at `threshold`, welds it, applies `smoothIters`
// Taubin (lambda/mu) smoothing passes (shrink-free surface fairing), and writes
// a binary STL to `path` — the print-it-directly path for a generative result.
// Returns false + *error on an empty surface or an I/O failure. Pure geometry.
// `stats` (optional) reports what a slicer will see: triangle count and
// boundary-edge count (0 boundary edges = watertight), mirroring export_stl's
// honesty about non-watertight sources.
struct TopoStlStats {
  size_t triangles = 0;
  size_t boundaryEdges = 0;
  bool watertight = false;
};
bool ExportTopoStl(const FemMesh& mesh, const std::vector<double>& density,
                   double threshold, int smoothIters, const std::string& path,
                   std::string* error, TopoStlStats* stats = nullptr);

// ---- Study orchestration (async-ready) ------------------------------------

// A self-contained topology-optimization job: everything TopologyOptimize
// needs, resolved off the Document up front, so the optimizer can run on a
// worker thread with no Document access (the one join point is on the main
// thread — see PrepareTopology / MakeTopoRuntime). Plain fem/std data only.
struct TopoInput {
  int bodyIndex = -1;
  std::string bodyName;
  FemMesh mesh;
  Material material;
  std::vector<int> fixedNodes;
  std::vector<FaceLoad> loads;
  std::vector<int> keepNodes;
  TopoParams params;
};

// Main-thread preparation: evaluates the study's expressions, resolves its
// refs against the document, meshes the single loaded body (v1), and builds the
// immutable `TopoInput` snapshot. On any failure sets study.error and *error
// and returns false. After this returns, TopologyOptimize(input...) is a pure
// function of `input` — safe to run on another thread.
bool PrepareTopology(const Document& doc, Study& study, TopoInput& out,
                     std::string* error);

// Builds the persisted runtime (rt.topology = true) from a finished
// optimization: moves `input.mesh` into the single body result, attaches the
// density field + convergence summary. `solvedAtVersion` is left 0 (the caller
// stamps it).
StudyRuntime MakeTopoRuntime(TopoInput& input, const TopoResult& result);

} // namespace fem
