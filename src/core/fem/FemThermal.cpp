// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/fem/FemThermal.h"

#ifdef HAVE_FEM_SOLVER
#include "core/fem/FemSolverInternal.h"

#include <Eigen/IterativeLinearSolvers>
#include <Eigen/Sparse>
#include <Eigen/SparseCholesky>
#endif

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

namespace fem {

#ifdef HAVE_FEM_SOLVER
using namespace internal;

namespace {

// The physical gradients of the ten shape functions at (r, s, t), as the
// columns of G, and det J.
double ShapeGradients(const Vector3d X[10], double r, double s, double t,
                      Eigen::Matrix<double, 3, 10>& G) {
  double dN[10][3];
  TetShapeGrad(r, s, t, dN);
  Eigen::Matrix3d J = Eigen::Matrix3d::Zero();
  for (int i = 0; i < 10; ++i)
    for (int j = 0; j < 3; ++j)
      for (int k = 0; k < 3; ++k)
        J(j, k) += dN[i][k] * X[i](j);
  const double detJ = J.determinant();
  const Eigen::Matrix3d invJt = J.inverse().transpose();
  for (int i = 0; i < 10; ++i)
    G.col(i) = invJt * Vector3d(dN[i][0], dN[i][1], dN[i][2]);
  return detJ;
}

void ElementNodes(const FemMesh& mesh, const std::array<int, 10>& tet,
                  Vector3d X[10]) {
  for (int i = 0; i < 10; ++i) {
    const glm::dvec3& p = mesh.nodes[tet[i]];
    X[i] = Vector3d(p.x, p.y, p.z);
  }
}

double FaceArea(const FemMesh& mesh, const std::vector<int>& tris) {
  double area = 0.0;
  ForEachFaceGaussPoint(mesh, tris,
                        [&](const auto&, const double*, const Vector3d&,
                            const Vector3d&, double dA) { area += dA; });
  return area;
}

double TetsVolume(const FemMesh& mesh, const std::vector<int>& tets) {
  double v = 0.0;
  const auto one = [&](size_t t) {
    Vector3d X[10];
    ElementNodes(mesh, mesh.tets[t], X);
    for (const auto& g : kTetGauss) {
      Eigen::Matrix<double, 3, 10> G;
      v += kTetW * std::abs(ShapeGradients(X, g[0], g[1], g[2], G));
    }
  };
  if (tets.empty())
    for (size_t t = 0; t < mesh.tets.size(); ++t)
      one(t);
  else
    for (int t : tets)
      one(static_cast<size_t>(t));
  return v;
}

double ConductivityOf(const FemMesh& mesh, const std::vector<Material>& mats,
                      size_t t) {
  const size_t d = mesh.tetDomain.empty() ? 0 : (size_t)mesh.tetDomain[t];
  return mats[std::min(d, mats.size() - 1)].conductivityWmK;
}

} // namespace

bool HeatNodalLoads(const FemMesh& mesh, const HeatLoads& loads,
                    std::vector<double>& out, std::string* error) {
  const auto fail = [&](const std::string& m) {
    if (error)
      *error = m;
    return false;
  };
  out.assign(mesh.nodes.size(), 0.0);
  for (const HeatLoads::Flux& f : loads.fluxes) {
    double q = f.fluxMWmm2;
    if (f.total) {
      const double area = FaceArea(mesh, f.tris);
      if (!(area > 0.0))
        return fail("heat_flux " + std::to_string(f.id) +
                    ": its face meshed to zero area");
      q = f.totalMW / area;
    }
    ForEachFaceGaussPoint(mesh, f.tris,
                          [&](const FemMesh::BoundaryTri& tri, const double* N,
                              const Vector3d&, const Vector3d&, double dA) {
                            for (int i = 0; i < 6; ++i)
                              out[tri.nodes[i]] += q * N[i] * dA;
                          });
  }
  for (const HeatLoads::Source& s : loads.sources) {
    const double volume = TetsVolume(mesh, s.tets);
    if (!(volume > 0.0))
      return fail("heat_generation " + std::to_string(s.id) +
                  ": its body meshed to no volume");
    const double q = s.powerMW / volume;
    const auto one = [&](size_t t) {
      Vector3d X[10];
      ElementNodes(mesh, mesh.tets[t], X);
      for (const auto& g : kTetGauss) {
        Eigen::Matrix<double, 3, 10> G;
        const double dV =
            kTetW * std::abs(ShapeGradients(X, g[0], g[1], g[2], G));
        double N[10];
        TetShape(g[0], g[1], g[2], N);
        for (int i = 0; i < 10; ++i)
          out[mesh.tets[t][i]] += q * N[i] * dV;
      }
    };
    if (s.tets.empty())
      for (size_t t = 0; t < mesh.tets.size(); ++t)
        one(t);
    else
      for (int t : s.tets)
        one(static_cast<size_t>(t));
  }
  return true;
}

bool SolveHeat(const FemMesh& mesh, const std::vector<Material>& byDomain,
               const HeatLoads& loads, const SolveOptions& options,
               HeatSolution& out, std::string* error,
               const GivenTemperatures* given) {
  out = HeatSolution{};
  const auto fail = [&](const std::string& m) {
    if (error)
      *error = m;
    return false;
  };
  const int n = static_cast<int>(mesh.nodes.size());
  if (n == 0 || mesh.tets.empty())
    return fail("empty mesh");
  if (byDomain.empty())
    return fail("no material");
  for (const Material& m : byDomain)
    if (!(m.conductivityWmK > 0.0))
      return fail("material has no thermal conductivity");
  if (loads.held.empty() && loads.films.empty())
    return fail("its heat has nowhere to go: no face is held at a "
                "temperature and none is cooled by convection, so the "
                "temperature is not determined -- add a temperature or a "
                "convection load");

  // Held nodes: each at its load's temperature; a node two loads hold at
  // different temperatures is refused (the field would jump there).
  std::vector<double> heldT(n, 0.0);
  std::vector<int> heldBy(n, -1);
  for (size_t k = 0; k < loads.held.size(); ++k) {
    const HeatLoads::Held& h = loads.held[k];
    for (int t : h.tris)
      for (int v : mesh.boundary[t].nodes) {
        if (heldBy[v] >= 0 &&
            loads.held[heldBy[v]].temperatureC != h.temperatureC)
          return fail("temperature " + std::to_string(h.id) + " and " +
                      std::to_string(loads.held[heldBy[v]].id) +
                      " hold the faces where they meet at two "
                      "temperatures; the temperature cannot jump at an edge "
                      "-- hold them at one, or hold one face and cool the "
                      "other by convection");
        if (heldBy[v] < 0) {
          heldBy[v] = static_cast<int>(k);
          heldT[v] = h.temperatureC;
        }
      }
  }
  std::vector<int> freeIndex(n, -1);
  int nFree = 0;
  for (int v = 0; v < n; ++v)
    if (heldBy[v] < 0)
      freeIndex[v] = nFree++;

  // K_T over every node: conduction, then each film's h N N^T.
  Eigen::SparseMatrix<double> K(n, n);
  {
    std::vector<Eigen::Triplet<double>> trip;
    trip.reserve(mesh.tets.size() * 100);
    for (size_t t = 0; t < mesh.tets.size(); ++t) {
      Vector3d X[10];
      ElementNodes(mesh, mesh.tets[t], X);
      const double k = ConductivityOf(mesh, byDomain, t);
      Eigen::Matrix<double, 10, 10> Ke = Eigen::Matrix<double, 10, 10>::Zero();
      for (const auto& g : kTetGauss) {
        Eigen::Matrix<double, 3, 10> G;
        const double detJ = ShapeGradients(X, g[0], g[1], g[2], G);
        if (std::abs(detJ) < 1e-12)
          return fail("mesh has a degenerate element");
        Ke += (kTetW * std::abs(detJ) * k) * (G.transpose() * G);
      }
      for (int a = 0; a < 10; ++a)
        for (int b = 0; b < 10; ++b)
          trip.emplace_back(mesh.tets[t][a], mesh.tets[t][b], Ke(a, b));
    }
    for (const HeatLoads::Film& f : loads.films)
      ForEachFaceGaussPoint(
          mesh, f.tris,
          [&](const FemMesh::BoundaryTri& tri, const double* N, const Vector3d&,
              const Vector3d&, double dA) {
            for (int a = 0; a < 6; ++a)
              for (int b = 0; b < 6; ++b)
                trip.emplace_back(tri.nodes[a], tri.nodes[b],
                                  f.hMWmm2K * N[a] * N[b] * dA);
          });
    K.setFromTriplets(trip.begin(), trip.end());
  }
  // Q: the flux faces and the sources, then each film's h T_amb N.
  std::vector<double> q;
  std::string err;
  if (!HeatNodalLoads(mesh, loads, q, &err))
    return fail(err);
  Eigen::VectorXd Q = Eigen::Map<const Eigen::VectorXd>(q.data(), n);
  for (const HeatLoads::Film& f : loads.films)
    ForEachFaceGaussPoint(mesh, f.tris,
                          [&](const FemMesh::BoundaryTri& tri, const double* N,
                              const Vector3d&, const Vector3d&, double dA) {
                            for (int a = 0; a < 6; ++a)
                              Q(tri.nodes[a]) +=
                                  f.hMWmm2K * f.ambientC * N[a] * dA;
                          });

  // The reduced system: K_ff T_f = Q_f - K_fh T_h.
  Eigen::VectorXd Th = Eigen::VectorXd::Zero(n);
  for (int v = 0; v < n; ++v)
    if (heldBy[v] >= 0)
      Th(v) = heldT[v];
  const Eigen::VectorXd KTh = K * Th;
  Eigen::VectorXd F(nFree);
  for (int v = 0; v < n; ++v)
    if (freeIndex[v] >= 0)
      F(freeIndex[v]) = Q(v) - KTh(v);
  Eigen::SparseMatrix<double> Kff(nFree, nFree);
  {
    std::vector<Eigen::Triplet<double>> trip;
    trip.reserve(K.nonZeros());
    for (int c = 0; c < K.outerSize(); ++c)
      for (Eigen::SparseMatrix<double>::InnerIterator it(K, c); it; ++it) {
        const int r = freeIndex[it.row()], cc = freeIndex[it.col()];
        if (r >= 0 && cc >= 0)
          trip.emplace_back(r, cc, it.value());
      }
    Kff.setFromTriplets(trip.begin(), trip.end());
  }

  Eigen::VectorXd Tf(nFree);
  constexpr const char* kSingular =
      "the temperature is not determined (singular conduction matrix)";
  if (given) {
    const std::string& who = given->name;
    if (!given->temperature ||
        given->temperature->size() != static_cast<size_t>(n))
      return fail("the answer from " + who +
                  " is not one temperature per node");
    const std::vector<double>& gt = *given->temperature;
    double scale = 0.0, heldMove = 0.0;
    int heldNode = -1;
    for (int v = 0; v < n; ++v) {
      scale = std::max(scale, std::abs(gt[v]));
      if (freeIndex[v] >= 0)
        Tf(freeIndex[v]) = gt[v];
      else if (std::abs(gt[v] - heldT[v]) > heldMove) {
        heldMove = std::abs(gt[v] - heldT[v]);
        heldNode = v;
      }
    }
    if (!Tf.allFinite())
      return fail("the answer from " + who + " is not finite");
    if (heldMove > (1e-6 + 4.0 * given->precision) * std::max(scale, 1.0))
      return fail("the answer from " + who + " moves node " +
                  std::to_string(heldNode + 1) + " by " +
                  std::to_string(heldMove) +
                  " K off the temperature this study holds it at");
    const double fn = F.norm();
    const double residual = fn > 0 ? (Kff * Tf - F).norm() / fn : 0.0;
    // The printed precision rounds the held temperatures too, which enter F.
    Eigen::VectorXd absT(n);
    for (int v = 0; v < n; ++v)
      absT(v) = std::abs(gt[v]);
    const Eigen::VectorXd KabsT = K.cwiseAbs() * absT;
    Eigen::VectorXd floorF(nFree);
    for (int v = 0; v < n; ++v)
      if (freeIndex[v] >= 0)
        floorF(freeIndex[v]) = KabsT(v);
    const double floor = fn > 0 ? floorF.norm() * given->precision / fn : 0.0;
    if (!(residual <= 1e-6 + floor))
      return fail("the answer from " + who +
                  " does not solve this study's own conduction matrix: |K_T T "
                  "- Q| / |Q| = " +
                  std::to_string(residual) + ", above 1e-6 plus the " +
                  std::to_string(floor) +
                  " its printed precision accounts for, so the deck and the "
                  "study it was written from differ");
    const double work = Tf.dot(F);
    const double energy =
        work != 0.0 ? std::abs(Tf.dot(Kff * Tf - F)) / std::abs(work) : 0.0;
    const double energyFloor =
        work != 0.0
            ? 4.0 * given->precision *
                  (Tf.cwiseAbs().dot(floorF + F.cwiseAbs())) / std::abs(work)
            : 0.0;
    if (!(energy <= 1e-6 + energyFloor))
      return fail("the answer from " + who +
                  " does not balance this study's own conduction matrix: |T . "
                  "(K_T T - Q)| / |T . Q| = " +
                  std::to_string(energy) + ", above 1e-6 plus the " +
                  std::to_string(energyFloor) +
                  " its printed precision accounts for, so the deck and the "
                  "study it was written from differ");
    out.residual = residual;
    out.residualFloor = floor;
    out.energyResidual = energy;
    out.solver = who;
  } else if (nFree > 0) {
    if (options.cancelled && options.cancelled())
      return fail("cancelled");
    const bool iterative = nFree > options.iterativeAboveDofs;
    out.solver = iterative ? kIterativeSolverName : kDirectSolverName;
    if (options.solving)
      options.solving(iterative ? kIterativeSolverName : kDirectSolverName);
    if (iterative) {
      Eigen::IncompleteCholesky<double> ic;
      ic.compute(Kff);
      if (ic.info() != Eigen::Success)
        return fail(kSingular);
      int its = 0;
      double rel = 0.0;
      const PcgOutcome o = Pcg(Kff, ic, F, Tf, 1e-8, std::max(10000, nFree / 4),
                               options.cancelled, &its, &rel);
      if (o == PcgOutcome::Cancelled)
        return fail("cancelled");
      if (o != PcgOutcome::Converged)
        return fail(std::string(kSingular) +
                    ": the iterative solve stopped at a relative residual of " +
                    std::to_string(rel) + " after " + std::to_string(its) +
                    " iterations");
    } else {
      Eigen::SimplicialLDLT<Eigen::SparseMatrix<double>> direct(Kff);
      if (direct.info() != Eigen::Success)
        return fail(kSingular);
      Tf = direct.solve(F);
    }
    const double residual =
        F.norm() > 0 ? (Kff * Tf - F).norm() / F.norm() : 0.0;
    if (!Tf.allFinite() || residual > 1e-6)
      return fail(kSingular);
    out.residual = residual;
  }

  // The field, every node.
  Eigen::VectorXd T = Th;
  for (int v = 0; v < n; ++v)
    if (freeIndex[v] >= 0)
      T(v) = Tf(freeIndex[v]);
  out.temperature.assign(T.data(), T.data() + n);

  // -k grad T at each element's nodes, averaged by element volume; W/m^2.
  {
    std::vector<Vector3d> acc(n, Vector3d::Zero());
    std::vector<double> weight(n, 0.0);
    for (size_t t = 0; t < mesh.tets.size(); ++t) {
      const auto& tet = mesh.tets[t];
      Vector3d X[10];
      ElementNodes(mesh, tet, X);
      Eigen::Matrix<double, 10, 1> Te;
      for (int i = 0; i < 10; ++i)
        Te(i) = T(tet[i]);
      double vol = 0.0;
      for (const auto& g : kTetGauss) {
        Eigen::Matrix<double, 3, 10> G;
        vol += kTetW * std::abs(ShapeGradients(X, g[0], g[1], g[2], G));
      }
      const double k = ConductivityOf(mesh, byDomain, t);
      for (int i = 0; i < 10; ++i) {
        Eigen::Matrix<double, 3, 10> G;
        ShapeGradients(X, kNodeRST[i][0], kNodeRST[i][1], kNodeRST[i][2], G);
        acc[tet[i]] += vol * (-k * (G * Te));
        weight[tet[i]] += vol;
      }
    }
    out.heatFlux.assign(n, glm::dvec3(0.0));
    for (int v = 0; v < n; ++v)
      if (weight[v] > 0) {
        // mW/mm^2 -> W/m^2.
        const Vector3d f = acc[v] / weight[v] / kWm2ToMWmm2;
        out.heatFlux[v] = glm::dvec3(f(0), f(1), f(2));
      }
  }

  // The heat through each load, W out of the part.
  const Eigen::VectorXd R = K * T - Q; // what the held faces put in, mW
  double floorW = 0.0;
  {
    std::vector<char> counted(n, 0);
    Eigen::VectorXd absT = T.cwiseAbs();
    const Eigen::VectorXd KabsT = K.cwiseAbs() * absT;
    for (const HeatLoads::Held& h : loads.held) {
      double mw = 0.0;
      for (int t : h.tris)
        for (int v : mesh.boundary[t].nodes)
          if (!counted[v]) {
            counted[v] = 1;
            mw -= R(v);
            if (given)
              floorW += given->precision * (KabsT(v) + std::abs(Q(v)));
          }
      out.flows.push_back(
          {h.id, LoadType::Temperature, mw / kMilliwattsPerWatt});
    }
  }
  for (const HeatLoads::Film& f : loads.films) {
    double mw = 0.0, absMw = 0.0;
    ForEachFaceGaussPoint(mesh, f.tris,
                          [&](const FemMesh::BoundaryTri& tri, const double* N,
                              const Vector3d&, const Vector3d&, double dA) {
                            double Tx = 0.0;
                            for (int a = 0; a < 6; ++a)
                              Tx += N[a] * T(tri.nodes[a]);
                            mw += f.hMWmm2K * (Tx - f.ambientC) * dA;
                            absMw += f.hMWmm2K * std::abs(Tx) * dA;
                          });
    if (given)
      floorW += given->precision * absMw;
    out.flows.push_back({f.id, LoadType::Convection, mw / kMilliwattsPerWatt});
  }
  for (const HeatLoads::Flux& f : loads.fluxes) {
    const double total =
        f.total ? f.totalMW : f.fluxMWmm2 * FaceArea(mesh, f.tris);
    out.flows.push_back(
        {f.id, LoadType::HeatFlux, -total / kMilliwattsPerWatt});
  }
  for (const HeatLoads::Source& s : loads.sources)
    out.flows.push_back(
        {s.id, LoadType::HeatGeneration, -s.powerMW / kMilliwattsPerWatt});
  for (const HeatFlow& f : out.flows) {
    if (f.watts > 0)
      out.outW += f.watts;
    else
      out.inW -= f.watts;
  }
  out.imbalanceW = out.inW - out.outW;
  out.imbalanceFloorW = floorW / kMilliwattsPerWatt;
  const double through = std::max(out.inW, out.outW);
  if (!(std::abs(out.imbalanceW) <=
        kHeatBalance * through + out.imbalanceFloorW + 1e-12))
    return fail("the heat does not balance: " + std::to_string(out.inW) +
                " W in, " + std::to_string(out.outW) + " W out" +
                (given ? ", past the " + std::to_string(out.imbalanceFloorW) +
                             " W the answer's printed precision accounts "
                             "for, so the deck and the study it was written "
                             "from differ"
                       : std::string()));
  return true;
}

#else

bool HeatNodalLoads(const FemMesh&, const HeatLoads&, std::vector<double>&,
                    std::string* error) {
  if (error)
    *error = "built without the FEM solver (Eigen)";
  return false;
}

bool SolveHeat(const FemMesh&, const std::vector<Material>&, const HeatLoads&,
               const SolveOptions&, HeatSolution&, std::string* error,
               const GivenTemperatures*) {
  if (error)
    *error = "built without the FEM solver (Eigen)";
  return false;
}

#endif

} // namespace fem
