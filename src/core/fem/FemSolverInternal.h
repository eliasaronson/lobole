// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// Internal (non-public) shared numerics for the Fem* solver translation units
// (FemSolver.cpp, FemTopo.cpp) and the GPU-solver gate test (test_femgpu.cpp,
// which packs element matrices for fem_gpu). Only those TUs include this;
// consumers use core/FemSolver.h / core/FemTopo.h. Everything here is small
// element-level math (shape functions, Gauss rules, the elasticity matrix) plus
// the two system-building helpers (DOF reduction, consistent face loads) the
// topology loop reuses verbatim — moved out of FemSolver.cpp's anonymous
// namespace so the SIMP loop composes them without forking the assembly math.
// Include only under HAVE_FEM_SOLVER (it needs Eigen).

#include "core/fem/FemMesher.h"
#include "core/fem/FemSolver.h"

#include <Eigen/Dense>

#include <Eigen/Sparse>

#include <cmath>
#include <functional>
#include <string>
#include <vector>

namespace fem {
namespace internal {

using Eigen::Vector3d;

// Natural (barycentric-derived) coordinates of the 10 tet nodes, matching the
// canonical ordering in FemMesh: corners then edges
// (0,1)(1,2)(0,2)(0,3)(1,3)(2,3).
inline constexpr double kNodeRST[10][3] = {
    {0, 0, 0},   {1, 0, 0},     {0, 1, 0},    {0, 0, 1}, // corners
    {0.5, 0, 0}, {0.5, 0.5, 0}, {0, 0.5, 0},             // 4,5,6
    {0, 0, 0.5}, {0.5, 0, 0.5}, {0, 0.5, 0.5}};          // 7,8,9

// dN/d(r,s,t) for the tet10 shape functions at (r,s,t). L0=1-r-s-t, L1=r,
// L2=s, L3=t; corner Ni = Li(2Li-1), edge N = 4 La Lb.
inline void TetShapeGrad(double r, double s, double t, double dN[10][3]) {
  const double L[4] = {1 - r - s - t, r, s, t};
  // dL/d(r,s,t): L0 = 1-r-s-t; L1=r; L2=s; L3=t.
  const double dL[4][3] = {{-1, -1, -1}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
  // Corners.
  for (int i = 0; i < 4; ++i) {
    const double f = 4 * L[i] - 1; // d/dL of L(2L-1) = 4L-1
    for (int k = 0; k < 3; ++k)
      dN[i][k] = f * dL[i][k];
  }
  // Edges: node index -> (a,b) corner pair.
  const int edge[6][2] = {{0, 1}, {1, 2}, {0, 2}, {0, 3}, {1, 3}, {2, 3}};
  for (int e = 0; e < 6; ++e) {
    const int a = edge[e][0], b = edge[e][1];
    for (int k = 0; k < 3; ++k)
      dN[4 + e][k] = 4 * (dL[a][k] * L[b] + L[a] * dL[b][k]);
  }
}

// tet10 shape function VALUES N[10] at (r,s,t). Same convention as
// TetShapeGrad: corner Ni = Li(2Li-1), edge N = 4 La Lb. For the mass matrix
// and body (gravity) loads.
inline void TetShape(double r, double s, double t, double N[10]) {
  const double L[4] = {1 - r - s - t, r, s, t};
  for (int i = 0; i < 4; ++i)
    N[i] = L[i] * (2 * L[i] - 1);
  const int edge[6][2] = {{0, 1}, {1, 2}, {0, 2}, {0, 3}, {1, 3}, {2, 3}};
  for (int e = 0; e < 6; ++e)
    N[4 + e] = 4 * L[edge[e][0]] * L[edge[e][1]];
}

// The 6x30 strain-displacement matrix B and detJ at natural point (r,s,t) for
// an element whose 10 node coordinates are `X`. Voigt order
// [xx,yy,zz,xy,yz,zx].
inline double StrainDisp(const Vector3d X[10], double r, double s, double t,
                         Eigen::Matrix<double, 6, 30>& B) {
  double dN[10][3];
  TetShapeGrad(r, s, t, dN);
  // Jacobian J_{jk} = dX_j/dcoord_k = sum_i dN_i/dcoord_k * X_i[j].
  Eigen::Matrix3d J = Eigen::Matrix3d::Zero();
  for (int i = 0; i < 10; ++i)
    for (int j = 0; j < 3; ++j)
      for (int k = 0; k < 3; ++k)
        J(j, k) += dN[i][k] * X[i](j);
  const double detJ = J.determinant();
  const Eigen::Matrix3d invJ = J.inverse();

  B.setZero();
  for (int i = 0; i < 10; ++i) {
    // grad_x N_i = J^-T * grad_rst N_i.
    const Vector3d gr(dN[i][0], dN[i][1], dN[i][2]);
    const Vector3d g = invJ.transpose() * gr; // [dN/dx, dN/dy, dN/dz]
    const int c = 3 * i;
    B(0, c + 0) = g(0);
    B(1, c + 1) = g(1);
    B(2, c + 2) = g(2);
    B(3, c + 0) = g(1);
    B(3, c + 1) = g(0); // xy
    B(4, c + 1) = g(2);
    B(4, c + 2) = g(1); // yz
    B(5, c + 0) = g(2);
    B(5, c + 2) = g(0); // zx
  }
  return detJ;
}

// Isotropic elasticity matrix (6x6, Voigt).
inline Eigen::Matrix<double, 6, 6> Elasticity(double E, double nu) {
  Eigen::Matrix<double, 6, 6> D = Eigen::Matrix<double, 6, 6>::Zero();
  const double lam = E * nu / ((1 + nu) * (1 - 2 * nu));
  const double mu = E / (2 * (1 + nu));
  for (int i = 0; i < 3; ++i) {
    for (int j = 0; j < 3; ++j)
      D(i, j) = lam;
    D(i, i) = lam + 2 * mu;
    D(3 + i, 3 + i) = mu;
  }
  return D;
}

// tri6 shape functions N[6] at area coords (u,v); corners then edges
// (0,1)(1,2)(2,0), matching FemMesh::BoundaryTri.
inline void TriShape(double u, double v, double N[6]) {
  const double L[3] = {1 - u - v, u, v};
  N[0] = L[0] * (2 * L[0] - 1);
  N[1] = L[1] * (2 * L[1] - 1);
  N[2] = L[2] * (2 * L[2] - 1);
  N[3] = 4 * L[0] * L[1];
  N[4] = 4 * L[1] * L[2];
  N[5] = 4 * L[2] * L[0];
}

// d(x)/d(u,v) tangents of a tri6 facet at (u,v): needed for the surface area
// element and normal. Returns the two tangent vectors.
inline void TriTangents(const Vector3d P[6], double u, double v, Vector3d& du,
                        Vector3d& dv) {
  const double L[3] = {1 - u - v, u, v};
  // dL/du = (-1, 1, 0); dL/dv = (-1, 0, 1).
  const double dLdu[3] = {-1, 1, 0}, dLdv[3] = {-1, 0, 1};
  double dNdu[6], dNdv[6];
  for (int i = 0; i < 3; ++i) {
    const double f = 4 * L[i] - 1;
    dNdu[i] = f * dLdu[i];
    dNdv[i] = f * dLdv[i];
  }
  const int e[3][2] = {{0, 1}, {1, 2}, {2, 0}};
  for (int k = 0; k < 3; ++k) {
    const int a = e[k][0], b = e[k][1];
    dNdu[3 + k] = 4 * (dLdu[a] * L[b] + L[a] * dLdu[b]);
    dNdv[3 + k] = 4 * (dLdv[a] * L[b] + L[a] * dLdv[b]);
  }
  du = dv = Vector3d::Zero();
  for (int i = 0; i < 6; ++i) {
    du += dNdu[i] * P[i];
    dv += dNdv[i] * P[i];
  }
}

// Von Mises from a Voigt stress [xx,yy,zz,xy,yz,zx].
inline double VonMises(const Eigen::Matrix<double, 6, 1>& s) {
  const double a = s(0) - s(1), b = s(1) - s(2), c = s(2) - s(0);
  return std::sqrt(0.5 * (a * a + b * b + c * c) +
                   3 * (s(3) * s(3) + s(4) * s(4) + s(5) * s(5)));
}

// 4-point tet Gauss rule (natural coords + weight folded with the 1/6 ref
// volume): each point has one barycentric = A, three = B.
inline constexpr double kGA = 0.5854101966249685;
inline constexpr double kGB = 0.1381966011250105;
inline constexpr double kTetGauss[4][3] = {
    {kGB, kGB, kGB}, {kGA, kGB, kGB}, {kGB, kGA, kGB}, {kGB, kGB, kGA}};
inline constexpr double kTetW = 1.0 / 24.0; // (1/4 weight) * (1/6 ref volume)

// 3-point triangle Gauss rule (degree 2): weight 1/3, ref area 1/2.
inline constexpr double kTriGauss[3][2] = {
    {1.0 / 6, 1.0 / 6}, {2.0 / 3, 1.0 / 6}, {1.0 / 6, 2.0 / 3}};
inline constexpr double kTriW = (1.0 / 3) * (1.0 / 2);

// Preconditioned conjugate gradients on an assembled K (SolveCases, and the
// heat solve's scalar K_T), written out rather
// than Eigen's ConjugateGradient so it can be cancelled mid-solve (Eigen's
// would have to be restarted in chunks, losing its search directions each
// time) and so its progress is judged by the measure CG minimises.
//
// Converged at |K u - f| <= tol |f|. Starts from `u` when it is f's size
// (a warm start: what another solver reached), else from zero.
//
// Progress is the potential energy Pi(u) = u.K.u/2 - f.u, which every step
// lowers by exactly alpha (r.z)/2. The residual's 2-norm is NOT CG's
// measure and is not monotone: on a thin plate in bending it climbs to 20
// times |f| over the first thousand iterations of a solve that converges
// (docs/simulation/README.md#the-solver). So:
//  - Stalled: the last kStallIterations together lowered Pi by less than
//    fp64 can tell from rounding (1e-13 of |Pi|): the solve has reached the
//    floor of what it can do, short of tol;
//  - BrokeDown: a non-finite residual, or a direction with p.K.p <= 0,
//    which an SPD K and preconditioner cannot give;
//  - Capped: maxIterations.
// None of these says K is singular: a free rigid motion is found by
// FreeRigidMotion before a solve, from the supports, not guessed from a
// residual.
enum class PcgOutcome { Converged, Cancelled, Stalled, BrokeDown, Capped };

// What an outcome that is not Converged or Cancelled reads as, for an error.
inline std::string PcgFailure(PcgOutcome o, int iterations, double rel) {
  const std::string at = "a relative residual of " + std::to_string(rel) +
                         " after " + std::to_string(iterations) + " iterations";
  switch (o) {
  case PcgOutcome::Stalled:
    return "the iterative solve stopped gaining (its energy no longer "
           "falls) at " +
           at;
  case PcgOutcome::BrokeDown:
    return "the iterative solve broke down (a non-finite step) at " + at;
  case PcgOutcome::Capped:
    return "the iterative solve did not converge: " + at;
  default:
    return "the iterative solve failed at " + at;
  }
}

template <class Precond>
inline PcgOutcome Pcg(const Eigen::SparseMatrix<double>& K, const Precond& M,
                      const Eigen::VectorXd& f, Eigen::VectorXd& u, double tol,
                      int maxIterations, const std::function<bool()>& cancelled,
                      int* iterations, double* relResidual) {
  constexpr int kCancelEvery = 32;
  constexpr int kStallIterations = 2000;
  const double fNorm = f.norm();
  *iterations = 0;
  *relResidual = 0.0;
  if (fNorm == 0.0) {
    u = Eigen::VectorXd::Zero(f.size());
    return PcgOutcome::Converged;
  }
  Eigen::VectorXd r;
  double energy = 0.0; // Pi(u)
  if (u.size() == f.size() && u.allFinite()) {
    const Eigen::VectorXd Ku = K * u;
    r = f - Ku;
    energy = 0.5 * u.dot(Ku) - f.dot(u);
    // A start worse than zero is no start.
    if (!(energy < 0.0)) {
      u = Eigen::VectorXd::Zero(f.size());
      r = f;
      energy = 0.0;
    }
  } else {
    u = Eigen::VectorXd::Zero(f.size());
    r = f;
  }
  double rel = r.norm() / fNorm;
  *relResidual = rel;
  if (rel <= tol)
    return PcgOutcome::Converged;
  Eigen::VectorXd z = M.solve(r);
  Eigen::VectorXd p = z;
  Eigen::VectorXd Kp(f.size());
  double rz = r.dot(z);
  // The energy gained over the current window, and where the window began.
  std::vector<double> gains(kStallIterations, 0.0);
  double windowGain = 0.0;
  for (int it = 1; it <= maxIterations; ++it) {
    Kp.noalias() = K * p;
    const double pKp = p.dot(Kp);
    if (!(pKp > 0.0) || !std::isfinite(pKp))
      return PcgOutcome::BrokeDown;
    const double alpha = rz / pKp;
    u.noalias() += alpha * p;
    r.noalias() -= alpha * Kp;
    rel = r.norm() / fNorm;
    *iterations = it;
    *relResidual = rel;
    if (!std::isfinite(rel))
      return PcgOutcome::BrokeDown;
    if (rel <= tol)
      return PcgOutcome::Converged;
    const double gain = 0.5 * alpha * rz;
    energy -= gain;
    double& slot = gains[it % kStallIterations];
    windowGain += gain - slot;
    slot = gain;
    if (it >= kStallIterations && !(windowGain > 1e-13 * std::abs(energy)))
      return PcgOutcome::Stalled;
    if (cancelled && it % kCancelEvery == 0 && cancelled())
      return PcgOutcome::Cancelled;
    z = M.solve(r);
    const double rzNext = r.dot(z);
    p = z + (rzNext / rz) * p;
    rz = rzNext;
  }
  return PcgOutcome::Capped;
}

// ---- System-building helpers shared by Solve and the topology loop --------

// The reduced-DOF map: map[3*node+k] = compacted free-DOF index, or -1 when
// that DOF is fixed (all three DOF of every node in fixedNodes are zeroed).
struct DofMap {
  std::vector<int> map;
  int nFree = 0;
};

inline DofMap BuildDofMap(int nNodes, const std::vector<int>& fixedNodes) {
  std::vector<char> fixed(3 * nNodes, 0);
  for (int n : fixedNodes)
    if (n >= 0 && n < nNodes)
      for (int k = 0; k < 3; ++k)
        fixed[3 * n + k] = 1;
  DofMap out;
  out.map.assign(3 * nNodes, -1);
  for (int d = 0; d < 3 * nNodes; ++d)
    if (!fixed[d])
      out.map[d] = out.nFree++;
  return out;
}

// The integrals over a load's triangles that its traction field needs, at
// the same Gauss points the traction is integrated at, so the totals come
// back exact: area, centroid, the polar second moment J about an axis, and
// the area "inertia" tensor about the centroid (sum |r|^2 E - r r^T).
struct FaceIntegrals {
  double area = 0.0;
  Vector3d centroid = Vector3d::Zero();
  double polar = 0.0; // J about (origin, axis), mm^4
  Eigen::Matrix3d inertia = Eigen::Matrix3d::Zero();
};

template <typename Fn>
inline void ForEachFaceGaussPoint(const FemMesh& mesh,
                                  const std::vector<int>& tris, Fn&& fn) {
  for (int ti : tris) {
    const auto& tri = mesh.boundary[ti];
    Vector3d P[6];
    for (int i = 0; i < 6; ++i) {
      const auto& p = mesh.nodes[tri.nodes[i]];
      P[i] = Vector3d(p.x, p.y, p.z);
    }
    for (const auto& q : kTriGauss) {
      double N[6];
      TriShape(q[0], q[1], N);
      Vector3d du, dv;
      TriTangents(P, q[0], q[1], du, dv);
      const Vector3d cross = du.cross(dv);
      Vector3d x = Vector3d::Zero();
      for (int i = 0; i < 6; ++i)
        x += N[i] * P[i];
      fn(tri, N, x, cross, kTriW * cross.norm());
    }
  }
}

inline FaceIntegrals IntegrateFace(const FemMesh& mesh,
                                   const std::vector<int>& tris,
                                   const Vector3d& origin,
                                   const Vector3d& axis) {
  FaceIntegrals out;
  Vector3d first = Vector3d::Zero();
  ForEachFaceGaussPoint(mesh, tris,
                        [&](const auto&, const double*, const Vector3d& x,
                            const Vector3d&, double dA) {
                          out.area += dA;
                          first += dA * x;
                          const Vector3d r = x - origin;
                          const Vector3d perp = r - r.dot(axis) * axis;
                          out.polar += dA * perp.squaredNorm();
                        });
  if (out.area <= 0)
    return out;
  out.centroid = first / out.area;
  ForEachFaceGaussPoint(mesh, tris,
                        [&](const auto&, const double*, const Vector3d& x,
                            const Vector3d&, double dA) {
                          const Vector3d r = x - out.centroid;
                          out.inertia += dA * (r.squaredNorm() *
                                                   Eigen::Matrix3d::Identity() -
                                               r * r.transpose());
                        });
  return out;
}

// Consistent nodal loads for the face tractions, accumulated into the
// reduced-system vector F (sized nFree, zero-initialized by the caller):
//   Force    uniform traction F/A, integrating back to exactly the total;
//   Pressure along the inward normal, positive = pushing;
//   Torque   t = (T/J) a x (x - o), J the polar moment of the face about the
//            axis: exactly T about it, and no net force on a face symmetric
//            about it;
//   Remote   t = F/A + (I^-1 M) x (x - c), M = (p - c) x F, I the face's area
//            inertia about its centroid c: the force F at the point p, the
//            way an RBE3 spreads it.
// Returns false + *error on a face meshed to zero area, a torque with no
// axis or no polar moment, or a remote load whose face cannot carry a
// moment (its nodes on a line).
inline bool AssembleFaceLoads(const FemMesh& mesh,
                              const std::vector<FaceLoad>& loads,
                              const std::vector<int>& dofMap,
                              Eigen::VectorXd& F, std::string* error) {
  auto addNodal = [&](int node, const Vector3d& fv) {
    for (int k = 0; k < 3; ++k) {
      const int g = dofMap[3 * node + k];
      if (g >= 0)
        F(g) += fv(k);
    }
  };
  auto fail = [&](const std::string& m) {
    if (error)
      *error = m;
    return false;
  };
  const auto vec = [](const glm::dvec3& v) { return Vector3d(v.x, v.y, v.z); };
  for (const FaceLoad& load : loads) {
    const Vector3d dir = vec(load.direction).squaredNorm() > 0
                             ? vec(load.direction).normalized()
                             : Vector3d(0, 0, 0);
    const Vector3d axis = vec(load.axis).squaredNorm() > 0
                              ? vec(load.axis).normalized()
                              : Vector3d(0, 0, 0);
    const Vector3d origin = vec(load.origin);
    FaceIntegrals in;
    if (load.type != LoadType::Pressure) {
      in = IntegrateFace(mesh, load.tris, origin, axis);
      if (in.area <= 0)
        return fail(std::string(LoadTypeToString(load.type)) +
                    " face meshed to zero area");
    }
    Vector3d omega = Vector3d::Zero(); // Remote: the moment's traction rate
    if (load.type == LoadType::Torque) {
      if (axis.squaredNorm() == 0)
        return fail("a torque has no axis to turn about");
      if (!(in.polar > 1e-12 * in.area * in.area))
        return fail("a torque's face has no polar moment about its axis "
                    "(the face lies on the axis)");
    } else if (load.type == LoadType::Remote) {
      const Vector3d M = (origin - in.centroid).cross(load.magnitude * dir);
      const double tr = in.inertia.trace();
      if (!(tr > 0) ||
          std::abs(in.inertia.determinant()) <= 1e-12 * tr * tr * tr)
        return fail("a remote load's face cannot carry a moment (its nodes "
                    "lie on a line)");
      omega = in.inertia.inverse() * M;
    }
    ForEachFaceGaussPoint(
        mesh, load.tris,
        [&](const FemMesh::BoundaryTri& tri, const double* N, const Vector3d& x,
            const Vector3d& cross, double dA) {
          Vector3d traction;
          switch (load.type) {
          case LoadType::Force:
            traction = (load.magnitude / in.area) * dir;
            break;
          case LoadType::Torque:
            traction = (load.magnitude / in.polar) * axis.cross(x - origin);
            break;
          case LoadType::Remote:
            traction =
                (load.magnitude / in.area) * dir + omega.cross(x - in.centroid);
            break;
          default: // Pressure: along the inward normal (positive = pushing)
            traction = -load.magnitude * cross.normalized(); // netgen: outward
            break;
          }
          for (int i = 0; i < 6; ++i)
            addNodal(tri.nodes[i], (N[i] * dA) * traction);
        });
  }
  return true;
}

} // namespace internal
} // namespace fem
