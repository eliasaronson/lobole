#include "core/cam/CamToolpath.h"

#include "core/cam/CamToolpathInternal.h"

#include <clipper2/clipper.h>

#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>

// The surfacing PICK: a face of any orientation, as the XY region under it
// (docs/cam/operations.md#3-axis-surfacing).
//
// Its two siblings live in CamPicks.cpp, and this one cannot: that file is
// built whether or not CAM is enabled, because a job document has to load and
// a pick has to resolve in either build, and a silhouette is a polygon UNION
// -- which needs Clipper2, which is exactly what the CAM-less build does not
// have. So the split is a build-configuration fact rather than a taste, and
// the file says so out loud instead of the function hiding in the geometry
// helpers where nobody looking for a pick would find it.

namespace cam {

using namespace detail;

// --- the surfacing pick ---------------------------------------------------

namespace {

// Tessellate `shape` and union its triangles' XY shadows into one region.
//
// A projection, not a wire walk. A curved face's outer wire runs over the
// surface in 3D, and flattening it gives the boundary of a curved patch --
// which is not the area under it. FreeCAD reaches the same place by
// extruding the face and slicing the solid; projecting the triangles is the
// same answer with the mesh we already need for the drop cutter.
Clipper2Lib::Paths64 SilhouetteOf(const CADShape& shape, double deflection,
                                  double& minZ, double& maxZ, bool& any) {
  // Detached, like everywhere else that wants its own tolerance: the
  // triangulation rides on the shared TShape.
  const CADShape::Mesh mesh = shape.GenerateMeshDetached(deflection);
  Clipper2Lib::Paths64 tris;
  tris.reserve(mesh.indices.size() / 3);
  for (size_t t = 0; t + 2 < mesh.indices.size(); t += 3) {
    const glm::vec3& a = mesh.vertices[mesh.indices[t]].position;
    const glm::vec3& b = mesh.vertices[mesh.indices[t + 1]].position;
    const glm::vec3& c = mesh.vertices[mesh.indices[t + 2]].position;
    for (const glm::vec3& p : {a, b, c}) {
      minZ = any ? std::min(minZ, static_cast<double>(p.z)) : p.z;
      maxZ = any ? std::max(maxZ, static_cast<double>(p.z)) : p.z;
      any = true;
    }
    Clipper2Lib::Path64 tri{{static_cast<int64_t>(a.x * kScale),
                             static_cast<int64_t>(a.y * kScale)},
                            {static_cast<int64_t>(b.x * kScale),
                             static_cast<int64_t>(b.y * kScale)},
                            {static_cast<int64_t>(c.x * kScale),
                             static_cast<int64_t>(c.y * kScale)}};
    // A triangle seen edge-on projects to a sliver of no area. Dropping it
    // costs nothing -- its neighbours cover the same ground -- and keeping it
    // feeds Clipper a degenerate polygon.
    if (std::abs(Clipper2Lib::Area(tri)) < 1.0)
      continue;
    if (Clipper2Lib::Area(tri) < 0)
      std::reverse(tri.begin(), tri.end());
    tris.push_back(std::move(tri));
  }
  if (tris.empty())
    return {};
  return Clipper2Lib::Union(tris, Clipper2Lib::FillRule::NonZero);
}

SurfaceRegion RegionFromPaths(const Clipper2Lib::Paths64& paths, double minZ,
                              double maxZ) {
  SurfaceRegion out;
  for (const Clipper2Lib::Path64& p : paths) {
    if (p.size() < 3)
      continue;
    Loop l;
    l.pts = FromClipper(p);
    // Clipper winds an outer boundary positive and a hole negative, which is
    // the same convention `Loop::hole` carries.
    l.hole = Clipper2Lib::Area(p) < 0;
    out.loops.push_back(std::move(l));
  }
  if (out.loops.empty()) {
    out.why = "that face has no area seen from above — it is edge-on to the "
              "spindle, so there is nothing under it to finish";
    return out;
  }
  std::stable_partition(out.loops.begin(), out.loops.end(),
                        [](const Loop& l) { return !l.hole; });
  out.minZ = minZ;
  out.maxZ = maxZ;
  out.ok = true;
  return out;
}

} // namespace

SurfaceRegion PickedSurfaceRegion(const std::vector<Body>& bodies,
                                  const ElementRef& ref, double deflection,
                                  uint64_t jobPartId) {
  SurfaceRegion out;
  if (!ref.IsSet()) {
    out.why = "nothing picked";
    return out;
  }
  for (const Body& body : bodies) {
    if (body.createdBy != ref.featureId || !body.shape.IsValid())
      continue;
    if (jobPartId != 0 && body.jobPartId != 0 && body.jobPartId != jobPartId)
      continue;
    const std::optional<int> idx = body.elementMap.Resolve(ref.name);
    if (!idx) {
      out.why = "that face is gone from the model";
      return out;
    }
    int i = 0;
    for (TopExp_Explorer ex(body.shape.GetShape(), TopAbs_FACE); ex.More();
         ex.Next(), i++) {
      if (i != *idx)
        continue;
      double minZ = 0.0, maxZ = 0.0;
      bool any = false;
      // NO planarity test, and that is the point: this is the one pick that
      // takes a curved or tilted face, which the other two refuse by design.
      const Clipper2Lib::Paths64 paths =
          SilhouetteOf(CADShape(ex.Current()), deflection, minZ, maxZ, any);
      if (!any) {
        out.why = "that face could not be tessellated";
        return out;
      }
      return RegionFromPaths(paths, minZ, maxZ);
    }
    out.why = "that face is gone from the model";
    return out;
  }
  out.why = "nothing in this job has that feature";
  return out;
}

SurfaceRegion WholePartSilhouette(const std::vector<Body>& bodies,
                                  double deflection) {
  SurfaceRegion out;
  Clipper2Lib::Paths64 all;
  double minZ = 0.0, maxZ = 0.0;
  bool any = false;
  for (const Body& body : bodies) {
    if (!body.shape.IsValid())
      continue;
    const Clipper2Lib::Paths64 one =
        SilhouetteOf(body.shape, deflection, minZ, maxZ, any);
    all.insert(all.end(), one.begin(), one.end());
  }
  if (!any) {
    out.why = "there is nothing in this job to finish";
    return out;
  }
  return RegionFromPaths(
      Clipper2Lib::Union(all, Clipper2Lib::FillRule::NonZero), minZ, maxZ);
}

} // namespace cam
