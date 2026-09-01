#include "core/AssemblyLoopSolve.h"

#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>

#ifdef HAVE_EIGEN
#include <Eigen/Dense>
#endif

namespace assembly {

namespace {

// THE definition of a mate's adjustment, once, at whatever precision the
// caller needs. The placement pass wants float (its transforms are float and
// its results are golden-tested); the loop solver wants double, because
// accumulating a chain of float matrices leaves ~1e-3 mm of noise at
// 100 mm scale — enough to swamp a finite-difference Jacobian and stall the
// solve just short of closing.
template <typename T>
glm::mat<4, 4, T> MateAdjustT(bool flipped, T rotation, T offset, T dx, T dy,
                              const glm::qua<T>& ballRotation) {
  glm::mat<4, 4, T> a(T(1));
  a = glm::translate(a, glm::vec<3, T>(dx, dy, offset));
  a = glm::rotate(a, rotation, glm::vec<3, T>(T(0), T(0), T(1)));
  a *= glm::mat4_cast(ballRotation);
  if (flipped)
    a = glm::rotate(a, glm::pi<T>(), glm::vec<3, T>(T(1), T(0), T(0)));
  return a;
}

} // namespace

glm::mat4 MateAdjustFor(bool flipped, float rotation, float offset, float dx,
                        float dy, const glm::quat& ballRotation) {
  return MateAdjustT<float>(flipped, rotation, offset, dx, dy, ballRotation);
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
  return MateAdjustT<double>(m.flipped, m.rotation, m.offset, m.dx, m.dy,
                             glm::dquat(m.ballRotation));
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

// Mate i's adjustment at the given q, in double, WITHOUT round-tripping
// through the float fields of LoopMate — that narrowing is what the solver
// must not do until it is finished.
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
    return MateAdjustT<double>(seed.flipped, q[col], seed.offset, seed.dx,
                               seed.dy, glm::dquat(seed.ballRotation));
  case MateType::Slider:
    return MateAdjustT<double>(seed.flipped, seed.rotation, q[col], seed.dx,
                               seed.dy, glm::dquat(seed.ballRotation));
  case MateType::Cylindrical:
    return MateAdjustT<double>(seed.flipped, q[col], q[col + 1], seed.dx,
                               seed.dy, glm::dquat(seed.ballRotation));
  case MateType::Planar:
    return MateAdjustT<double>(seed.flipped, q[col + 2], seed.offset, q[col],
                               q[col + 1], glm::dquat(seed.ballRotation));
  case MateType::Ball:
    return MateAdjustT<double>(
        seed.flipped, seed.rotation, seed.offset, seed.dx, seed.dy,
        glm::normalize(glm::dquat(seed.ballRotation) *
                       ExpSo3(glm::dvec3(q[col], q[col + 1], q[col + 2]))));
  case MateType::Fastened:
    break;
  }
  return AdjustOfD(seed);
}

// Narrowed back into LoopMate fields — done ONCE, at the end, because the
// document stores these as floats.
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
      m.rotation = static_cast<float>(q[c]);
      break;
    case MateType::Slider:
      m.offset = static_cast<float>(q[c]);
      break;
    case MateType::Cylindrical:
      m.rotation = static_cast<float>(q[c]);
      m.offset = static_cast<float>(q[c + 1]);
      break;
    case MateType::Planar:
      m.dx = static_cast<float>(q[c]);
      m.dy = static_cast<float>(q[c + 1]);
      m.rotation = static_cast<float>(q[c + 2]);
      break;
    case MateType::Ball:
      m.ballRotation = glm::quat(
          glm::normalize(glm::dquat(seed[i].ballRotation) *
                         ExpSo3(glm::dvec3(q[c], q[c + 1], q[c + 2]))));
      break;
    case MateType::Fastened:
      break;
    }
  }
  return out;
}

// ONE tree walk, in double, driven by per-mate adjustments the caller chose.
// Both the public float EvaluateTree and every residual evaluation inside the
// solver go through here, so there is no second copy of the composition to
// drift.
std::unordered_map<uint64_t, glm::dmat4>
WalkTree(const LoopProblem& problem, const std::vector<glm::dmat4>& adjust) {
  std::unordered_map<uint64_t, glm::dmat4> world;
  for (const auto& [id, m] : problem.anchor)
    world[id] = glm::dmat4(m);
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

std::unordered_map<uint64_t, glm::mat4>
EvaluateTree(const LoopProblem& problem, const std::vector<LoopMate>& mates) {
  std::vector<glm::dmat4> adjust;
  adjust.reserve(problem.mates.size());
  for (size_t i = 0; i < problem.mates.size(); i++)
    adjust.push_back(AdjustOfD(i < mates.size() ? mates[i] : problem.mates[i]));
  const auto world = WalkTree(problem, adjust);
  std::unordered_map<uint64_t, glm::mat4> out;
  out.reserve(world.size());
  for (const auto& [id, m] : world)
    out[id] = glm::mat4(m);
  return out;
}

#ifdef HAVE_EIGEN

LoopSolution SolveLoops(const LoopProblem& problem) {
  LoopSolution sol;
  if (problem.cut.empty())
    return sol; // loop-free: the caller keeps its closed-form path
  sol.ran = true;

  const Packing pack = PackDofs(problem.mates);
  const int cols = pack.cols;
  const int rows = static_cast<int>(problem.cut.size()) * 6;
  std::vector<double> q = SeedQ(problem.mates, pack);

  const double charLen =
      CharacteristicLength(WalkTree(problem, AdjustsFrom(problem.mates)));

  std::unordered_map<uint64_t, glm::dmat4> world;
  const auto residualAt = [&](const std::vector<double>& qq, Eigen::VectorXd& r,
                              bool keepWorld) {
    std::vector<glm::dmat4> adjust(problem.mates.size());
    for (size_t i = 0; i < problem.mates.size(); i++)
      adjust[i] = AdjustAt(problem.mates[i], pack.columnOf[i], qq);
    auto w = WalkTree(problem, adjust);
    r.resize(rows);
    for (size_t k = 0; k < problem.cut.size(); k++) {
      const size_t mi = problem.cut[k];
      double v[6];
      CutResidual(problem.mates[mi], adjust[mi], w, charLen, v);
      for (int i = 0; i < 6; i++)
        r[static_cast<int>(k) * 6 + i] = v[i];
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
    bool stepped = false, tiny = false;
    for (int backoff = 0; backoff < 16; backoff++) {
      Eigen::MatrixXd damped = jtj;
      for (int i = 0; i < cols; i++)
        damped(i, i) += lambda * std::max(1e-12, jtj(i, i));
      const Eigen::VectorXd step = damped.ldlt().solve(-jtr);
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
  sol.world.reserve(world.size());
  for (const auto& [id, m] : world)
    sol.world[id] = glm::mat4(m);

  // Which cut mates could not close, by their own residual block.
  std::vector<bool> conflicts(problem.cut.size(), false);
  for (size_t k = 0; k < problem.cut.size(); k++) {
    double worst = 0.0;
    for (int i = 0; i < 6; i++)
      worst = std::max(worst, std::abs(r[static_cast<int>(k) * 6 + i]));
    if (worst > kCloseTolMm) {
      conflicts[k] = true;
      sol.conflicting.push_back(problem.mates[problem.cut[k]].id);
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

#endif

} // namespace assembly
