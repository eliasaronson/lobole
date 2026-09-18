#include "core/tube/CutList.h"

#include <BRepAdaptor_Curve.hxx>
#include <BRepTools_WireExplorer.hxx>
#include <BRep_Tool.hxx>
#include <GCPnts_AbscissaPoint.hxx>
#include <GeomAPI_ProjectPointOnCurve.hxx>
#include <Standard_Failure.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Wire.hxx>

#include <algorithm>
#include <limits>
#include <vector>

namespace tube {
namespace {

// One edge of the path, with where it starts along the whole path. A bent
// path is line-arc-line-arc-line, so measuring along the first edge alone
// answers the length of the first straight — 15 mm for a handle whose tube is
// 481 mm, which is how this was caught.
struct Segment {
  TopoDS_Edge edge;
  double startsAt = 0.0; // arc length from the path's start
  double length = 0.0;
};

std::vector<Segment> SegmentsOf(const TopoDS_Shape& path) {
  std::vector<Segment> segs;
  double at = 0.0;
  const auto push = [&](const TopoDS_Edge& e) {
    BRepAdaptor_Curve adaptor(e);
    Segment s;
    s.edge = e;
    s.startsAt = at;
    s.length = GCPnts_AbscissaPoint::Length(adaptor);
    at += s.length;
    segs.push_back(std::move(s));
  };
  // In CONNECTION order where the path is a wire — an explorer's order is not
  // promised to be the order you travel it.
  if (path.ShapeType() == TopAbs_WIRE) {
    for (BRepTools_WireExplorer it(TopoDS::Wire(path)); it.More(); it.Next())
      push(it.Current());
    return segs;
  }
  for (TopExp_Explorer it(path, TopAbs_EDGE); it.More(); it.Next())
    push(TopoDS::Edge(it.Current()));
  return segs;
}

} // namespace

std::optional<std::pair<double, double>>
MetalAlongPath(const Body& body, const CADShape& pathEdge) {
  if (!pathEdge.IsValid() || !body.shape.IsValid())
    return std::nullopt;

  try {
    const std::vector<Segment> segs = SegmentsOf(pathEdge.GetShape());
    if (segs.empty())
      return std::nullopt;

    TopTools_IndexedMapOfShape verts;
    TopExp::MapShapes(body.shape.GetShape(), TopAbs_VERTEX, verts);
    if (verts.IsEmpty())
      return std::nullopt;

    double lo = 0.0, hi = 0.0;
    bool any = false;
    for (int i = 1; i <= verts.Extent(); i++) {
      const gp_Pnt p = BRep_Tool::Pnt(TopoDS::Vertex(verts(i)));
      // Which point of the WHOLE path this vertex is nearest, in arc length
      // from its start — the parameter would be an angle on an arc and
      // nothing in particular on a spline.
      double best = std::numeric_limits<double>::max();
      double bestAt = 0.0;
      for (const Segment& seg : segs) {
        Standard_Real first = 0.0, last = 0.0;
        const Handle(Geom_Curve) curve =
            BRep_Tool::Curve(seg.edge, first, last);
        if (curve.IsNull())
          continue;
        GeomAPI_ProjectPointOnCurve proj(p, curve, first, last);
        if (proj.NbPoints() < 1)
          continue;
        if (proj.LowerDistance() < best) {
          best = proj.LowerDistance();
          BRepAdaptor_Curve adaptor(seg.edge);
          bestAt = seg.startsAt +
                   GCPnts_AbscissaPoint::Length(adaptor, first,
                                                proj.LowerDistanceParameter());
        }
      }
      if (best == std::numeric_limits<double>::max())
        continue;
      if (!any) {
        lo = hi = bestAt;
        any = true;
      } else {
        lo = std::min(lo, bestAt);
        hi = std::max(hi, bestAt);
      }
    }
    if (!any)
      return std::nullopt;
    return std::make_pair(lo, hi);
  } catch (const Standard_Failure&) {
    return std::nullopt;
  }
}

} // namespace tube
