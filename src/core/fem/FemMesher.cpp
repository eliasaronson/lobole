// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/fem/FemMesher.h"

#include "common/Logger.h"

#include <BRepAdaptor_Surface.hxx>
#include <BRepBndLib.hxx>
#include <BRepGProp.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box.hxx>
#include <GProp_GProps.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

#ifdef HAVE_NETGEN
// Netgen's C++ meshing interface. Every netgen symbol is confined to this
// translation unit. OCCGEOMETRY gates the OCCGeometry class in occgeom.hpp and
// is defined here (locally) rather than globally; the ABI-matching defines
// (PARALLEL, -fabi-version=17, ...) come from the nglib target in CMakeLists.
// See the FEM0 spike: the C nglib API cannot attribute surface elements to OCC
// faces, and mismatching netgen's build defines heap-corrupts the process.
#define OCCGEOMETRY
#include <meshing.hpp>
#include <occgeom.hpp>
#endif

namespace fem {

namespace {

// Auto element size when the study leaves it at 0: a fraction of the bounding
// box diagonal, the same adaptive spirit as CADKernel tessellation deflection.
double AutoSize(const TopoDS_Shape& shape) {
  Bnd_Box box;
  BRepBndLib::Add(shape, box);
  if (box.IsVoid())
    return 1.0;
  double xmin, ymin, zmin, xmax, ymax, zmax;
  box.Get(xmin, ymin, zmin, xmax, ymax, zmax);
  const double diag =
      std::sqrt((xmax - xmin) * (xmax - xmin) + (ymax - ymin) * (ymax - ymin) +
                (zmax - zmin) * (zmax - zmin));
  return diag > 0 ? diag / 15.0 : 1.0;
}

// The volume of every solid of `shape` (mm^3; 0 for one with none), and in
// `error` how well it is known: the adaptive integration's own estimate
// over curved faces, asked for to a part in a hundred thousand.
[[maybe_unused]] double ShapeVolume(const TopoDS_Shape& shape, double& error) {
  double v = 0.0;
  error = 0.0;
  for (TopExp_Explorer ex(shape, TopAbs_SOLID); ex.More(); ex.Next()) {
    GProp_GProps props;
    const double rel = BRepGProp::VolumeProperties(ex.Current(), props, 1e-5);
    const double solid = std::abs(props.Mass());
    v += solid;
    error += std::max(rel, 0.0) * solid;
  }
  return v;
}

// Normalized tetrahedron shape quality from its four corners, in [0,1]:
// 72*sqrt(3) * V / (sum of squared edge lengths)^(3/2). 1 for a regular tet,
// -> 0 for a sliver. Midside nodes don't enter the linear shape metric.
double TetQuality(const glm::dvec3& a, const glm::dvec3& b, const glm::dvec3& c,
                  const glm::dvec3& d) {
  const double vol = std::abs(glm::dot(b - a, glm::cross(c - a, d - a))) / 6.0;
  double sumSq = 0.0;
  const glm::dvec3 p[4] = {a, b, c, d};
  for (int i = 0; i < 4; ++i)
    for (int j = i + 1; j < 4; ++j) {
      const glm::dvec3 e = p[i] - p[j];
      sumSq += glm::dot(e, e);
    }
  if (sumSq <= 0)
    return 0.0;
  const double q = 72.0 * std::sqrt(3.0) * vol / std::pow(sumSq, 1.5);
  return q < 0 ? 0 : (q > 1 ? 1 : q);
}

// WHERE the face is, in the model, not just which slot it occupies in a
// list the caller cannot see. "face 13 of 90" is unresolvable — the numbering
// is OCC's explorer order, no RPC hands it out, and the body name in the
// message is the last feature that touched the shape. Its kind, its area and
// its middle are enough to find it by eye or by list_elements.
std::string DescribeFace(const TopTools_IndexedMapOfShape& faceMap,
                         int oneBased) {
  if (oneBased < 1 || oneBased > faceMap.Extent())
    return "face not in the map";
  const TopoDS_Face face = TopoDS::Face(faceMap(oneBased));
  const char* kind = "a face";
  switch (BRepAdaptor_Surface(face).GetType()) {
  case GeomAbs_Plane:
    kind = "a plane";
    break;
  case GeomAbs_Cylinder:
    kind = "a cylinder";
    break;
  case GeomAbs_Cone:
    kind = "a cone";
    break;
  case GeomAbs_Sphere:
    kind = "a sphere";
    break;
  case GeomAbs_Torus:
    kind = "a torus";
    break;
  case GeomAbs_BSplineSurface:
  case GeomAbs_BezierSurface:
    kind = "a spline surface";
    break;
  default:
    break;
  }
  GProp_GProps props;
  BRepGProp::SurfaceProperties(face, props);
  const gp_Pnt c = props.CentreOfMass();
  const auto mm = [](double v) {
    std::string s = std::to_string(v);
    const size_t dot = s.find('.');
    return dot == std::string::npos ? s : s.substr(0, dot + 3);
  };
  std::string out = std::string(kind) + ", area " + mm(props.Mass()) +
                    " mm2, centred at (" + mm(c.X()) + ", " + mm(c.Y()) + ", " +
                    mm(c.Z()) + ")";
  // A sliver edge is the usual reason, and it is invisible: BRepCheck passes
  // it and no picture shows a 5e-6 mm edge. Say so, and where.
  double shortest = 1e30;
  gp_Pnt at;
  for (TopExp_Explorer ex(face, TopAbs_EDGE); ex.More(); ex.Next()) {
    const TopoDS_Edge& e = TopoDS::Edge(ex.Current());
    if (BRep_Tool::Degenerated(e))
      continue;
    GProp_GProps lp;
    BRepGProp::LinearProperties(e, lp);
    if (lp.Mass() < shortest) {
      shortest = lp.Mass();
      at = lp.CentreOfMass();
    }
  }
  if (shortest < 1e-3) {
    char buf[200];
    std::snprintf(buf, sizeof buf,
                  "; it has an edge %.2g mm long at (%.2f, %.2f, %.2f) -- "
                  "two faces meant to coincide that miss by that much",
                  shortest, at.X(), at.Y(), at.Z());
    out += buf;
  }
  return out;
}

} // namespace

double AutoMeshSize(const TopoDS_Shape& shape) { return AutoSize(shape); }

#ifdef HAVE_NETGEN

namespace {

// Netgen keeps every shape's meshing properties (a face's maxh among them)
// in a STATIC table keyed by shape (OCCGeometry::global_shape_property_
// indices / global_shape_properties), shared by every mesh in the process
// and not thread-safe. OCCGeometry's constructor copies a shape's entry into
// its own GeometryFace, and OCCGeometry::MeshFace adds an entry for every
// face it meshes, refined or not. So one lock covers a whole Mesh() -- from
// setting the properties until the volume mesh and its second order are done
// -- and every entry for a shape of ours is taken out again when it returns
// (docs/simulation/README.md#meshing).
std::mutex& NetgenTableMutex() {
  static std::mutex m;
  return m;
}

// Netgen has no remove call: the indexed map is rebuilt without the shapes
// of `shape`, each dropped entry reset to the default properties first, so
// nothing of ours outlives the mesh (the table would otherwise grow with
// every run and hold every meshed copy alive). Caller holds the lock.
void ForgetNetgenProperties(const TopoDS_Shape& shape) {
  TopTools_IndexedMapOfShape ours;
  TopExp::MapShapes(shape, ours);
  ours.Add(shape);
  auto& index = netgen::OCCGeometry::global_shape_property_indices;
  auto& props = netgen::OCCGeometry::global_shape_properties;
  bool any = false;
  for (int i = 1; i <= index.Extent() && !any; ++i)
    any = ours.Contains(index(i));
  if (!any)
    return;
  TopTools_IndexedMapOfShape keptIndex;
  std::vector<netgen::ShapeProperties> keptProps;
  for (int i = 1; i <= index.Extent(); ++i) {
    if (ours.Contains(index(i))) {
      if (static_cast<size_t>(i - 1) < props.size())
        props[i - 1] = netgen::ShapeProperties{};
      continue;
    }
    keptIndex.Add(index(i));
    keptProps.push_back(static_cast<size_t>(i - 1) < props.size()
                            ? std::move(props[i - 1])
                            : netgen::ShapeProperties{});
  }
  index = std::move(keptIndex);
  props = std::move(keptProps);
}

} // namespace

bool Available() { return true; }

int NetgenPropertyEntries(const TopoDS_Shape& shape) {
  std::lock_guard<std::mutex> lock(NetgenTableMutex());
  TopTools_IndexedMapOfShape ours;
  TopExp::MapShapes(shape, ours);
  ours.Add(shape);
  const auto& index = netgen::OCCGeometry::global_shape_property_indices;
  int n = 0;
  for (int i = 1; i <= index.Extent(); ++i)
    if (ours.Contains(index(i)))
      ++n;
  return n;
}

std::optional<FemMesh> Mesh(const TopoDS_Shape& shape,
                            const MeshSettings& settings, std::string* error) {
  auto fail = [&](const std::string& m) -> std::optional<FemMesh> {
    if (error)
      *error = m;
    return std::nullopt;
  };
  if (shape.IsNull())
    return fail("cannot mesh a null shape");

  // OCC face index (our TopExp_Explorer 0-based order) for a netgen surfnr.
  // The FEM0 spike verified geo.fmap agrees with the explorer order, but we
  // resolve through OCC's own IndexedMap rather than assume it.
  TopTools_IndexedMapOfShape faceMap;
  TopExp::MapShapes(shape, TopAbs_FACE, faceMap);

  // The property table, held for the whole mesh and emptied of our shapes
  // on every way out, an exception's included.
  std::lock_guard<std::mutex> lock(NetgenTableMutex());
  struct Forget {
    const TopoDS_Shape& shape;
    ~Forget() { ForgetNetgenProperties(shape); }
  } forget{shape};

  try {
    netgen::MeshingParameters mp;
    mp.maxh = settings.maxSize > 0 ? settings.maxSize : AutoSize(shape);
    mp.uselocalh = true;
    mp.optsteps3d = 3;

    // Per-face refinements through the face's shape properties, as Netgen's
    // python `face.maxh` sets them: Netgen grades that size into the face's
    // edges and the volume behind it. The legacy SetFaceMaxH array sized the
    // face's own triangles only, so a fine face met coarse tets head on and
    // left slivers (one gave a peak 50 % high). Set BEFORE the geometry is
    // built: OCCGeometry's constructor copies each face's properties then.
    // A refinement also takes the grading from Netgen's 0.3 to 0.2, which
    // lifts the worst tet of a refined hole from ~0.13-0.25 to ~0.35.
    bool refined = false;
    for (const auto& [faceIndex, size] : settings.faceSizes) {
      if (size <= 0 || faceIndex < 0 || faceIndex >= faceMap.Extent())
        continue;
      netgen::ShapeProperties& prop =
          netgen::OCCGeometry::GetProperties(faceMap(faceIndex + 1));
      prop.maxh = std::min(prop.maxh, size);
      refined = true;
    }
    if (refined)
      mp.grading = 0.2;

    netgen::OCCGeometry geo(shape);

    auto ngmesh = std::make_shared<netgen::Mesh>();
    // Non-owning geometry handle: the mesh must not free our stack geometry.
    ngmesh->SetGeometry(std::shared_ptr<netgen::NetgenGeometry>(
        &geo, [](netgen::NetgenGeometry*) {}));

    // Surface phase first, then verify every face actually meshed, then the
    // volume phase. Netgen's volume mesher walks into corrupt memory when
    // handed an incomplete surface mesh (measured: "SURFACE MESHING ERROR
    // OCCURRED IN 2 FACES" followed by a double-free abort that took the
    // whole app down) — the failure must be caught BETWEEN the phases,
    // where it is still a clean, reportable error.
    mp.perfstepsstart = netgen::MESHCONST_ANALYSE;
    mp.perfstepsend = netgen::MESHCONST_OPTSURFACE;
    int rc = geo.GenerateMesh(ngmesh, mp);
    if (rc != 0)
      return fail("surface meshing failed (netgen returned " +
                  std::to_string(rc) + ")");
    for (int i = 0; i < geo.facemeshstatus.Size(); ++i)
      if (geo.facemeshstatus[i] <= 0)
        return fail("surface meshing failed on face " + std::to_string(i + 1) +
                    " of " + std::to_string(geo.facemeshstatus.Size()) + " (" +
                    DescribeFace(faceMap, i + 1) +
                    ") — the shape may have sliver/degenerate faces, or the "
                    "mesh size may not suit its smallest features. Refine "
                    "the model there, or Refine the body to heal the seams "
                    "a long boolean chain leaves");
    mp.perfstepsstart = netgen::MESHCONST_MESHVOLUME;
    mp.perfstepsend = netgen::MESHCONST_OPTVOLUME;
    rc = geo.GenerateMesh(ngmesh, mp);
    if (rc != 0)
      return fail("volume meshing failed (netgen returned " +
                  std::to_string(rc) + ")");

    // Second order with geometry adaption: midside nodes projected onto the
    // true surface rather than linearly interpolated.
    geo.GetRefinement().MakeSecondOrder(*ngmesh);

    const int np = ngmesh->GetNP();
    const int ne = ngmesh->GetNE();
    const int nse = ngmesh->GetNSE();
    if (np <= 0 || ne <= 0)
      return fail("meshing produced an empty mesh");

    FemMesh out;
    out.nodes.resize(np);
    for (int i = 1; i <= np; ++i) {
      const auto& pt = (*ngmesh)[netgen::PointIndex(i)];
      out.nodes[i - 1] = glm::dvec3(pt(0), pt(1), pt(2));
    }

    // netgen tet10 PNum -> our canonical slot (see FemMesh doc + FEM0 spike):
    //   corners 1..4; PNum 5..10 = edges (1,2)(1,3)(1,4)(2,3)(2,4)(3,4).
    static const int kTetRemap[10] = {1, 2, 3, 4, 5, 8, 6, 7, 9, 10};
    out.tets.reserve(ne);
    double worst = 1.0;
    for (netgen::ElementIndex ei(0); ei < ne; ++ei) {
      const netgen::Element& el = (*ngmesh)[ei];
      if (el.GetNP() != 10)
        return fail("expected tet10 elements from second-order meshing");
      std::array<int, 10> t;
      for (int k = 0; k < 10; ++k)
        t[k] = int(el.PNum(kTetRemap[k])) - 1;
      out.tets.push_back(t);
      out.tetDomain.push_back(std::max(0, el.GetIndex() - 1));
      worst = std::min(worst, TetQuality(out.nodes[t[0]], out.nodes[t[1]],
                                         out.nodes[t[2]], out.nodes[t[3]]));
    }

    // netgen tri6 PNum -> canonical (corners 1..3; PNum 4,5,6 = edges
    // (2,3)(1,3)(1,2)); canonical is corners then edges (0,1)(1,2)(2,0).
    static const int kTriRemap[6] = {1, 2, 3, 6, 4, 5};
    out.boundary.reserve(nse);
    for (netgen::SurfaceElementIndex si(0); si < nse; ++si) {
      const netgen::Element2d& se = (*ngmesh)[si];
      if (se.GetNP() != 6)
        continue; // skip any non-tri6 facets defensively
      const int surfnr = ngmesh->GetFaceDescriptor(se.GetIndex()).SurfNr();
      FemMesh::BoundaryTri tri;
      for (int k = 0; k < 6; ++k)
        tri.nodes[k] = int(se.PNum(kTriRemap[k])) - 1;
      // surfnr is 1-based over the same map as our 0-based face index.
      tri.faceIndex = surfnr - 1;
      out.boundary.push_back(tri);
    }

    // Midside nodes projected onto a tight curve can fold an element
    // (docs/simulation/README.md#curved-elements-that-fold): those are
    // straightened, and one still folded after is refused.
    // The mesh fills its shape, or it is not that shape's mesh: measured
    // before anything is straightened, which moves the boundary off it.
    out.stats.volumeMm3 = MeshVolume(out);
    out.stats.shapeVolumeMm3 =
        ShapeVolume(shape, out.stats.shapeVolumeErrorMm3);
    if (std::string why = MeshVolumeRefusal(out, out.stats.shapeVolumeMm3,
                                            out.stats.shapeVolumeErrorMm3);
        !why.empty())
      return fail(why);
    const std::size_t moved = StraightenFoldedTets(out);
    if (moved)
      LOG_INFO("fem mesh: straightened {} midside nodes of elements their "
               "curved edges folded",
               moved);
    if (std::string why = FoldedTetRefusal(out); !why.empty())
      return fail(why);

    out.stats.straightenedNodes = moved;
    out.stats.nodeCount = std::size_t(np);
    out.stats.tetCount = std::size_t(ne);
    out.stats.worstQuality = worst;
    return out;
  } catch (const std::exception& e) {
    return fail(std::string("meshing failed: ") + e.what());
  } catch (...) {
    return fail("meshing failed (netgen threw an unknown exception)");
  }
}

#else // !HAVE_NETGEN

bool Available() { return false; }

int NetgenPropertyEntries(const TopoDS_Shape&) { return 0; }

std::optional<FemMesh> Mesh(const TopoDS_Shape&, const MeshSettings&,
                            std::string* error) {
  if (error)
    *error = "built without FEM support (Netgen not found at build time)";
  return std::nullopt;
}

#endif // HAVE_NETGEN

// --- Curved tet10 that fold ------------------------------------------------

namespace {

// det J of tet `X` at natural point (r, s, t): J_jk = sum_i dN_i/dk X_i[j],
// with the solver's shape functions (FemSolverInternal.h, TetShapeGrad).
double DetJ(const glm::dvec3 X[10], double r, double s, double t) {
  const double L[4] = {1 - r - s - t, r, s, t};
  static const double dL[4][3] = {
      {-1, -1, -1}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
  static const int edge[6][2] = {{0, 1}, {1, 2}, {0, 2},
                                 {0, 3}, {1, 3}, {2, 3}};
  double J[3][3] = {{0, 0, 0}, {0, 0, 0}, {0, 0, 0}};
  const auto add = [&](const glm::dvec3& x, const double g[3]) {
    for (int j = 0; j < 3; ++j)
      for (int k = 0; k < 3; ++k)
        J[j][k] += g[k] * x[j];
  };
  for (int i = 0; i < 4; ++i) {
    const double f = 4 * L[i] - 1;
    const double g[3] = {f * dL[i][0], f * dL[i][1], f * dL[i][2]};
    add(X[i], g);
  }
  for (int e = 0; e < 6; ++e) {
    const int a = edge[e][0], b = edge[e][1];
    double g[3];
    for (int k = 0; k < 3; ++k)
      g[k] = 4 * (dL[a][k] * L[b] + L[a] * dL[b][k]);
    add(X[4 + e], g);
  }
  return J[0][0] * (J[1][1] * J[2][2] - J[1][2] * J[2][1]) -
         J[0][1] * (J[1][0] * J[2][2] - J[1][2] * J[2][0]) +
         J[0][2] * (J[1][0] * J[2][1] - J[1][1] * J[2][0]);
}

// The 20 multi-indices of degree 3 over four barycentric coordinates: the
// lattice det J is sampled on, and the Bernstein basis it is written in.
struct Cubic {
  int idx[20][4];
  double toBernstein[20][20]; // Bernstein coefficients from lattice values
  Cubic() {
    int n = 0;
    for (int i = 0; i <= 3; ++i)
      for (int j = 0; j <= 3 - i; ++j)
        for (int k = 0; k <= 3 - i - j; ++k) {
          idx[n][0] = i;
          idx[n][1] = j;
          idx[n][2] = k;
          idx[n][3] = 3 - i - j - k;
          ++n;
        }
    // M[p][q] = B_q(lattice point p) = 3!/(q!) prod L_p^q; invert it.
    static const double fact[4] = {1, 1, 2, 6};
    double M[20][40];
    for (int p = 0; p < 20; ++p)
      for (int q = 0; q < 20; ++q) {
        double v = 6.0;
        for (int c = 0; c < 4; ++c)
          v *= std::pow(idx[p][c] / 3.0, idx[q][c]) / fact[idx[q][c]];
        M[p][q] = v;
        M[p][20 + q] = p == q ? 1.0 : 0.0;
      }
    for (int c = 0; c < 20; ++c) {
      int piv = c;
      for (int r = c + 1; r < 20; ++r)
        if (std::abs(M[r][c]) > std::abs(M[piv][c]))
          piv = r;
      for (int k = 0; k < 40; ++k)
        std::swap(M[c][k], M[piv][k]);
      const double d = M[c][c];
      for (int k = 0; k < 40; ++k)
        M[c][k] /= d;
      for (int r = 0; r < 20; ++r)
        if (r != c && M[r][c] != 0.0) {
          const double f = M[r][c];
          for (int k = 0; k < 40; ++k)
            M[r][k] -= f * M[c][k];
        }
    }
    for (int p = 0; p < 20; ++p)
      for (int q = 0; q < 20; ++q)
        toBernstein[p][q] = M[p][20 + q];
  }
};
const Cubic& TheCubic() {
  static const Cubic c;
  return c;
}

using Bary = std::array<double, 4>;

// Whether det J keeps `sign` over the sub-tet with barycentric corners `c`:
// +1 proven, -1 not (with `at`, the natural point found), 0 undecided at the
// depth cap (every sample right, the bound not yet tight).
int Certify(const glm::dvec3 X[10], const std::array<Bary, 4>& c, int sign,
            int depth, Bary& at) {
  const Cubic& cu = TheCubic();
  double v[20];
  for (int p = 0; p < 20; ++p) {
    Bary b{0, 0, 0, 0};
    for (int k = 0; k < 4; ++k)
      for (int m = 0; m < 4; ++m)
        b[m] += (cu.idx[p][k] / 3.0) * c[k][m];
    v[p] = sign * DetJ(X, b[1], b[2], b[3]);
    if (!(v[p] > 0.0)) {
      at = b;
      return -1;
    }
  }
  bool proven = true;
  for (int q = 0; q < 20 && proven; ++q) {
    double bq = 0.0;
    for (int p = 0; p < 20; ++p)
      bq += cu.toBernstein[q][p] * v[p];
    proven = bq > 0.0;
  }
  if (proven)
    return 1;
  constexpr int kMaxDepth = 5;
  if (depth >= kMaxDepth)
    return 0;
  const auto mid = [&](int a, int b) {
    Bary m;
    for (int k = 0; k < 4; ++k)
      m[k] = 0.5 * (c[a][k] + c[b][k]);
    return m;
  };
  const Bary m01 = mid(0, 1), m02 = mid(0, 2), m03 = mid(0, 3), m12 = mid(1, 2),
             m13 = mid(1, 3), m23 = mid(2, 3);
  const std::array<Bary, 4> sub[8] = {
      {c[0], m01, m02, m03}, {m01, c[1], m12, m13}, {m02, m12, c[2], m23},
      {m03, m13, m23, c[3]}, {m01, m02, m03, m13},  {m01, m02, m12, m13},
      {m02, m03, m13, m23},  {m02, m12, m13, m23}};
  int worst = 1;
  for (const auto& s : sub) {
    const int r = Certify(X, s, sign, depth + 1, at);
    if (r < 0)
      return -1;
    worst = std::min(worst, r);
  }
  return worst;
}

// Whether tet `t` folds against `sign`; `at` the point where it does.
bool Folds(const FemMesh& mesh, std::size_t t, int sign, glm::dvec3& at) {
  glm::dvec3 X[10];
  for (int i = 0; i < 10; ++i)
    X[i] = mesh.nodes[mesh.tets[t][i]];
  const std::array<Bary, 4> whole = {Bary{1, 0, 0, 0}, Bary{0, 1, 0, 0},
                                     Bary{0, 0, 1, 0}, Bary{0, 0, 0, 1}};
  Bary b;
  if (Certify(X, whole, sign, 0, b) >= 0)
    return false;
  // The natural point's place: the shape functions at it.
  const double L[4] = {b[0], b[1], b[2], b[3]};
  static const int edge[6][2] = {{0, 1}, {1, 2}, {0, 2},
                                 {0, 3}, {1, 3}, {2, 3}};
  at = glm::dvec3(0.0);
  for (int i = 0; i < 4; ++i)
    at += L[i] * (2 * L[i] - 1) * X[i];
  for (int e = 0; e < 6; ++e)
    at += 4 * L[edge[e][0]] * L[edge[e][1]] * X[4 + e];
  return true;
}

} // namespace

int MeshOrientation(const FemMesh& mesh) {
  std::size_t pos = 0, neg = 0;
  for (const auto& t : mesh.tets) {
    const glm::dvec3 a = mesh.nodes[t[0]];
    const double v =
        glm::dot(mesh.nodes[t[1]] - a,
                 glm::cross(mesh.nodes[t[2]] - a, mesh.nodes[t[3]] - a));
    (v >= 0 ? pos : neg) += 1;
  }
  if (pos == 0 && neg == 0)
    return 0;
  return pos >= neg ? 1 : -1;
}

std::vector<TetFold> FoldedTets(const FemMesh& mesh, std::size_t limit) {
  std::vector<TetFold> out;
  const int sign = MeshOrientation(mesh);
  if (sign == 0)
    return out;
  for (std::size_t t = 0; t < mesh.tets.size() && out.size() < limit; ++t) {
    glm::dvec3 at;
    if (Folds(mesh, t, sign, at))
      out.push_back({static_cast<int>(t), at});
  }
  return out;
}

std::string FoldedTetRefusal(const FemMesh& mesh) {
  const std::vector<TetFold> f = FoldedTets(mesh, 1);
  if (f.empty())
    return "";
  char buf[160];
  std::snprintf(buf, sizeof buf, "(%.4g, %.4g, %.4g) mm", f[0].at.x, f[0].at.y,
                f[0].at.z);
  return "the mesh has an element that folds: its curved edges turn it inside "
         "out at " +
         std::string(buf) + " (element " + std::to_string(f[0].tet + 1) +
         " of " + std::to_string(mesh.tets.size()) +
         "), so it cannot be integrated; a finer mesh size there, or a "
         "larger fillet radius, gives the curve more elements";
}

TetMeasure MeasureTet(const FemMesh& mesh, std::size_t tet, int sign) {
  TetMeasure m;
  const auto& t = mesh.tets[tet];
  glm::dvec3 X[10];
  for (int i = 0; i < 10; ++i)
    X[i] = mesh.nodes[t[i]];
  m.shape = TetQuality(X[0], X[1], X[2], X[3]);
  double len = 0.0;
  for (int i = 0; i < 4; ++i)
    for (int j = i + 1; j < 4; ++j)
      len += glm::length(X[i] - X[j]);
  m.size = len / 6.0;
  const Cubic& cu = TheCubic();
  double lo = 0.0, hi = 0.0;
  for (int p = 0; p < 20; ++p) {
    const double d =
        (sign < 0 ? -1.0 : 1.0) *
        DetJ(X, cu.idx[p][1] / 3.0, cu.idx[p][2] / 3.0, cu.idx[p][3] / 3.0);
    lo = p == 0 ? d : std::min(lo, d);
    hi = p == 0 ? d : std::max(hi, d);
  }
  m.jacobian = hi > 0.0 ? std::clamp(lo / hi, -1.0, 1.0) : -1.0;
  return m;
}

double MeshVolume(const FemMesh& mesh) {
  // Closed Newton-Cotes of degree 3 on a tetrahedron: the corners at 1/40
  // and the face centres at 9/40 of the reference volume, 1/6.
  static const double pts[8][3] = {{0, 0, 0},
                                   {1, 0, 0},
                                   {0, 1, 0},
                                   {0, 0, 1},
                                   {1.0 / 3, 1.0 / 3, 1.0 / 3},
                                   {0, 1.0 / 3, 1.0 / 3},
                                   {1.0 / 3, 0, 1.0 / 3},
                                   {1.0 / 3, 1.0 / 3, 0}};
  double v = 0.0;
  for (const auto& t : mesh.tets) {
    glm::dvec3 X[10];
    for (int i = 0; i < 10; ++i)
      X[i] = mesh.nodes[t[i]];
    double e = 0.0;
    for (int p = 0; p < 8; ++p)
      e += (p < 4 ? 1.0 : 9.0) * DetJ(X, pts[p][0], pts[p][1], pts[p][2]);
    v += std::abs(e) / 240.0;
  }
  return v;
}

double MeshVolumeSlack(const FemMesh& mesh, double volume, double shapeError) {
  double slack = 0.0;
  for (const FemMesh::BoundaryTri& tri : mesh.boundary) {
    const glm::dvec3 a = mesh.nodes[tri.nodes[0]], b = mesh.nodes[tri.nodes[1]],
                     c = mesh.nodes[tri.nodes[2]];
    const double area = 0.5 * glm::length(glm::cross(b - a, c - a));
    const double sag =
        std::max({glm::length(mesh.nodes[tri.nodes[3]] - 0.5 * (a + b)),
                  glm::length(mesh.nodes[tri.nodes[4]] - 0.5 * (b + c)),
                  glm::length(mesh.nodes[tri.nodes[5]] - 0.5 * (c + a))});
    slack += area * sag;
  }
  return slack + std::max(shapeError, 0.0) + 1e-5 * std::abs(volume);
}

std::string MeshVolumeRefusal(const FemMesh& mesh, double shapeVolume,
                              double shapeError) {
  if (!(shapeVolume > 0.0) || mesh.tets.empty())
    return "";
  const double v = MeshVolume(mesh);
  const double slack = MeshVolumeSlack(mesh, shapeVolume, shapeError);
  if (std::abs(v - shapeVolume) <= slack)
    return "";
  char buf[400];
  std::snprintf(buf, sizeof buf,
                "the mesh does not fill its shape: its elements hold %.6g "
                "mm3 and the body %.6g mm3, %.3g mm3 apart where the "
                "curved faces' deflection accounts for %.3g; the mesher "
                "%s, so nothing is solved on it (a finer mesh size, or "
                "repairing the body, is the way on)",
                v, shapeVolume, std::abs(v - shapeVolume), slack,
                v < shapeVolume ? "left part of the body empty"
                                : "meshed more than the body");
  return buf;
}

std::size_t StraightenFoldedTets(FemMesh& mesh) {
  const int sign = MeshOrientation(mesh);
  if (sign == 0)
    return 0;
  static const int edge[6][2] = {{0, 1}, {1, 2}, {0, 2},
                                 {0, 3}, {1, 3}, {2, 3}};
  std::vector<std::size_t> check(mesh.tets.size());
  for (std::size_t t = 0; t < check.size(); ++t)
    check[t] = t;
  std::vector<std::vector<int>> tetsOf; // built at the first fold
  std::vector<char> moved(mesh.nodes.size(), 0);
  std::size_t count = 0;
  // Each pass straightens what it finds and checks the tets that share a
  // moved node; a node straightened once stays put, so it ends.
  while (!check.empty()) {
    std::vector<std::size_t> next;
    std::vector<char> queued(mesh.tets.size(), 0);
    for (std::size_t t : check) {
      glm::dvec3 at;
      if (!Folds(mesh, t, sign, at))
        continue;
      if (tetsOf.empty()) {
        tetsOf.resize(mesh.nodes.size());
        for (std::size_t u = 0; u < mesh.tets.size(); ++u)
          for (int v : mesh.tets[u])
            tetsOf[v].push_back(static_cast<int>(u));
      }
      const auto& tet = mesh.tets[t];
      for (int e = 0; e < 6; ++e) {
        const int m = tet[4 + e];
        if (moved[m])
          continue;
        moved[m] = 1;
        mesh.nodes[m] =
            0.5 * (mesh.nodes[tet[edge[e][0]]] + mesh.nodes[tet[edge[e][1]]]);
        ++count;
        for (int u : tetsOf[m])
          if (!queued[u]) {
            queued[u] = 1;
            next.push_back(static_cast<std::size_t>(u));
          }
      }
    }
    check = std::move(next);
  }
  return count;
}

} // namespace fem
