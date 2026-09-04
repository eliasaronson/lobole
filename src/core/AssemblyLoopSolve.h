#pragma once

#include "core/Occurrence.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

// A6 — the numeric half of the assembly solve: closing genuine mate LOOPS.
//
// A3 places mate *trees* in closed form (assembly::Solve) and reports loops as
// over-constrained. This closes them, in REDUCED (joint) coordinates: the
// unknowns are the mates' own DOF fields, which the document already stores,
// so there is no new state and no format change. See
// docs/assemblies/a6-loop-solver-plan.md.
//
// The formulation is MuJoCo's: a spanning tree in generalized coordinates plus
// the cut edges re-added as equality constraints. The redundancy handling is
// OndselSolver's and SolveSpace's: factorize the Jacobian with pivoting and
// the rows that find no pivot ARE the redundant ones — which is what lets us
// name the two extra bolts instead of counting them.
namespace assembly {

// The adjustment from a's connector frame to b's, by explicit DOF rather than
// by reading a Mate. THE one definition of the composition — AssemblySolve's
// MateAdjust(const Mate&) forwards to it, so the solver and the placement pass
// cannot drift apart (a transform with a second copy in this codebase has
// drifted before).
glm::mat4 MateAdjustFor(bool flipped, float rotation, float offset, float dx,
                        float dy, const glm::quat& ballRotation);

// How many DOF this mate type leaves free — the width of its slice of q.
// Fastened 0, Revolute/Slider 1, Cylindrical 2, Planar/Ball 3.
int MateDofCount(MateType type);

// One mate as the numeric solver sees it: two connector frames in their
// occurrences' local space, plus the seed pose. Deliberately free of
// Document/ElementRef — the frames are already resolved by the caller, so this
// half is pure math and testable without any geometry.
struct LoopMate {
  uint64_t id = 0;
  MateType type = MateType::Fastened;
  uint64_t aOcc = 0;
  uint64_t bOcc = 0;
  glm::mat4 fa{1.0f}; // a's connector frame, in a's occurrence space
  glm::mat4 fb{1.0f}; // b's connector frame, in b's occurrence space
  bool flipped = true;
  float rotation = 0.0f;
  float offset = 0.0f;
  float dx = 0.0f;
  float dy = 0.0f;
  glm::quat ballRotation{1.0f, 0.0f, 0.0f, 0.0f};
};

// One placement step: `to` is positioned from `from` across `mate`.
struct LoopTreeEdge {
  size_t mate = 0; // index into LoopProblem::mates
  uint64_t from = 0;
  uint64_t to = 0;
  bool reversed = false; // true when `from` is the mate's B side
};

// The problem the caller hands over: the spanning forest it already walked,
// the edges that closed loops, and where each component is pinned.
// A relation between two mates' DOFs (docs/assemblies/relations-plan.md):
// one residual row, dofB - ratio * dofA - phase, beside the six per cut mate.
struct LoopRelation {
  size_t mateA = 0; // indices into LoopProblem::mates
  size_t mateB = 0;
  MateDof dofA = MateDof::Rotation;
  MateDof dofB = MateDof::Rotation;
  double ratio = -1.0;
  double phase = 0.0;
  // A cam's lift table (v88), empty for every other kind. When set, the row
  // is dofB - lift(dofA) - phase rather than dofB - ratio*dofA - phase, and
  // the solver differentiates the spline rather than a constant.
  std::vector<std::pair<double, double>> lift;
};

struct LoopProblem {
  std::vector<LoopMate> mates;
  std::vector<LoopTreeEdge> tree;                     // in placement order
  std::vector<size_t> cut;                            // indices into `mates`
  std::vector<std::pair<uint64_t, glm::mat4>> anchor; // pinned occ -> world
  std::vector<LoopRelation> relations;
};

// The column of one DOF of a mate in the solver's state vector, or -1 when
// that mate type has no such DOF. `firstColumn` is the mate's first column
// (PackDofs); shared with the residual code so the two cannot disagree.
int DofColumn(MateType type, MateDof dof, int firstColumn);

struct LoopSolution {
  bool ran = false;       // false when there was nothing to close
  bool converged = false; // residual fell under tolerance
  int iterations = 0;
  int dof = 0;             // remaining degrees of freedom: cols - rank(J)
  double residualMm = 0.0; // worst closure error, mm
  std::vector<uint64_t> redundant;   // mate ids: dependent but CONSISTENT
  std::vector<uint64_t> conflicting; // mate ids that could not close
  std::vector<LoopMate> solved;      // `mates` with their DOF updated
  std::unordered_map<uint64_t, glm::mat4> world; // solved placements
  // Non-empty when this build has no Eigen: the caller keeps A3 behaviour and
  // says so rather than silently placing something plausible.
  std::string unavailable;
};

// Closes every loop in `problem`. Loop-free problems are not this function's
// business — the caller keeps its closed-form path, so existing assemblies
// never touch a solver.
LoopSolution SolveLoops(const LoopProblem& problem);

// Forward kinematics: walk the tree from the anchors at the given DOF values.
// Exposed because it is the residual evaluation AND the write-back pass, and
// because a test wants to check placement without running the solver.
std::unordered_map<uint64_t, glm::mat4>
EvaluateTree(const LoopProblem& problem, const std::vector<LoopMate>& mates);

} // namespace assembly
