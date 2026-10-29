// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/fem/FemMesher.h"

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

#include <cmath>
#include <cstdio>
#include <mutex>
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

} // namespace fem
