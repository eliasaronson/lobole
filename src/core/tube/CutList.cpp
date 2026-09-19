#include "core/tube/CutList.h"

#include "core/Document.h"
#include "core/features/CurveFeature.h"
#include "core/features/PipeFeature.h"

#include <BRepAdaptor_Curve.hxx>
#include <BRepTools_WireExplorer.hxx>
#include <BRep_Tool.hxx>
#include <GCPnts_AbscissaPoint.hxx>
#include <GCPnts_QuasiUniformDeflection.hxx>
#include <GeomAPI_ProjectPointOnCurve.hxx>
#include <Standard_Failure.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Wire.hxx>

#include <algorithm>
#include <functional>
#include <limits>
#include <set>
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

std::vector<std::pair<glm::dvec3, double>> SampleCurve(const CADShape& pathEdge,
                                                       double deflection) {
  std::vector<std::pair<glm::dvec3, double>> out;
  if (!pathEdge.IsValid() || deflection <= 0.0)
    return out;
  try {
    for (const Segment& seg : SegmentsOf(pathEdge.GetShape())) {
      BRepAdaptor_Curve curve(seg.edge);
      GCPnts_QuasiUniformDeflection sampler(curve, deflection);
      if (!sampler.IsDone())
        continue;
      for (int k = 1; k <= sampler.NbPoints(); k++) {
        const gp_Pnt p = sampler.Value(k);
        const double at = seg.startsAt + GCPnts_AbscissaPoint::Length(
                                             curve, curve.FirstParameter(),
                                             sampler.Parameter(k));
        // Segments meet at a shared point; keep one of each.
        if (!out.empty() && std::abs(out.back().second - at) < 1e-9)
          continue;
        out.push_back({glm::dvec3(p.X(), p.Y(), p.Z()), at});
      }
    }
  } catch (const Standard_Failure&) {
    out.clear();
  }
  return out;
}

std::optional<std::pair<double, double>>
MetalAlongPath(const Body& body, const CADShape& pathEdge) {
  if (!pathEdge.IsValid() || !body.shape.IsValid())
    return std::nullopt;

  try {
    const std::vector<Segment> segs = SegmentsOf(pathEdge.GetShape());
    if (segs.empty())
      return std::nullopt;

    // Sample the body's EDGES, not just its vertices. The far point of a
    // coped end is the heel of the fishmouth, which lies ALONG the cut curve
    // — where its vertices happen to fall is a matter of how the boolean
    // split it, and on a symmetric frame the two sides do not split alike.
    // Measuring vertices alone made a mirrored pair of braces differ by
    // 0.77 mm, which is a real tube cut to the wrong length.
    std::vector<gp_Pnt> probes;
    TopTools_IndexedMapOfShape verts, edges;
    TopExp::MapShapes(body.shape.GetShape(), TopAbs_VERTEX, verts);
    TopExp::MapShapes(body.shape.GetShape(), TopAbs_EDGE, edges);
    for (int i = 1; i <= verts.Extent(); i++)
      probes.push_back(BRep_Tool::Pnt(TopoDS::Vertex(verts(i))));
    for (int i = 1; i <= edges.Extent(); i++) {
      BRepAdaptor_Curve curve(TopoDS::Edge(edges(i)));
      // Deflection-based, so a long straight costs two points and a tight
      // cope curve costs as many as its curvature needs.
      GCPnts_QuasiUniformDeflection sampler(curve, 0.05);
      if (!sampler.IsDone())
        continue;
      for (int k = 1; k <= sampler.NbPoints(); k++)
        probes.push_back(sampler.Value(k));
    }
    if (probes.empty())
      return std::nullopt;

    double lo = 0.0, hi = 0.0;
    bool any = false;
    for (const gp_Pnt& p : probes) {
      // Which point of the WHOLE path this vertex is nearest, in arc length
      // from its start — the parameter would be an angle on an arc and
      // nothing in particular on a spline.
      double best = std::numeric_limits<double>::max();
      double bestAt = 0.0;
      const auto consider = [&](double distance, double at) {
        if (distance < best) {
          best = distance;
          bestAt = at;
        }
      };
      for (const Segment& seg : segs) {
        Standard_Real first = 0.0, last = 0.0;
        const Handle(Geom_Curve) curve =
            BRep_Tool::Curve(seg.edge, first, last);
        if (curve.IsNull())
          continue;
        BRepAdaptor_Curve adaptor(seg.edge);
        GeomAPI_ProjectPointOnCurve proj(p, curve, first, last);
        if (proj.NbPoints() >= 1) {
          consider(proj.LowerDistance(),
                   seg.startsAt +
                       GCPnts_AbscissaPoint::Length(
                           adaptor, first, proj.LowerDistanceParameter()));
        }
        // The segment's ENDS as well. A projection finds only the points
        // where the distance is STATIONARY, and a vertex off the end of a
        // segment has none — so the tube's own end face found no projection
        // on the first leg, fell through to a later one, and a hoop measured
        // 1511 mm of a 2335 mm path: one leg missing, silently.
        consider(p.Distance(curve->Value(first)), seg.startsAt);
        consider(p.Distance(curve->Value(last)), seg.startsAt + seg.length);
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

std::vector<MemberRow> Members(const Document& doc, uint64_t onlyCurve) {
  std::vector<MemberRow> rows;
  for (const Body& body : doc.GetBodies()) {
    // Breadth-first back through the features that made this body until a
    // Pipe turns up.
    const PipeFeature* pipe = nullptr;
    std::vector<uint64_t> frontier = {body.createdBy};
    std::set<uint64_t> seen;
    while (!frontier.empty() && pipe == nullptr) {
      const uint64_t id = frontier.back();
      frontier.pop_back();
      if (id == 0 || !seen.insert(id).second)
        continue;
      const Feature* f = doc.FindFeatureById(id);
      if (f == nullptr)
        continue;
      if (const auto* asPipe = dynamic_cast<const PipeFeature*>(f)) {
        pipe = asPipe;
        break;
      }
      f->VisitFeatureRefs([&](uint64_t ref) { frontier.push_back(ref); });
    }
    if (pipe == nullptr)
      continue; // not a length of tube; nothing to cut or bend
    const auto* curve =
        dynamic_cast<const CurveFeature*>(doc.FindFeatureById(pipe->pathId));
    if (curve == nullptr || curve->kind != Curve3DKind::BentPath)
      continue; // a tube on a sketch or a spline has no former to quote
    if (onlyCurve != 0 && curve->GetId() != onlyCurve)
      continue;

    MemberRow row;
    // The MEMBER's name, not its centreline's. A shop reading "Lower rail L
    // CL" on a cut list has to work out that the tube is called something
    // else.
    row.name = pipe->GetName();
    row.curveId = curve->GetId();
    row.bodies = {body.name};
    row.outerDiameter = pipe->outerDiameter;
    row.wallThickness = pipe->wallThickness;
    row.pathLength = curve->Length();
    row.cutLength = row.pathLength;
    if (const auto span = MetalAlongPath(body, curve->EdgeShape())) {
      row.cutLength = span->second - span->first;
      // The measurement has a noise floor — an arc length integrated along a
      // wire against the curve's own length differ in the last digits — and
      // below a micron there is no cope, there is arithmetic. Reporting 39
      // nanometres of cope on an untouched member is a fact nobody can act
      // on and a difference no saw can make.
      if (row.pathLength - row.cutLength < 1e-3)
        row.cutLength = row.pathLength;
    }
    for (const CurveFeature::Bend& b : curve->Bends())
      row.bends.push_back({b.angleDeg, b.radius, b.straightBefore, b.rollDeg});

    // Identical members are one row with a quantity: four braces authored
    // four ways are still four of the same tube to the shop.
    const auto same = [&](const MemberRow& a) {
      if (a.quantity == 0 || a.bends.size() != row.bends.size())
        return false;
      if (std::abs(a.cutLength - row.cutLength) > kSameLength ||
          std::abs(a.outerDiameter - row.outerDiameter) > 1e-9 ||
          std::abs(a.wallThickness - row.wallThickness) > 1e-9)
        return false;
      for (size_t i = 0; i < a.bends.size(); i++) {
        const Bend& x = a.bends[i];
        const Bend& y = row.bends[i];
        // A bend's ANGLE and FORMER must match exactly — they are what the
        // machine is set to — but where it falls along the tube is measured,
        // so it gets the same slack as the length.
        if (std::abs(x.angleDeg - y.angleDeg) > 1e-6 ||
            std::abs(x.radius - y.radius) > 1e-9 ||
            std::abs(x.straightBefore - y.straightBefore) > kSameLength ||
            std::abs(x.rollDeg - y.rollDeg) > 1e-6)
          return false;
      }
      return true;
    };
    auto it = std::find_if(rows.begin(), rows.end(), same);
    if (it != rows.end()) {
      it->quantity++;
      it->bodies.push_back(body.name);
      // The LONGEST of the group: cut to it and a member can only be long.
      it->cutLength = std::max(it->cutLength, row.cutLength);
    } else {
      rows.push_back(std::move(row));
    }
  }
  return rows;
}

std::vector<Stick> PackIntoStock(std::vector<double> pieces, double stockLength,
                                 double kerf) {
  std::vector<Stick> sticks;
  if (stockLength <= 0.0)
    return sticks;
  std::sort(pieces.begin(), pieces.end(), std::greater<double>());
  for (const double piece : pieces) {
    if (piece > stockLength + 1e-9)
      continue; // longer than the stock: the caller refuses this, not us
    Stick* home = nullptr;
    for (Stick& s : sticks) {
      // The kerf is only spent where a piece FOLLOWS another on the stick.
      const double need = s.pieces.empty() ? piece : kerf + piece;
      if (s.used + need <= stockLength + 1e-9) {
        home = &s;
        break;
      }
    }
    if (home == nullptr) {
      sticks.push_back(Stick{});
      home = &sticks.back();
    }
    home->used += home->pieces.empty() ? piece : kerf + piece;
    home->pieces.push_back(piece);
  }
  for (Stick& s : sticks)
    s.offcut = stockLength - s.used;
  return sticks;
}

} // namespace tube
