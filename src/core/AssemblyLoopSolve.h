// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "core/Occurrence.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

// A6 — the numeric half of the assembly solve: closing genuine mate LOOPS.
//
// A3 places mate *trees* in closed form (assembly::Solve) and reports loops as
// over-constrained. This closes them, in REDUCED (joint) coordinates: the
// unknowns are the mates' own DOF fields, which the document already stores,
// so there is no new state and no format change. See
// docs/assemblies/solver.md#closing-loops.
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
glm::dmat4 MateAdjustFor(bool flipped, double rotation, double offset,
                         double dx, double dy, const glm::dquat& ballRotation);

// Which of a fastened mate's numbers to READ OFF the placement the two
// occurrences are already in, rather than take from the caller.
//
//   Twist  which side and how far round — `flipped` and `rotation` — with
//          dx/dy/offset left at zero, so the two anchors coincide. The joint
//          is exact and the part is turned the way it already roughly was.
//   Pose   all five, so the mate holds the parts exactly where they are.
//          Reproduces any error in that placement, which is the trade.
enum class MateDerive { Twist, Pose };

// The exact inverse of `MateAdjustFor`: given b's connector frame relative to
// a's, fill in the DOFs that compose to it. Returns false when `rel` is NOT of
// that form — the two anchors are not square to one axis — leaving `flipped`,
// `rotation`, `dx`, `dy` and `offset` untouched. Refusing there is the point:
// the nearest quarter turn between skew frames is an answer to a question
// nobody asked, and it is indistinguishable from a joint that fits.
[[nodiscard]] bool DeriveMateDofs(const glm::dmat4& rel, MateDerive how,
                                  bool& flipped, double& rotation, double& dx,
                                  double& dy, double& offset);

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
  glm::dmat4 fa{1.0}; // a's connector frame, in a's occurrence space
  glm::dmat4 fb{1.0}; // b's connector frame, in b's occurrence space
  bool flipped = true;
  double rotation = 0.0;
  double offset = 0.0;
  double dx = 0.0;
  double dy = 0.0;
  glm::dquat ballRotation{1.0, 0.0, 0.0, 0.0};
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
// A relation between two mates' DOFs (docs/assemblies/relations.md):
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
  // the solver differentiates the spline rather than a constant. A turning
  // follower's (dofB rotation, v102) table is in DEGREES; the row converts.
  std::vector<std::pair<double, double>> lift;
  // A relation over several mates (v171): when set, mateA/mateB/ratio are
  // unused and the row is sum(coefficient * dof) - phase, divided by the
  // largest coefficient so it weighs like a binary row.
  struct Term {
    size_t mate = 0; // index into LoopProblem::mates
    MateDof dof = MateDof::Rotation;
    double coefficient = 1.0;
  };
  std::vector<Term> terms;
  // An intermittent relation (v174): when set, the row is
  // dofB - value(dofA), its phase included, rather than a ratio's.
  std::function<double(double)> value;
};

// A DOF held at a value while the loop closes around it (v88 fix): the mate
// the caller just DROVE.
//
// Without this the driven DOF is one more unknown in an underdetermined
// least-squares, and the solver moves it: setting a crank to 90 degrees and
// reading back 67 is what the single-cylinder engine found
// (generators/README.md#single-cylinder-and-air-cooled-engines). A closed
// loop's whole point is that turning the crank moves the piston, and it did
// not.
struct LoopPin {
  size_t mate = 0; // index into LoopProblem::mates
  MateDof dof = MateDof::Rotation;
  double value = 0.0;
};

struct LoopProblem {
  std::vector<LoopMate> mates;
  std::vector<LoopTreeEdge> tree;                     // in placement order
  std::vector<size_t> cut;                            // indices into `mates`
  std::vector<std::pair<uint64_t, glm::dmat4>> anchor; // pinned occ -> world
  std::vector<LoopRelation> relations;
  std::vector<LoopPin> pinned;
  // Run the tilted-axis diagnosis (DiagnoseLoop) on every component that
  // fails to close. Off by default: it costs one extra solve per
  // axis-carrying mate of a failing component, which a motion study's
  // per-sample solve has no use for.
  bool diagnose = false;
};

// The column of one DOF of a mate in the solver's state vector, or -1 when
// that mate type has no such DOF. `firstColumn` is the mate's first column
// (PackDofs); shared with the residual code so the two cannot disagree.
int DofColumn(MateType type, MateDof dof, int firstColumn);

// One connector of an axis-carrying mate, as the diagnosis tried it: the loop
// re-solved with that connector free to tilt (two extra unknowns, rotations
// about the connector's own X and Y at its origin).
struct LoopTiltTrial {
  uint64_t mate = 0;
  // 'a' or 'b': which connector tilted. In a suspect, 0 means either did
  // equally well, so the diagnosis cannot say which side is wrong.
  char connector = 'a';
  double tiltRad = 0.0;    // how far the axis had to tilt, radians
  double residualMm = 0.0; // the loop's closure error with that tilt, mm
  // The mechanism's mobility with the tilt applied and then held: the joint
  // columns' DOF. A tilt that restores the intended mechanism gives back its
  // mobility; one that closes the loop only at an isolated pose does not.
  int dof = 0;
  bool closes = false; // residualMm within the closure tolerance
};

// What the diagnosis concluded about one component that failed to close.
// `suspects` is what the numbers support: the mates with a closing trial of
// the highest mobility, one entry per mate. One suspect names the mate;
// several are named together and none is claimed; none means the fault is
// not a single tilted axis.
struct LoopDiagnosis {
  std::vector<uint64_t> conflicting; // this component's cut mates that failed
  std::vector<LoopTiltTrial> trials; // both connectors of every axis mate
  std::vector<LoopTiltTrial> suspects;
};

struct LoopSolution {
  bool ran = false;       // false when there was nothing to close
  bool converged = false; // residual fell under tolerance
  int iterations = 0;
  int dof = 0;             // remaining degrees of freedom: cols - rank(J)
  double residualMm = 0.0; // worst closure error, mm
  std::vector<uint64_t> redundant;   // mate ids: dependent but CONSISTENT
  std::vector<uint64_t> conflicting; // mate ids that could not close
  // Each conflicting mate's OWN closure error (the largest entry of its six
  // residual rows, mm), parallel to `conflicting`.
  std::vector<double> conflictingMm;
  // One per failing component, when the problem asked to `diagnose`.
  std::vector<LoopDiagnosis> diagnoses;
  std::vector<LoopMate> solved;      // `mates` with their DOF updated
  std::unordered_map<uint64_t, glm::dmat4> world; // solved placements
  // Non-empty when this build has no Eigen: the caller keeps A3 behaviour and
  // says so rather than silently placing something plausible.
  std::string unavailable;
};

// Closes every loop in `problem`. Loop-free problems are not this function's
// business — the caller keeps its closed-form path, so existing assemblies
// never touch a solver.
LoopSolution SolveLoops(const LoopProblem& problem);

// For ONE component that failed to close (`problem` holds only it): which
// single revolute, cylindrical or slider mate — tree or cut — would let the
// loop close if its axis were allowed to tilt. SolveLoops calls this when
// `problem.diagnose` is set; exposed so a test can read every trial.
// docs/assemblies/solver.md#naming-the-mate-that-keeps-a-loop-open
LoopDiagnosis DiagnoseLoop(const LoopProblem& problem);

// Forward kinematics: walk the tree from the anchors at the given DOF values.
// Exposed because it is the residual evaluation AND the write-back pass, and
// because a test wants to check placement without running the solver.
std::unordered_map<uint64_t, glm::dmat4>
EvaluateTree(const LoopProblem& problem, const std::vector<LoopMate>& mates);

} // namespace assembly
