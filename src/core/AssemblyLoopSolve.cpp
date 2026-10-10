// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/AssemblyLoopSolve.h"
#include "core/Angles.h"

#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <functional>

#ifdef HAVE_EIGEN
#include <Eigen/Dense>
#endif

namespace assembly {

// THE definition of a mate's adjustment, in double like everything it
// composes: a chain of float matrices left ~1e-3 mm of noise at 100 mm
// scale, enough to swamp the loop solver's finite-difference Jacobian and
// stall it just short of closing.
glm::dmat4 MateAdjustFor(bool flipped, double rotation, double offset,
                         double dx, double dy, const glm::dquat& ballRotation) {
  glm::dmat4 a(1.0);
  a = glm::translate(a, glm::dvec3(dx, dy, offset));
  a = glm::rotate(a, rotation, glm::dvec3(0.0, 0.0, 1.0));
  a *= glm::mat4_cast(ballRotation);
  if (flipped)
    a = glm::rotate(a, glm::pi<double>(), glm::dvec3(1.0, 0.0, 0.0));
  return a;
}

bool DeriveMateDofs(const glm::dmat4& rel, MateDerive how, bool& flipped,
                    double& rotation, double& dx, double& dy, double& offset) {
  // A fastened adjust is T(dx,dy,offset) * Rz(rotation) * [Rx(180)]. Its
  // rotation part is therefore determined by the third column's sign and one
  // atan2, and its translation IS the fourth column.
  const bool over = rel[2][2] < 0.0;
  const double turn = std::atan2(rel[0][1], rel[0][0]);
  const double tx = how == MateDerive::Pose ? rel[3][0] : 0.0;
  const double ty = how == MateDerive::Pose ? rel[3][1] : 0.0;
  const double tz = how == MateDerive::Pose ? rel[3][2] : 0.0;

  // Compose it back and insist it is what we were given. `Twist` discards the
  // translation deliberately, so only the rotation is checked there — but it
  // is checked, which is what separates "turned a quarter out" from "these
  // two frames are skew and no turn will seat them".
  const glm::dmat4 back =
      MateAdjustFor(over, turn, tz, tx, ty, glm::dquat(1.0, 0.0, 0.0, 0.0));
  const int cols = how == MateDerive::Pose ? 4 : 3;
  for (int c = 0; c < cols; c++)
    for (int r = 0; r < 3; r++)
      if (std::abs(back[c][r] - rel[c][r]) > 1e-3)
        return false;

  flipped = over;
  rotation = turn;
  dx = tx;
  dy = ty;
  offset = tz;
  return true;
}

int MateDofCount(MateType type) {
  switch (type) {
  case MateType::Fastened:
    return 0;
  case MateType::Revolute: // rotation about Z
  case MateType::Slider:   // translation along Z
    return 1;
  case MateType::Cylindrical: // both
    return 2;
  case MateType::Planar: // dx, dy, spin
  case MateType::Ball:   // free 3-axis rotation
    return 3;
  }
  return 0;
}

namespace {

// A loop closes when its error transform is the identity; this is the
// tolerance on that, in mm. Deliberately the 1e-3 the A3 consistency check
// already used, so "closed" keeps meaning what it meant.
constexpr double kCloseTolMm = 1e-3;

glm::dmat4 AdjustOfD(const LoopMate& m) {
  return MateAdjustFor(m.flipped, m.rotation, m.offset, m.dx, m.dy,
                       m.ballRotation);
}

// Rotation vector of R (axis * angle). Via the quaternion rather than the
// matrix-log series: the series needs separate branches at 0 and at pi, and
// this one is well conditioned everywhere.
glm::dvec3 LogSo3(const glm::dmat3& rot) {
  glm::dquat q = glm::normalize(glm::quat_cast(rot));
  if (q.w < 0.0) // shortest path: -q is the same rotation
    q = glm::dquat(-q.w, -q.x, -q.y, -q.z);
  const glm::dvec3 v(q.x, q.y, q.z);
  const double vn = glm::length(v);
  if (vn < 1e-15)
    return glm::dvec3(0.0);
  return v * (2.0 * std::atan2(vn, q.w) / vn);
}

glm::dquat ExpSo3(const glm::dvec3& w) {
  const double th = glm::length(w);
  if (th < 1e-15)
    return glm::dquat(1.0, 0.0, 0.0, 0.0);
  const glm::dvec3 axis = w / th;
  const double s = std::sin(th * 0.5);
  return glm::dquat(std::cos(th * 0.5), axis.x * s, axis.y * s, axis.z * s);
}

// q layout: each mate contributes MateDofCount(type) entries, in mate order.
struct Packing {
  std::vector<int> columnOf; // per mate -> first column, -1 when it has none
  int cols = 0;
};

} // namespace

int DofColumn(MateType type, MateDof dof, int firstColumn) {
  if (firstColumn < 0)
    return -1;
  // Mirrors SeedQ/AdjustAt's packing order per type.
  switch (type) {
  case MateType::Revolute:
    return dof == MateDof::Rotation ? firstColumn : -1;
  case MateType::Slider:
    return dof == MateDof::Offset ? firstColumn : -1;
  case MateType::Cylindrical:
    return dof == MateDof::Rotation ? firstColumn : firstColumn + 1;
  case MateType::Planar:
    return dof == MateDof::Rotation ? firstColumn + 2 : -1;
  case MateType::Ball:
  case MateType::Fastened:
    break;
  }
  return -1;
}

namespace {

Packing PackDofs(const std::vector<LoopMate>& mates) {
  Packing p;
  p.columnOf.assign(mates.size(), -1);
  for (size_t i = 0; i < mates.size(); i++) {
    const int n = MateDofCount(mates[i].type);
    if (n == 0)
      continue;
    p.columnOf[i] = p.cols;
    p.cols += n;
  }
  return p;
}

std::vector<double> SeedQ(const std::vector<LoopMate>& mates,
                          const Packing& pack) {
  std::vector<double> q(static_cast<size_t>(pack.cols), 0.0);
  for (size_t i = 0; i < mates.size(); i++) {
    const int c = pack.columnOf[i];
    if (c < 0)
      continue;
    const LoopMate& m = mates[i];
    switch (m.type) {
    case MateType::Revolute:
      q[c] = m.rotation;
      break;
    case MateType::Slider:
      q[c] = m.offset;
      break;
    case MateType::Cylindrical:
      q[c] = m.rotation;
      q[c + 1] = m.offset;
      break;
    case MateType::Planar:
      q[c] = m.dx;
      q[c + 1] = m.dy;
      q[c + 2] = m.rotation;
      break;
    case MateType::Ball: // a DELTA on the seed quaternion: starts at zero
    case MateType::Fastened:
      break;
    }
  }
  return q;
}

// Mate i's adjustment at the given q, without writing q into the LoopMate.
//
// Scalars are absolute (seeded from the stored pose); the ball rotation is a
// so(3) DELTA composed onto its seed, which keeps the unknown unconstrained —
// no unit-norm side condition for the solver to maintain.
glm::dmat4 AdjustAt(const LoopMate& seed, int col,
                    const std::vector<double>& q) {
  if (col < 0)
    return AdjustOfD(seed);
  switch (seed.type) {
  case MateType::Revolute:
    return MateAdjustFor(seed.flipped, q[col], seed.offset, seed.dx, seed.dy,
                         seed.ballRotation);
  case MateType::Slider:
    return MateAdjustFor(seed.flipped, seed.rotation, q[col], seed.dx, seed.dy,
                         seed.ballRotation);
  case MateType::Cylindrical:
    return MateAdjustFor(seed.flipped, q[col], q[col + 1], seed.dx, seed.dy,
                         seed.ballRotation);
  case MateType::Planar:
    return MateAdjustFor(seed.flipped, q[col + 2], seed.offset, q[col],
                         q[col + 1], seed.ballRotation);
  case MateType::Ball:
    return MateAdjustFor(
        seed.flipped, seed.rotation, seed.offset, seed.dx, seed.dy,
        glm::normalize(seed.ballRotation *
                       ExpSo3(glm::dvec3(q[col], q[col + 1], q[col + 2]))));
  case MateType::Fastened:
    break;
  }
  return AdjustOfD(seed);
}

// Written back into LoopMate fields ONCE, at the end.
std::vector<LoopMate> ApplyQ(const std::vector<LoopMate>& seed,
                             const Packing& pack,
                             const std::vector<double>& q) {
  std::vector<LoopMate> out = seed;
  for (size_t i = 0; i < out.size(); i++) {
    const int c = pack.columnOf[i];
    if (c < 0)
      continue;
    LoopMate& m = out[i];
    switch (m.type) {
    case MateType::Revolute:
      m.rotation = q[c];
      break;
    case MateType::Slider:
      m.offset = q[c];
      break;
    case MateType::Cylindrical:
      m.rotation = q[c];
      m.offset = q[c + 1];
      break;
    case MateType::Planar:
      m.dx = q[c];
      m.dy = q[c + 1];
      m.rotation = q[c + 2];
      break;
    case MateType::Ball:
      m.ballRotation = glm::normalize(
          seed[i].ballRotation * ExpSo3(glm::dvec3(q[c], q[c + 1], q[c + 2])));
      break;
    case MateType::Fastened:
      break;
    }
  }
  return out;
}

// ONE tree walk, in double, driven by per-mate adjustments the caller chose.
// Both the public EvaluateTree and every residual evaluation inside the
// solver go through here, so there is no second copy of the composition to
// drift.
std::unordered_map<uint64_t, glm::dmat4>
WalkTree(const LoopProblem& problem, const std::vector<glm::dmat4>& adjust) {
  std::unordered_map<uint64_t, glm::dmat4> world;
  for (const auto& [id, m] : problem.anchor)
    world[id] = m;
  // In placement order, so every edge's `from` is already known.
  for (const LoopTreeEdge& e : problem.tree) {
    const auto from = world.find(e.from);
    if (from == world.end())
      continue; // a forest the caller built wrong; place what we can
    const LoopMate& m = problem.mates[e.mate];
    const glm::dmat4 fa(m.fa), fb(m.fb);
    world[e.to] = e.reversed
                      ? from->second * fb * glm::inverse(adjust[e.mate]) *
                            glm::inverse(fa)
                      : from->second * fa * adjust[e.mate] * glm::inverse(fb);
  }
  return world;
}

// The closure error of one cut mate as a 6-vector: translation in mm, then
// rotation scaled BY a characteristic length so it is also in mm — mixing raw
// mm with raw radians would make "close enough" depend on the document's units
// and on how big the part is.
//
// Decoupled ([t; log R]) rather than a true SE(3) log ([V^-1 t; log R]): the
// two have the same zero set and the same rank structure there, which is all a
// root-find and a rank count use, and this one has no V^-1 edge cases.
void CutResidual(const LoopMate& m, const glm::dmat4& adjust,
                 const std::unordered_map<uint64_t, glm::dmat4>& world,
                 double charLength, double* out) {
  const auto ita = world.find(m.aOcc);
  const auto itb = world.find(m.bOcc);
  if (ita == world.end() || itb == world.end()) {
    for (int i = 0; i < 6; i++)
      out[i] = 0.0;
    return;
  }
  const glm::dmat4 expected = ita->second * glm::dmat4(m.fa) * adjust;
  const glm::dmat4 actual = itb->second * glm::dmat4(m.fb);
  const glm::dmat4 err = glm::inverse(expected) * actual;
  const glm::dvec3 t(err[3]);
  const glm::dvec3 w = LogSo3(glm::dmat3(err));
  out[0] = t.x;
  out[1] = t.y;
  out[2] = t.z;
  out[3] = w.x * charLength;
  out[4] = w.y * charLength;
  out[5] = w.z * charLength;
}

// The spread of the placed occurrences, floored at 1 mm. Computed once from
// the SEED placement and then held fixed: a characteristic length that moved
// with q would move the objective under the solver.
double
CharacteristicLength(const std::unordered_map<uint64_t, glm::dmat4>& world) {
  bool any = false;
  glm::dvec3 lo(0.0), hi(0.0);
  for (const auto& [id, m] : world) {
    (void)id;
    const glm::dvec3 t(m[3]);
    if (!any) {
      lo = hi = t;
      any = true;
    } else {
      lo = glm::min(lo, t);
      hi = glm::max(hi, t);
    }
  }
  return std::max(1.0, any ? glm::length(hi - lo) : 0.0);
}

std::vector<glm::dmat4> AdjustsFrom(const std::vector<LoopMate>& mates) {
  std::vector<glm::dmat4> a;
  a.reserve(mates.size());
  for (const LoopMate& m : mates)
    a.push_back(AdjustOfD(m));
  return a;
}

} // namespace

std::unordered_map<uint64_t, glm::dmat4>
EvaluateTree(const LoopProblem& problem, const std::vector<LoopMate>& mates) {
  std::vector<glm::dmat4> adjust;
  adjust.reserve(problem.mates.size());
  for (size_t i = 0; i < problem.mates.size(); i++)
    adjust.push_back(AdjustOfD(i < mates.size() ? mates[i] : problem.mates[i]));
  return WalkTree(problem, adjust);
}

#ifdef HAVE_EIGEN

namespace {

// The diagnosis's extra unknowns: one connector of mate `mate` may tilt, by
// the rotation vector (q[column], q[column + 1], 0) in that connector's own
// frame, about its own origin. Connector A's tilt is composed BEFORE the
// joint motion — a revolute then turns about the tilted axis, a slider
// slides along it — and connector B's AFTER it, where it tilts the part that
// rides the joint. Both are needed: a wrong frame on B cannot be expressed as
// one on A (for a revolute, only at a single joint angle; for a slider, not
// at all).
struct Tilt {
  int mate = -1; // index into LoopProblem::mates; -1 = no tilt (the solve)
  bool onB = false;
  int column = 0;
  // Out: the tilt reached (radians) and the joint columns' own mobility with
  // it held.
  double angle = 0.0;
  int jointDof = 0;
};

glm::dmat4 TiltAt(const std::vector<double>& q, int column) {
  const size_t c = static_cast<size_t>(column);
  return glm::mat4_cast(ExpSo3(glm::dvec3(q[c], q[c + 1], 0.0)));
}

// Solves ONE connected component. SolveLoops below partitions first, so this
// never sees unknowns that cannot influence its residuals. With `tilt` set it
// is the diagnosis's re-solve: two more columns, everything else identical.
LoopSolution SolveComponent(const LoopProblem& problem, Tilt* tilt = nullptr) {
  LoopSolution sol;
  if (problem.cut.empty())
    return sol; // loop-free: the caller keeps its closed-form path
  sol.ran = true;

  const Packing pack = PackDofs(problem.mates);
  const bool tilting = tilt != nullptr && tilt->mate >= 0;
  const int jointCols = pack.cols;
  const int cols = jointCols + (tilting ? 2 : 0);
  if (tilting)
    tilt->column = jointCols;
  // Relations whose DOFs are both unknowns here are rows; one that names a
  // DOF the mate lacks was refused at creation and is skipped, not guessed.
  struct RelationRow {
    int colA, colB;
    double ratio, phase;
    bool rotationB;
    std::vector<std::pair<double, double>> lift; // cam only (v88)
    // A relation over several mates (v171): (column, coefficient) per term,
    // the coefficients already divided by the largest.
    std::vector<std::pair<int, double>> terms;
  };
  std::vector<RelationRow> relRows;
  for (const LoopRelation& r : problem.relations) {
    if (!r.terms.empty()) {
      RelationRow row{-1, -1, 0.0, 0.0, true, {}, {}};
      double scale = 0.0;
      for (const LoopRelation::Term& t : r.terms)
        scale = std::max(scale, std::abs(t.coefficient));
      bool all = scale > 0.0;
      for (const LoopRelation::Term& t : r.terms) {
        const int c =
            DofColumn(problem.mates[t.mate].type, t.dof, pack.columnOf[t.mate]);
        if (c < 0) {
          all = false;
          break;
        }
        row.terms.emplace_back(c, t.coefficient / scale);
        // Weighed as a rotation (by the characteristic length) only when
        // every member turns; a mixed row stays in its own units.
        row.rotationB = row.rotationB && t.dof == MateDof::Rotation;
      }
      if (!all)
        continue;
      row.phase = r.phase / scale;
      relRows.push_back(std::move(row));
      continue;
    }
    const int ca =
        DofColumn(problem.mates[r.mateA].type, r.dofA, pack.columnOf[r.mateA]);
    const int cb =
        DofColumn(problem.mates[r.mateB].type, r.dofB, pack.columnOf[r.mateB]);
    if (ca < 0 || cb < 0)
      continue;
    relRows.push_back(
        {ca, cb, r.ratio, r.phase, r.dofB == MateDof::Rotation, r.lift});
  }
  // A pinned DOF is one more row: q[col] - value = 0. It is a row rather
  // than a removed column so an over-driven mechanism reports a residual
  // instead of silently refusing to move — the same treatment relations get.
  struct PinRow {
    int col;
    double value;
    bool rotation;
  };
  std::vector<PinRow> pinRows;
  for (const LoopPin& pin : problem.pinned) {
    if (pin.mate >= problem.mates.size())
      continue;
    const int col = DofColumn(problem.mates[pin.mate].type, pin.dof,
                              pack.columnOf[pin.mate]);
    if (col < 0)
      continue;
    pinRows.push_back({col, pin.value, pin.dof == MateDof::Rotation});
  }
  const int cutRows = static_cast<int>(problem.cut.size()) * 6;
  const int rows = cutRows + static_cast<int>(relRows.size()) +
                   static_cast<int>(pinRows.size());
  std::vector<double> q = SeedQ(problem.mates, pack);
  q.resize(static_cast<size_t>(cols), 0.0); // a tilt starts at zero

  const double charLen =
      CharacteristicLength(WalkTree(problem, AdjustsFrom(problem.mates)));

  std::unordered_map<uint64_t, glm::dmat4> world;
  const auto residualAt = [&](const std::vector<double>& qq, Eigen::VectorXd& r,
                              bool keepWorld) {
    std::vector<glm::dmat4> adjust(problem.mates.size());
    for (size_t i = 0; i < problem.mates.size(); i++)
      adjust[i] = AdjustAt(problem.mates[i], pack.columnOf[i], qq);
    if (tilting) {
      glm::dmat4& a = adjust[static_cast<size_t>(tilt->mate)];
      a = tilt->onB ? a * TiltAt(qq, tilt->column)
                    : TiltAt(qq, tilt->column) * a;
    }
    auto w = WalkTree(problem, adjust);
    r.resize(rows);
    for (size_t k = 0; k < problem.cut.size(); k++) {
      const size_t mi = problem.cut[k];
      double v[6];
      CutResidual(problem.mates[mi], adjust[mi], w, charLen, v);
      for (int i = 0; i < 6; i++)
        r[static_cast<int>(k) * 6 + i] = v[i];
    }
    // A relation row in the same units as the cut rows: a rotation residual
    // is scaled by the characteristic length, as the cut rows' rotational
    // part is, so radians and millimetres weigh alike in the least squares.
    for (size_t k = 0; k < relRows.size(); k++) {
      const RelationRow& rr = relRows[k];
      if (!rr.terms.empty()) {
        double v = -rr.phase;
        for (const auto& [col, c] : rr.terms)
          v += c * qq[col];
        r[cutRows + static_cast<int>(k)] = rr.rotationB ? v * charLen : v;
        continue;
      }
      // A cam reads its lift table where the others multiply by a ratio.
      // The residual is still "what B is, less what A says it should be",
      // and the numeric Jacobian differentiates the spline — which is why
      // the profile is interpolated smoothly rather than as a polyline.
      // A turning follower's table is in degrees, as RelationValue reads it.
      const double lift = rr.lift.empty() ? 0.0 : CamLift(rr.lift, qq[rr.colA]);
      const double want =
          rr.lift.empty()
              ? rr.ratio * qq[rr.colA] + rr.phase
              : (rr.rotationB ? angles::ToRadians(lift) : lift) + rr.phase;
      const double v = qq[rr.colB] - want;
      r[cutRows + static_cast<int>(k)] = rr.rotationB ? v * charLen : v;
    }
    // The pinned rows, in the same units as the rest.
    const int pinBase = cutRows + static_cast<int>(relRows.size());
    for (size_t k = 0; k < pinRows.size(); k++) {
      const PinRow& pr = pinRows[k];
      const double v = qq[pr.col] - pr.value;
      r[pinBase + static_cast<int>(k)] = pr.rotation ? v * charLen : v;
    }
    if (keepWorld)
      world = std::move(w);
  };

  Eigen::VectorXd r;
  residualAt(q, r, true);

  // Levenberg-Marquardt. LM rather than Gauss-Newton for the damping term,
  // which is also what will keep drag stable through a singular configuration
  // (a linkage at full extension) when that stage arrives.
  Eigen::MatrixXd jac(rows, std::max(cols, 1));
  const auto buildJacobian = [&](const std::vector<double>& qq,
                                 const Eigen::VectorXd& r0) {
    Eigen::VectorXd rh;
    for (int c = 0; c < cols; c++) {
      std::vector<double> qh = qq;
      const double h = 1e-7 * std::max(1.0, std::abs(qq[c]));
      qh[c] += h;
      residualAt(qh, rh, false);
      jac.col(c) = (rh - r0) / h;
    }
  };

  constexpr int kMaxIterations = 100;
  constexpr double kStepTol = 1e-12;
  constexpr double kConvergeMm = kCloseTolMm * 0.01;
  double lambda = 1e-3;
  for (int iter = 0; cols > 0 && iter < kMaxIterations; iter++) {
    sol.iterations = iter + 1;
    if (r.lpNorm<Eigen::Infinity>() < kConvergeMm)
      break;
    buildJacobian(q, r);
    const Eigen::MatrixXd jtj = jac.transpose() * jac;
    const Eigen::VectorXd jtr = jac.transpose() * r;
    // A DOF that changes NO residual is not being solved for, and must not
    // be moved. Marquardt's damping is RELATIVE — lambda * jtj(i,i) — so a
    // column of zeros gets a zero diagonal as well, and the solve hands that
    // unknown whatever satisfies 0 = 0.
    //
    // It is not a corner case: every bolt a fastener fits across an
    // interface is a mate, so a bolted joint between two parts that were
    // already mated closes a small loop, and everything hanging off that
    // component is then inside the loop problem. On the grill it was the
    // lid: its hinge is nowhere near the loop the handle's two bolts close,
    // and the lid came out standing 443 degrees open — with the loop closed
    // to a ten-thousandth of a millimetre and every mate "satisfied".
    // "No influence" is measured in the residual's own units: a column norm
    // is millimetres of loop error per unit of DOF (the rotation rows are
    // scaled by the characteristic length, so a radian and a millimetre
    // weigh alike). A DOF that cannot move the loop by the tolerance it has
    // to close to — 1e-3 mm — when moved a WHOLE unit is not solving
    // anything: a real linkage's columns are tens or hundreds of mm per
    // radian, and the grill's free hinges came back at 8e-7 to 3.4e-6,
    // which is what a finite difference through a metre-wide tree cancels
    // to rather than a derivative. A relative test cannot help here, since
    // on this assembly EVERY column was that noise.
    std::vector<bool> frozen(static_cast<size_t>(cols), false);
    int live = 0;
    for (int i = 0; i < cols; i++) {
      frozen[static_cast<size_t>(i)] =
          std::sqrt(std::max(0.0, jtj(i, i))) <= kCloseTolMm;
      live += frozen[static_cast<size_t>(i)] ? 0 : 1;
    }
    if (live == 0)
      break; // nothing here can move the residual; the poses stand
    bool stepped = false, tiny = false;
    for (int backoff = 0; backoff < 16; backoff++) {
      Eigen::MatrixXd damped = jtj;
      Eigen::VectorXd rhs = -jtr;
      for (int i = 0; i < cols; i++) {
        if (frozen[static_cast<size_t>(i)]) {
          damped.row(i).setZero();
          damped.col(i).setZero();
          damped(i, i) = 1.0;
          rhs[i] = 0.0;
          continue;
        }
        damped(i, i) += lambda * std::max(1e-12, jtj(i, i));
      }
      const Eigen::VectorXd step = damped.ldlt().solve(rhs);
      if (!step.allFinite()) {
        lambda *= 10.0;
        continue;
      }
      std::vector<double> qn = q;
      for (int i = 0; i < cols; i++)
        qn[i] += step[i];
      Eigen::VectorXd rn;
      residualAt(qn, rn, false);
      if (rn.squaredNorm() < r.squaredNorm()) {
        q = std::move(qn);
        r = std::move(rn);
        lambda = std::max(1e-12, lambda * 0.3);
        stepped = true;
        tiny = step.lpNorm<Eigen::Infinity>() < kStepTol;
        break;
      }
      lambda *= 10.0;
    }
    if (!stepped || tiny)
      break; // at a minimum, or no downhill step exists
  }

  residualAt(q, r, true);
  sol.residualMm = r.lpNorm<Eigen::Infinity>();
  sol.converged = sol.residualMm < kCloseTolMm;
  sol.solved = ApplyQ(problem.mates, pack, q);
  sol.world = world;

  // Which cut mates could not close, by their own residual block — and by
  // how much, each its own: the component's worst belongs to one of them.
  std::vector<bool> conflicts(problem.cut.size(), false);
  for (size_t k = 0; k < problem.cut.size(); k++) {
    double worst = 0.0;
    for (int i = 0; i < 6; i++)
      worst = std::max(worst, std::abs(r[static_cast<int>(k) * 6 + i]));
    if (worst > kCloseTolMm) {
      conflicts[k] = true;
      sol.conflicting.push_back(problem.mates[problem.cut[k]].id);
      sol.conflictingMm.push_back(worst);
    }
  }

  // Rank at the solution. DOF = cols - rank(J), and the ROWS that find no
  // pivot are the redundant ones — Ondsel's SingularMatrixError rows and
  // SolveSpace's CalculateRank, here as a column-pivoted QR of J^T so the
  // permutation runs over the rows of J.
  //
  // Grubler would be cheaper and wrong: a planar four-bar scores 6*3-5*4 = -2
  // ("over-constrained") when its mobility is 1, because three of the six loop
  // equations are identically zero and only a rank can see that.
  std::vector<int> dependentRows(problem.cut.size(), 0);
  if (cols == 0) {
    // Every cut mate adds rows and no unknowns — the three-bolts-through-a-
    // flange case. Rank is 0, so every row is dependent.
    sol.dof = 0;
    for (size_t k = 0; k < problem.cut.size(); k++)
      dependentRows[k] = 6;
  } else {
    buildJacobian(q, r);
    const Eigen::ColPivHouseholderQR<Eigen::MatrixXd> qr(jac.transpose());
    const int rank = static_cast<int>(qr.rank());
    sol.dof = cols - rank;
    const auto& perm = qr.colsPermutation().indices();
    for (int i = rank; i < static_cast<int>(perm.size()); i++)
      dependentRows[static_cast<size_t>(perm[i]) / 6]++;
    if (tilting) {
      // The mobility of the mechanism the tilt made, with the tilt HELD:
      // only the joint columns count.
      //
      // Relative threshold, not Eigen's default near machine epsilon: a tilt
      // that restores a planar linkage restores it only to the closure
      // tolerance, so its structurally-zero rows come back as finite-
      // difference noise around 1e-5 of the real pivots, and the default
      // counts that noise as rank.
      tilt->jointDof = 0;
      if (jointCols > 0) {
        Eigen::ColPivHouseholderQR<Eigen::MatrixXd> jq(jac.leftCols(jointCols));
        jq.setThreshold(1e-4);
        tilt->jointDof = jointCols - static_cast<int>(jq.rank());
      }
      const size_t c = static_cast<size_t>(tilt->column);
      tilt->angle = std::hypot(q[c], q[c + 1]);
    }
  }

  // A mate is REDUNDANT only when ALL SIX of its rows are dependent: remove it
  // and nothing about the assembly changes. Some dependent rows is the normal
  // case and says nothing — a planar four-bar's closing joint has three
  // structurally-zero rows (no z, no tilt) while being the very thing that
  // makes it a linkage, and reporting that one as redundant would be exactly
  // backwards.
  for (size_t k = 0; k < problem.cut.size(); k++) {
    if (conflicts[k] || dependentRows[k] < 6)
      continue; // did not close => a conflict, never a redundancy
    const uint64_t id = problem.mates[problem.cut[k]].id;
    if (std::find(sol.redundant.begin(), sol.redundant.end(), id) ==
        sol.redundant.end())
      sol.redundant.push_back(id);
  }

  return sol;
}

// Connected components of the mate graph, each as a standalone problem, with
// a map back to the parent's mate indices.
struct Component {
  LoopProblem sub;
  std::vector<size_t> mateIndex; // sub index -> index in the parent
};

std::vector<Component> SplitIntoComponents(const LoopProblem& p) {
  // Union-find over occurrence ids; every mate joins its two sides.
  std::unordered_map<uint64_t, uint64_t> parent;
  const std::function<uint64_t(uint64_t)> find = [&](uint64_t x) -> uint64_t {
    auto it = parent.find(x);
    if (it == parent.end() || it->second == x)
      return x;
    const uint64_t root = find(it->second);
    parent[x] = root; // path compression
    return root;
  };
  const auto unite = [&](uint64_t a, uint64_t b) {
    parent.emplace(a, a);
    parent.emplace(b, b);
    const uint64_t ra = find(a), rb = find(b);
    if (ra != rb)
      parent[ra] = rb;
  };
  for (const LoopMate& m : p.mates)
    unite(m.aOcc, m.bOcc);

  std::unordered_map<uint64_t, size_t> slot; // component root -> index
  std::vector<Component> out;
  const auto slotFor = [&](uint64_t occ) -> size_t {
    const uint64_t root = find(occ);
    const auto it = slot.find(root);
    if (it != slot.end())
      return it->second;
    slot[root] = out.size();
    out.emplace_back();
    return out.size() - 1;
  };

  std::vector<size_t> subIndexOf(p.mates.size(), 0);
  for (size_t i = 0; i < p.mates.size(); i++) {
    Component& c = out[slotFor(p.mates[i].aOcc)];
    subIndexOf[i] = c.sub.mates.size();
    c.sub.mates.push_back(p.mates[i]);
    c.mateIndex.push_back(i);
  }
  for (const LoopTreeEdge& e : p.tree) {
    Component& c = out[slotFor(e.from)];
    c.sub.tree.push_back({subIndexOf[e.mate], e.from, e.to, e.reversed});
  }
  for (size_t i : p.cut)
    out[slotFor(p.mates[i].aOcc)].sub.cut.push_back(subIndexOf[i]);
  for (const auto& [occ, at] : p.anchor)
    out[slotFor(occ)].sub.anchor.emplace_back(occ, at);
  // A relation is a row only where both its mates live in ONE component;
  // across components it is propagation's business (AssemblySolve.cpp).
  for (const LoopPin& pin : p.pinned) {
    if (pin.mate >= p.mates.size())
      continue;
    Component& c = out[slotFor(p.mates[pin.mate].aOcc)];
    c.sub.pinned.push_back({subIndexOf[pin.mate], pin.dof, pin.value});
  }
  for (const LoopRelation& r : p.relations) {
    if (!r.terms.empty()) {
      // Over several mates: a row only when EVERY member is in this one
      // component, the same rule as two.
      const size_t s0 = slotFor(p.mates[r.terms.front().mate].aOcc);
      bool one = true;
      for (const LoopRelation::Term& t : r.terms)
        one = one && slotFor(p.mates[t.mate].aOcc) == s0;
      if (!one)
        continue;
      LoopRelation sub = r;
      for (LoopRelation::Term& t : sub.terms)
        t.mate = subIndexOf[t.mate];
      out[s0].sub.relations.push_back(std::move(sub));
      continue;
    }
    const size_t sa = slotFor(p.mates[r.mateA].aOcc);
    const size_t sb = slotFor(p.mates[r.mateB].aOcc);
    if (sa != sb)
      continue;
    LoopRelation sub = r;
    sub.mateA = subIndexOf[r.mateA];
    sub.mateB = subIndexOf[r.mateB];
    out[sa].sub.relations.push_back(sub);
  }
  return out;
}

bool CarriesAxis(MateType type) {
  return type == MateType::Revolute || type == MateType::Cylindrical ||
         type == MateType::Slider;
}

// The diagnosis proper, for a component whose plain solve left `conflicting`.
LoopDiagnosis Diagnose(const LoopProblem& problem,
                       const std::vector<uint64_t>& conflicting) {
  LoopDiagnosis d;
  d.conflicting = conflicting;
  if (conflicting.empty())
    return d; // it closes: nothing to explain
  for (size_t i = 0; i < problem.mates.size(); i++) {
    if (!CarriesAxis(problem.mates[i].type))
      continue;
    for (const bool onB : {false, true}) {
      Tilt tilt;
      tilt.mate = static_cast<int>(i);
      tilt.onB = onB;
      const LoopSolution s = SolveComponent(problem, &tilt);
      LoopTiltTrial t;
      t.mate = problem.mates[i].id;
      t.connector = onB ? 'b' : 'a';
      t.tiltRad = tilt.angle;
      t.residualMm = s.residualMm;
      t.dof = tilt.jointDof;
      t.closes = s.residualMm < kCloseTolMm;
      d.trials.push_back(t);
    }
  }
  // Closing is not enough to accuse a mate. A loop with a tilt allowed at
  // one joint has as many unknowns as it has equations, so a tilt at almost
  // ANY joint downstream of the fault closes it — at one isolated pose, as a
  // rigid spatial linkage. Only a tilt that undoes the fault gives back the
  // mechanism's mobility (a planar four-bar's one DOF), so the suspects are
  // the closing trials of the HIGHEST mobility, and a tie is reported as a
  // tie.
  int best = -1;
  for (const LoopTiltTrial& t : d.trials)
    if (t.closes)
      best = std::max(best, t.dof);
  for (const LoopTiltTrial& t : d.trials) {
    if (!t.closes || t.dof != best)
      continue;
    auto same =
        std::find_if(d.suspects.begin(), d.suspects.end(),
                     [&](const LoopTiltTrial& s) { return s.mate == t.mate; });
    if (same == d.suspects.end())
      d.suspects.push_back(t);
    else
      same->connector = 0; // both sides do: which one is wrong is unknown
  }
  return d;
}

} // namespace

LoopDiagnosis DiagnoseLoop(const LoopProblem& problem) {
  return Diagnose(problem, SolveComponent(problem).conflicting);
}

LoopSolution SolveLoops(const LoopProblem& problem) {
  LoopSolution sol;
  if (problem.cut.empty())
    return sol;

  // Partition first. Without it, one small loop in a large assembly still
  // builds a Jacobian with a column per mate in the WHOLE document, and each
  // column costs a full tree walk — quadratic in the size of the model for a
  // loop that involves four parts. Independent loops also stay independent:
  // ten of them are ten small solves, not one system ten times as wide.
  sol.solved = problem.mates;
  for (const Component& c : SplitIntoComponents(problem)) {
    if (c.sub.cut.empty())
      continue; // a mate TREE: the caller's closed-form placement stands
    const LoopSolution part = SolveComponent(c.sub);
    if (!part.unavailable.empty())
      return part;
    sol.ran = true;
    sol.iterations = std::max(sol.iterations, part.iterations);
    sol.dof += part.dof;
    sol.residualMm = std::max(sol.residualMm, part.residualMm);
    sol.converged = sol.ran && (sol.conflicting.empty() && part.converged &&
                                (sol.residualMm < kCloseTolMm));
    for (uint64_t id : part.redundant)
      sol.redundant.push_back(id);
    for (uint64_t id : part.conflicting)
      sol.conflicting.push_back(id);
    for (double mm : part.conflictingMm)
      sol.conflictingMm.push_back(mm);
    if (problem.diagnose && !part.conflicting.empty())
      sol.diagnoses.push_back(Diagnose(c.sub, part.conflicting));
    for (size_t i = 0; i < part.solved.size(); i++)
      sol.solved[c.mateIndex[i]] = part.solved[i];
    for (const auto& [id, m] : part.world)
      sol.world[id] = m;
  }
  sol.converged = sol.conflicting.empty() && sol.residualMm < kCloseTolMm;
  return sol;
}

#else

LoopSolution SolveLoops(const LoopProblem& problem) {
  LoopSolution sol;
  if (problem.cut.empty())
    return sol;
  sol.ran = true;
  sol.unavailable =
      "this build has no Eigen, so mate loops are reported, not solved";
  return sol;
}

LoopDiagnosis DiagnoseLoop(const LoopProblem&) { return {}; }

#endif

} // namespace assembly
