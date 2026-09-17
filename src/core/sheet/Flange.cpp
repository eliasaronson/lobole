#include "core/sheet/Flange.h"

#include "core/ElementHistory.h"

#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepAlgoAPI_Common.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <BRep_Tool.hxx>
#include <GC_MakeArcOfCircle.hxx>
#include <Standard_Failure.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedDataMapOfShapeListOfShape.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>

#include <algorithm>
#include <cmath>
#include <optional>

namespace sheet {
namespace {

gp_Pnt At(const EdgeFrame& f, const gp_Pnt& origin, double u, double v,
          double along) {
  return origin.Translated(gp_Vec(f.outward) * u + gp_Vec(f.up) * v +
                           gp_Vec(f.e) * along);
}

// The outward normal of a face of a solid, at its middle parameter.
bool FaceNormal(const TopoDS_Face& face, gp_Dir& out) {
  BRepAdaptor_Surface surf(face);
  if (surf.GetType() != GeomAbs_Plane)
    return false;
  gp_Dir n = surf.Plane().Axis().Direction();
  if (face.Orientation() == TopAbs_REVERSED)
    n.Reverse();
  out = n;
  return true;
}

// The extent of `face` along `dir`, which is how the thin RIM face of a sheet
// is told from the broad PANEL face it shares an edge with: the rim measures
// one thickness across and the panel does not.
double ExtentAlong(const TopoDS_Face& face, const gp_Dir& dir) {
  double lo = 1e30, hi = -1e30;
  TopTools_IndexedMapOfShape verts;
  TopExp::MapShapes(face, TopAbs_VERTEX, verts);
  for (int i = 1; i <= verts.Extent(); ++i) {
    const gp_Pnt p = BRep_Tool::Pnt(TopoDS::Vertex(verts(i)));
    const double d = gp_Vec(p.XYZ()).Dot(gp_Vec(dir));
    lo = std::min(lo, d);
    hi = std::max(hi, d);
  }
  return hi - lo;
}

// Resolve one picked edge to the frame the flange is built in. Every refusal
// here names the thing that is wrong with the pick, because "flange failed" on
// a curved edge or an edge of a non-sheet body sends the caller looking at the
// numbers instead of the selection.
bool ResolveFrameImpl(const Body& body, const ElementRef& ref, double thickness,
                      EdgeFrame& out, std::string& error) {
  const auto edgeIndex = body.elementMap.ResolveEdge(ref.name);
  if (!edgeIndex) {
    error = "edge '" + ref.name + "' no longer exists";
    return false;
  }
  TopTools_IndexedMapOfShape edgeMap;
  TopExp::MapShapes(body.shape.GetShape(), TopAbs_EDGE, edgeMap);
  if (*edgeIndex < 0 || *edgeIndex >= edgeMap.Extent()) {
    error = "edge '" + ref.name + "' is out of range on this body";
    return false;
  }
  const TopoDS_Edge edge = TopoDS::Edge(edgeMap(*edgeIndex + 1));

  BRepAdaptor_Curve curve(edge);
  if (curve.GetType() != GeomAbs_Line) {
    error = "edge '" + ref.name +
            "' is not straight - a brake bends about a line, so a curved "
            "edge needs a contour flange rather than an edge flange";
    return false;
  }
  out.p0 = curve.Value(curve.FirstParameter());
  out.p1 = curve.Value(curve.LastParameter());
  const gp_Vec span(out.p0, out.p1);
  out.length = span.Magnitude();
  if (out.length < 1e-9) {
    error = "edge '" + ref.name + "' has no length";
    return false;
  }
  out.e = gp_Dir(span);

  // The two faces meeting at the edge: the broad panel the flange folds from,
  // and the thin rim it grows out of.
  TopTools_IndexedDataMapOfShapeListOfShape edgeFaces;
  TopExp::MapShapesAndAncestors(body.shape.GetShape(), TopAbs_EDGE, TopAbs_FACE,
                                edgeFaces);
  const int idx = edgeFaces.FindIndex(edge);
  if (idx == 0) {
    error = "edge '" + ref.name + "' belongs to no face";
    return false;
  }
  std::vector<TopoDS_Face> planar;
  for (const TopoDS_Shape& f : edgeFaces.FindFromIndex(idx)) {
    const TopoDS_Face face = TopoDS::Face(f);
    gp_Dir n;
    if (FaceNormal(face, n))
      planar.push_back(face);
  }
  if (planar.size() != 2) {
    error = "edge '" + ref.name +
            "' is not between two flat faces - an edge flange folds a flat "
            "panel along its rim";
    return false;
  }
  gp_Dir n0, n1;
  FaceNormal(planar[0], n0);
  FaceNormal(planar[1], n1);
  // The rim measures one thickness across and the panel does not, so each
  // face's extent along the OTHER's normal tells them apart — including on a
  // part whose panel happens to be only a few millimetres wide.
  const double t0 = ExtentAlong(planar[0], n1);
  const double t1 = ExtentAlong(planar[1], n0);
  // Exactly ONE of them may be a thickness wide. Where two rims meet at a
  // corner both are, and every other check below passes — two flat faces,
  // perpendicular, the edge in both — so without this a flange grows off the
  // short corner edge of the sheet and records a bend one gauge long.
  const double tol = std::max(1e-6, thickness * 1e-6);
  const bool rim0 = std::abs(t0 - thickness) < tol;
  const bool rim1 = std::abs(t1 - thickness) < tol;
  if (rim0 == rim1) {
    error = "edge '" + ref.name +
            "' is not the edge of a panel - pick an edge where the sheet's "
            "broad face meets its rim, not a corner where two rims meet";
    return false;
  }
  const int rimIdx = rim0 ? 0 : 1;
  const int panelIdx = 1 - rimIdx;
  FaceNormal(planar[rimIdx], out.outward);
  FaceNormal(planar[panelIdx], out.up);

  if (std::abs(gp_Vec(out.outward).Dot(gp_Vec(out.up))) > 1e-6) {
    error = "edge '" + ref.name +
            "' is not a square sheet rim (its rim and panel are not "
            "perpendicular)";
    return false;
  }
  if (std::abs(gp_Vec(out.e).Dot(gp_Vec(out.up))) > 1e-6 ||
      std::abs(gp_Vec(out.e).Dot(gp_Vec(out.outward))) > 1e-6) {
    error = "edge '" + ref.name + "' does not lie in its panel";
    return false;
  }
  return true;
}

// The bend-plus-wall cross-section, swept along the edge. Built as a real
// profile rather than a box plus a filleted corner so the bend is an analytic
// cylinder of the radius asked for — which is what a STEP consumer, and S4's
// unfolder, read back.
CADShape BuildFlange(const EdgeFrame& f, const gp_Pnt& origin, double radius,
                     double theta, double t, double wall, double span,
                     std::string& error) {
  const double sinT = std::sin(theta), cosT = std::cos(theta);
  // Inner surface of the bend, radius `radius` about an axis one radius above
  // the panel's picked face; the outer surface is the same axis at radius+t.
  const auto inner = [&](double phi) {
    return At(f, origin, radius * std::sin(phi),
              radius - radius * std::cos(phi), 0.0);
  };
  const auto outer = [&](double phi) {
    return At(f, origin, (radius + t) * std::sin(phi),
              radius - (radius + t) * std::cos(phi), 0.0);
  };
  const gp_Vec dir = gp_Vec(f.outward) * cosT + gp_Vec(f.up) * sinT;

  const gp_Pnt i0 = inner(0.0), i1 = inner(theta);
  const gp_Pnt o0 = outer(0.0), o1 = outer(theta);
  const gp_Pnt ti = i1.Translated(dir * wall);
  const gp_Pnt to = o1.Translated(dir * wall);

  try {
    BRepBuilderAPI_MakeWire wire;
    const Handle(Geom_TrimmedCurve) innerArc =
        GC_MakeArcOfCircle(i0, inner(theta / 2.0), i1).Value();
    const Handle(Geom_TrimmedCurve) outerArc =
        GC_MakeArcOfCircle(o1, outer(theta / 2.0), o0).Value();
    wire.Add(BRepBuilderAPI_MakeEdge(innerArc).Edge());
    wire.Add(BRepBuilderAPI_MakeEdge(i1, ti).Edge());
    wire.Add(BRepBuilderAPI_MakeEdge(ti, to).Edge());
    wire.Add(BRepBuilderAPI_MakeEdge(to, o1).Edge());
    wire.Add(BRepBuilderAPI_MakeEdge(outerArc).Edge());
    wire.Add(BRepBuilderAPI_MakeEdge(o0, i0).Edge());
    if (!wire.IsDone()) {
      error = "the flange cross-section did not close";
      return {};
    }
    BRepBuilderAPI_MakeFace face(wire.Wire(), Standard_True);
    if (!face.IsDone()) {
      error = "the flange cross-section is not planar";
      return {};
    }
    BRepPrimAPI_MakePrism prism(face.Face(), gp_Vec(f.e) * span);
    if (!prism.IsDone()) {
      error = "the flange could not be swept along the edge";
      return {};
    }
    CADShape out(prism.Shape());
    if (!out.IsValid() || out.Volume() < 1e-12) {
      error = "the flange swept to an empty solid";
      return {};
    }
    return out;
  } catch (const Standard_Failure& e) {
    error = std::string("flange sweep failed: ") + e.GetMessageString();
    return {};
  }
}

// A box in the edge's frame, for the setback cut and for relief slots.
//
// Built as a prism of a rectangle rather than a gp_Ax2 box: {outward, up, e}
// is orthonormal but NOT necessarily right-handed, so an Ax2's implied third
// axis runs along +e on some edges of a part and -e on others — a cut that
// lands on the wrong side of the flange on half the picks.
CADShape FrameBox(const EdgeFrame& f, const gp_Pnt& origin, double u0,
                  double u1, double v0, double v1, double a0, double a1) {
  const gp_Pnt c00 = At(f, origin, u0, v0, a0);
  const gp_Pnt c10 = At(f, origin, u1, v0, a0);
  const gp_Pnt c11 = At(f, origin, u1, v1, a0);
  const gp_Pnt c01 = At(f, origin, u0, v1, a0);
  BRepBuilderAPI_MakeWire wire(BRepBuilderAPI_MakeEdge(c00, c10).Edge(),
                               BRepBuilderAPI_MakeEdge(c10, c11).Edge(),
                               BRepBuilderAPI_MakeEdge(c11, c01).Edge(),
                               BRepBuilderAPI_MakeEdge(c01, c00).Edge());
  BRepBuilderAPI_MakeFace face(wire.Wire(), Standard_True);
  BRepPrimAPI_MakePrism prism(face.Face(), gp_Vec(f.e) * (a1 - a0));
  return CADShape(prism.Shape());
}

struct Plan {
  size_t body = 0;
  ElementRef edge;
  EdgeFrame frame;
  gp_Pnt origin; // the tangent line, at the edge's start
  double a0 = 0.0, a1 = 0.0;
  CADShape solid;
  std::vector<BendRegion::Trim> trims;
};

CADShape KeepSide(const CADShape& s, const gp_Pnt& at, const gp_Dir& n,
                  double size) {
  const gp_Dir a = std::abs(n.Z()) < 0.9
                       ? gp_Dir(gp_Vec(n).Crossed(gp_Vec(0, 0, 1)))
                       : gp_Dir(gp_Vec(n).Crossed(gp_Vec(1, 0, 0)));
  const gp_Dir b(gp_Vec(n).Crossed(gp_Vec(a)));
  const gp_Vec va = gp_Vec(a) * size, vb = gp_Vec(b) * size;
  const gp_Pnt base = at.Translated(-va - vb);
  BRepBuilderAPI_MakeWire wire(
      BRepBuilderAPI_MakeEdge(base, base.Translated(va * 2.0)).Edge(),
      BRepBuilderAPI_MakeEdge(base.Translated(va * 2.0),
                              base.Translated(va * 2.0 + vb * 2.0))
          .Edge(),
      BRepBuilderAPI_MakeEdge(base.Translated(va * 2.0 + vb * 2.0),
                              base.Translated(vb * 2.0))
          .Edge(),
      BRepBuilderAPI_MakeEdge(base.Translated(vb * 2.0), base).Edge());
  const CADShape half(
      BRepPrimAPI_MakePrism(
          BRepBuilderAPI_MakeFace(wire.Wire(), Standard_True).Face(),
          gp_Vec(n) * size)
          .Shape());
  BRepAlgoAPI_Common op(s.GetShape(), half.GetShape());
  op.SetNonDestructive(Standard_True);
  return op.IsDone() ? CADShape(op.Shape()) : CADShape{};
}

double OverlapVolume(const CADShape& a, const CADShape& b) {
  BRepAlgoAPI_Common op(a.GetShape(), b.GetShape());
  op.SetNonDestructive(Standard_True);
  if (!op.IsDone())
    return 0.0;
  const CADShape c(op.Shape());
  return c.IsValid() ? c.Volume() : 0.0;
}

bool Coincide(const gp_Pnt& a, const gp_Pnt& b) { return a.Distance(b) < 1e-6; }

} // namespace

bool ResolveEdgeFrame(const Body& body, const ElementRef& ref, double thickness,
                      EdgeFrame& out, std::string& error) {
  return ResolveFrameImpl(body, ref, thickness, out, error);
}

std::string ApplyFlanges(std::vector<Body>& bodies,
                         const std::vector<ElementRef>& edges,
                         const FlangeSpec& spec, const SheetMetalRules& rules,
                         uint64_t featureId, double miterGap) {
  const double t = rules.thickness;
  const double R = spec.radius;
  ArmOccSignalHandlers();
  try {
    // ---- 1. Every flange's frame and solid, against the bodies as they
    // came in: mitring needs to see all of them before any is joined.
    std::vector<Plan> plans;
    for (const ElementRef& ref : edges) {
      const auto body =
          std::find_if(bodies.begin(), bodies.end(), [&](const Body& b) {
            return b.createdBy == ref.featureId;
          });
      if (body == bodies.end())
        return "the sheet body this flange grows from no longer exists";
      Plan plan;
      plan.body = static_cast<size_t>(body - bodies.begin());
      plan.edge = ref;
      std::string why;
      if (!ResolveFrameImpl(*body, ref, t, plan.frame, why))
        return why;
      plan.a0 = spec.offsetStart;
      plan.a1 = plan.frame.length - spec.offsetEnd;
      if (plan.a1 - plan.a0 < 1e-9)
        return "the flange's insets (" + std::to_string(spec.offsetStart) +
               " + " + std::to_string(spec.offsetEnd) +
               " mm) leave nothing of a " + std::to_string(plan.frame.length) +
               " mm edge to bend";
      plan.origin =
          plan.frame.p0.Translated(gp_Vec(plan.frame.outward) * -spec.setback);
      plan.solid = BuildFlange(
          plan.frame, At(plan.frame, plan.origin, 0.0, 0.0, plan.a0), R,
          spec.angle, t, spec.wall, plan.a1 - plan.a0, why);
      if (!plan.solid.IsValid())
        return why;
      plans.push_back(std::move(plan));
    }

    // The corner two plans share, if any, and each edge's direction AWAY
    // from it, into its own span.
    struct Corner {
      gp_Pnt at;
      gp_Vec di, dj;
      bool iStart = false, jStart = false;
    };
    const auto cornerOf = [](const Plan& a,
                             const Plan& b) -> std::optional<Corner> {
      for (const gp_Pnt& va : {a.frame.p0, a.frame.p1})
        for (const gp_Pnt& vb : {b.frame.p0, b.frame.p1})
          if (Coincide(va, vb)) {
            Corner c;
            c.at = va;
            c.iStart = Coincide(va, a.frame.p0);
            c.jStart = Coincide(vb, b.frame.p0);
            c.di = gp_Vec(va, c.iStart ? a.frame.p1 : a.frame.p0);
            c.dj = gp_Vec(vb, c.jStart ? b.frame.p1 : b.frame.p0);
            c.di.Normalize();
            c.dj.Normalize();
            return c;
          }
      return std::nullopt;
    };

    // ---- 1b. Closing corners: an open corner — two flanges that only touch
    // at a point — is closed by carrying each flange past the corner as far
    // as the other one sticks out there. The mitre below then cuts them
    // apart along the bisector, exactly as it does flanges that overlapped
    // to begin with.
    if (spec.closedCorners) {
      std::vector<double> extStart(plans.size(), 0.0);
      std::vector<double> extEnd(plans.size(), 0.0);
      const auto reach = [](const CADShape& solid, const gp_Pnt& from,
                            const gp_Vec& dir) {
        double far_ = 0.0;
        for (TopExp_Explorer ex(solid.GetShape(), TopAbs_VERTEX); ex.More();
             ex.Next())
          far_ = std::max(
              far_, gp_Vec(from, BRep_Tool::Pnt(TopoDS::Vertex(ex.Current())))
                        .Dot(dir));
        return far_;
      };
      for (size_t i = 0; i < plans.size(); ++i) {
        for (size_t j = i + 1; j < plans.size(); ++j) {
          if (plans[i].body != plans[j].body)
            continue;
          const auto c = cornerOf(plans[i], plans[j]);
          if (!c)
            continue;
          const double tol = 1e-9 * std::max(1.0, plans[i].solid.Volume());
          if (OverlapVolume(plans[i].solid, plans[j].solid) > tol)
            continue; // already overlapping: the mitre alone closes it
          if (std::abs(c->di.Dot(c->dj)) > 1e-6)
            return "only a square corner can be closed - these two edges "
                   "meet at another angle";
          // An end the user inset is meant to stop short; leave it alone.
          const double ei = reach(plans[j].solid, c->at, -c->di);
          const double ej = reach(plans[i].solid, c->at, -c->dj);
          if (c->iStart ? spec.offsetStart <= 1e-9 : spec.offsetEnd <= 1e-9)
            (c->iStart ? extStart[i] : extEnd[i]) =
                std::max(c->iStart ? extStart[i] : extEnd[i], ei);
          if (c->jStart ? spec.offsetStart <= 1e-9 : spec.offsetEnd <= 1e-9)
            (c->jStart ? extStart[j] : extEnd[j]) =
                std::max(c->jStart ? extStart[j] : extEnd[j], ej);
        }
      }
      for (size_t i = 0; i < plans.size(); ++i) {
        if (extStart[i] <= 0.0 && extEnd[i] <= 0.0)
          continue;
        Plan& plan = plans[i];
        plan.a0 -= extStart[i];
        plan.a1 += extEnd[i];
        std::string why;
        plan.solid = BuildFlange(
            plan.frame, At(plan.frame, plan.origin, 0.0, 0.0, plan.a0), R,
            spec.angle, t, spec.wall, plan.a1 - plan.a0, why);
        if (!plan.solid.IsValid())
          return why;
      }
    }

    // ---- 2. Mitre every pair that meets at a corner AND overlaps. Two
    // flanges set back into the panel both fill the corner square; left
    // alone they fuse into a solid block, and the flat pattern cannot say
    // which bend the block belongs to. Flanges that only touch at a point —
    // bent outside the panel — are left as an open corner.
    for (size_t i = 0; i < plans.size(); ++i) {
      for (size_t j = i + 1; j < plans.size(); ++j) {
        Plan& pi = plans[i];
        Plan& pj = plans[j];
        if (pi.body != pj.body)
          continue;
        const auto c = cornerOf(pi, pj);
        if (!c)
          continue;
        const gp_Pnt corner = c->at;
        const double tol = 1e-9 * std::max(1.0, pi.solid.Volume());
        if (OverlapVolume(pi.solid, pj.solid) <= tol)
          continue;
        gp_Vec split = c->di - c->dj;
        if (split.Magnitude() < 1e-9)
          return "two picked edges run the same way from one corner, so "
                 "there is no mitre between them";
        const gp_Dir n(split);
        const double size =
            (pi.frame.length + pj.frame.length + spec.wall + R + t) * 4.0;
        const gp_Pnt keepI = corner.Translated(gp_Vec(n) * (miterGap / 2.0));
        const gp_Pnt keepJ = corner.Translated(gp_Vec(n) * -(miterGap / 2.0));
        pi.solid = KeepSide(pi.solid, keepI, n, size);
        pj.solid = KeepSide(pj.solid, keepJ, n.Reversed(), size);
        if (!pi.solid.IsValid() || !pj.solid.IsValid())
          return "a mitred corner could not be cut";
        pi.trims.push_back(
            {{keepI.X(), keepI.Y(), keepI.Z()}, {n.X(), n.Y(), n.Z()}});
        pj.trims.push_back(
            {{keepJ.X(), keepJ.Y(), keepJ.Z()}, {-n.X(), -n.Y(), -n.Z()}});
      }
    }

    // Work on copies, so a failure half way leaves the bodies untouched.
    std::vector<Body> work = bodies;
    const double eps = std::max(t, 1.0) * 1e-3;

    // ---- 3. Every panel cut before any flange is joined: a second flange's
    // setback cut must never bite the first flange's bend at a shared corner.
    for (const Plan& plan : plans) {
      Body& body = work[plan.body];
      std::vector<CADShape> tools;
      // Cut the panel back to the tangent line over the flange's span —
      // EXACTLY to it. Starting the cut even a micron behind the line leaves
      // the panel and the bend not touching, and the body two solids, while
      // every volume still reads right to five figures. At bend_outside there
      // is nothing to cut — the bend starts at the edge.
      if (spec.setback > 1e-9)
        tools.push_back(FrameBox(plan.frame, plan.origin, 0.0,
                                 spec.setback + eps, -t - eps, eps, plan.a0,
                                 plan.a1));
      // Relief: where the bend line stops INSIDE the panel, the metal needs a
      // cut to move into or it tears the panel beside it. `tear` and `none`
      // cut nothing — one is a decision, the other a claim that the bend
      // line runs edge to edge.
      const bool cuts = spec.relief == ReliefStyle::Rectangular ||
                        spec.relief == ReliefStyle::Obround;
      for (int end = 0; cuts && end < 2; ++end) {
        const double inset = end == 0 ? spec.offsetStart : spec.offsetEnd;
        if (inset <= 1e-9)
          continue; // the bend line reaches the panel's own edge here
        const double w = spec.reliefWidth;
        const double s0 = end == 0 ? plan.a0 - w : plan.a1;
        const double s1 = end == 0 ? plan.a0 : plan.a1 + w;
        // `origin` is the TANGENT LINE, so the relief has to reach
        // reliefDepth back INTO the panel (negative u) and then out over
        // whatever the setback removed. Measured from the setback line
        // instead, a material_inside flange — the default — got a notch
        // exactly as deep as its own setback, which stops AT the tangent
        // line and relieves nothing: the metal still tears from the corner
        // of the cut, and check_sheet_metal said so while the part claimed
        // to have relief.
        const double back = -spec.reliefDepth;
        if (spec.relief == ReliefStyle::Obround) {
          tools.push_back(FrameBox(plan.frame, plan.origin, back + w / 2.0,
                                   spec.setback + eps, -t - eps, eps, s0, s1));
          const gp_Pnt centre =
              At(plan.frame, plan.origin, back + w / 2.0, eps, (s0 + s1) / 2.0);
          tools.push_back(CADShape(
              BRepPrimAPI_MakeCylinder(gp_Ax2(centre, plan.frame.up.Reversed()),
                                       w / 2.0, t + 2.0 * eps)
                  .Shape()));
        } else {
          tools.push_back(FrameBox(plan.frame, plan.origin, back,
                                   spec.setback + eps, -t - eps, eps, s0, s1));
        }
      }
      for (const CADShape& tool : tools) {
        ElementMap toolMap, outMap;
        std::string why;
        CADShape cut =
            topo::BooleanNamed(BooleanOp::Subtract, body.shape, body.elementMap,
                               tool, toolMap, featureId, outMap, &why);
        if (!cut.IsValid())
          return why.empty() ? "the panel could not be cut for the flange"
                             : why;
        body.shape = std::move(cut);
        body.elementMap = std::move(outMap);
      }
    }

    // ---- 4. Join each flange, and record its bend. The record is what the
    // flat pattern is computed FROM; the geometry is only the picture of it.
    for (size_t k = 0; k < plans.size(); ++k) {
      const Plan& plan = plans[k];
      Body& body = work[plan.body];
      // Named before the fuse so the bend's cylinders are referenceable
      // afterwards; one prefix PER FLANGE, or two edges of one feature give
      // their faces the same names.
      ElementMap flangeMap;
      topo::NamePrimitiveElements(
          plan.solid, std::to_string(featureId) + "/f" + std::to_string(k),
          flangeMap);
      ElementMap fusedMap;
      std::string why;
      CADShape fused =
          topo::BooleanNamed(BooleanOp::Union, body.shape, body.elementMap,
                             plan.solid, flangeMap, featureId, fusedMap, &why);
      if (!fused.IsValid() ||
          fused.Volume() < body.shape.Volume() * (1.0 - 1e-9))
        return why.empty() ? "the flange would not join the panel" : why;

      BendRegion bend;
      bend.featureId = featureId;
      const gp_Pnt axisPt = At(plan.frame, plan.origin, 0.0, R, plan.a0);
      bend.axisOrigin = {axisPt.X(), axisPt.Y(), axisPt.Z()};
      bend.axisDir = {plan.frame.e.X(), plan.frame.e.Y(), plan.frame.e.Z()};
      bend.radius = R;
      bend.crushed = spec.crushed;
      bend.width = plan.a1 - plan.a0;
      bend.angle = spec.angle;
      bend.kFactor = rules.EffectiveKFactor(R);
      bend.kSource = rules.KSource(R);
      bend.parentFace = plan.edge;
      bend.parentDir = {plan.frame.outward.X(), plan.frame.outward.Y(),
                        plan.frame.outward.Z()};
      bend.upDir = {plan.frame.up.X(), plan.frame.up.Y(), plan.frame.up.Z()};
      bend.trims = plan.trims;
      int fi = 0;
      for (TopExp_Explorer ex(plan.solid.GetShape(), TopAbs_FACE); ex.More();
           ex.Next(), ++fi) {
        BRepAdaptor_Surface surf(TopoDS::Face(ex.Current()));
        if (surf.GetType() != GeomAbs_Cylinder)
          continue;
        const double rad = surf.Cylinder().Radius();
        const std::string* name = flangeMap.NameOf(fi);
        if (name == nullptr)
          continue;
        if (std::abs(rad - R) < 1e-6)
          bend.innerFace = ElementRef{plan.edge.featureId, *name};
        else if (std::abs(rad - (R + t)) < 1e-6)
          bend.outerFace = ElementRef{plan.edge.featureId, *name};
      }
      body.shape = std::move(fused);
      body.elementMap = std::move(fusedMap);
      body.bends.push_back(std::move(bend));
    }
    bodies = std::move(work);
    return {};
  } catch (const Standard_Failure& e) {
    return std::string("flange failed: ") + e.GetMessageString();
  }
}

} // namespace sheet
