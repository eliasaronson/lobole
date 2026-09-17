#include "core/features/EdgeFlangeFeature.h"

#include "core/Angles.h"
#include "core/ElementHistory.h"

#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
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
#include <numbers>

namespace {

// The local frame of a flangeable edge. `e` runs from `p0` to `p1`; `outward`
// is the rim's own outward normal, so it points away from the material along
// the panel; `up` is the outward normal of the PICKED panel face, which is
// what decides which way the wall folds — picking the edge on the far face of
// the same rim bends the other way.
struct EdgeFrame {
  gp_Pnt p0, p1;
  gp_Dir e, outward, up;
  double length = 0.0;
};

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

} // namespace

std::string FlangePositionName(FlangePosition p) {
  switch (p) {
  case FlangePosition::MaterialInside:
    return "material_inside";
  case FlangePosition::MaterialOutside:
    return "material_outside";
  case FlangePosition::BendOutside:
    return "bend_outside";
  }
  return "material_inside";
}

bool FlangePositionFromString(const std::string& name, FlangePosition& out) {
  if (name == "material_inside") {
    out = FlangePosition::MaterialInside;
    return true;
  }
  if (name == "material_outside") {
    out = FlangePosition::MaterialOutside;
    return true;
  }
  if (name == "bend_outside") {
    out = FlangePosition::BendOutside;
    return true;
  }
  return false;
}

const std::string& FlangePositionList() {
  static const std::string list =
      "material_inside, material_outside, bend_outside";
  return list;
}

std::string FlangeLengthModeName(FlangeLengthMode m) {
  switch (m) {
  case FlangeLengthMode::Flat:
    return "flat";
  case FlangeLengthMode::Outside:
    return "outside";
  case FlangeLengthMode::Inside:
    return "inside";
  }
  return "flat";
}

bool FlangeLengthModeFromString(const std::string& name,
                                FlangeLengthMode& out) {
  if (name == "flat") {
    out = FlangeLengthMode::Flat;
    return true;
  }
  if (name == "outside") {
    out = FlangeLengthMode::Outside;
    return true;
  }
  if (name == "inside") {
    out = FlangeLengthMode::Inside;
    return true;
  }
  return false;
}

const std::string& FlangeLengthModeList() {
  static const std::string list = "flat, outside, inside";
  return list;
}

EdgeFlangeFeature::EdgeFlangeFeature(const std::string& name,
                                     std::vector<ElementRef> edges,
                                     double length, double angleDeg)
    : edges(std::move(edges)), length(length), angleDeg(angleDeg) {
  m_name = name;
}

namespace {

// Resolve one picked edge to the frame the flange is built in. Every refusal
// here names the thing that is wrong with the pick, because "flange failed" on
// a curved edge or an edge of a non-sheet body sends the caller looking at the
// numbers instead of the selection.
bool ResolveFrame(const Body& body, const ElementRef& ref, double thickness,
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
  const int rimIdx =
      std::abs(t0 - thickness) < std::abs(t1 - thickness) ? 0 : 1;
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

} // namespace

bool EdgeFlangeFeature::Execute(RegenContext& ctx) {
  if (ctx.sheetMetal == nullptr) {
    SetError("this part has no sheet-metal rules - set the material and gauge "
             "first (set_sheet_metal_rules)");
    return false;
  }
  if (edges.empty()) {
    SetError("no edges picked to flange");
    return false;
  }
  const sheet::SheetMetalRules& rules = *ctx.sheetMetal;
  const double t = rules.thickness;
  const double theta = angles::ToRadians(angleDeg);
  if (theta <= 1e-6 || theta > angles::kPi + 1e-9) {
    SetError("bend angle must be above 0 and at most 180 degrees (got " +
             std::to_string(angleDeg) + ")");
    return false;
  }
  const double R = radius > 0.0 ? radius : rules.EffectiveBendRadius();
  const double minR = sheet::MinBendRadius(rules.material, t);
  if (R < minR - 1e-9) {
    SetError("bend radius " + std::to_string(R) + " mm is tighter than " +
             rules.material + " takes at " + rules.gauge + " (minimum " +
             std::to_string(minR) + " mm)");
    return false;
  }

  // The setback from the picked edge to the bend's tangent line — the whole
  // of what `position` means.
  const double halfTan = std::tan(theta / 2.0);
  double setback = 0.0;
  switch (position) {
  case FlangePosition::MaterialInside:
    setback = (R + t) * halfTan;
    break;
  case FlangePosition::MaterialOutside:
    setback = R * halfTan;
    break;
  case FlangePosition::BendOutside:
    setback = 0.0;
    break;
  }

  // `length` may be stated to either mold line; the wall itself is what gets
  // built, so convert once here.
  double wall = length;
  if (lengthMode == FlangeLengthMode::Outside)
    wall = length - (R + t) * halfTan;
  else if (lengthMode == FlangeLengthMode::Inside)
    wall = length - R * halfTan;
  if (wall <= 1e-9) {
    SetError("flange length " + std::to_string(length) + " mm (" +
             FlangeLengthModeName(lengthMode) +
             ") leaves no wall past the bend - at radius " + std::to_string(R) +
             " mm the bend alone needs more than that");
    return false;
  }

  const sheet::ReliefStyle relief = reliefStyle.value_or(rules.reliefStyle);
  const double reliefW = rules.EffectiveReliefWidth();
  const double reliefD = rules.EffectiveReliefDepth(R);

  ArmOccSignalHandlers();

  for (const ElementRef& ref : edges) {
    const auto body =
        std::find_if(ctx.bodies.begin(), ctx.bodies.end(), [&](const Body& b) {
          return b.createdBy == ref.featureId;
        });
    if (body == ctx.bodies.end()) {
      SetError("the sheet body this flange grows from no longer exists");
      return false;
    }
    EdgeFrame frame;
    std::string why;
    if (!ResolveFrame(*body, ref, t, frame, why)) {
      SetError(why);
      return false;
    }
    const double a0 = offsetStart;
    const double a1 = frame.length - offsetEnd;
    if (a1 - a0 < 1e-9) {
      SetError("the flange's insets (" + std::to_string(offsetStart) + " + " +
               std::to_string(offsetEnd) + " mm) leave nothing of a " +
               std::to_string(frame.length) + " mm edge to bend");
      return false;
    }

    // The tangent line: setback back along the panel from the picked edge,
    // at the panel's picked face.
    const gp_Pnt origin = frame.p0.Translated(gp_Vec(frame.outward) * -setback);

    std::string buildErr;
    CADShape flange = BuildFlange(frame, At(frame, origin, 0.0, 0.0, a0), R,
                                  theta, t, wall, a1 - a0, buildErr);
    if (!flange.IsValid()) {
      SetError(buildErr);
      return false;
    }

    CADShape working = body->shape;
    ElementMap workingMap = body->elementMap;
    const double eps = std::max(t, 1.0) * 1e-3;

    // Cut the panel back to the tangent line over the flange's span. At
    // bend_outside there is nothing to cut — the bend starts at the edge.
    if (setback > 1e-9) {
      ElementMap boxMap, cutMap;
      const CADShape box =
          FrameBox(frame, origin, -eps, setback + eps, -t - eps, eps, a0, a1);
      std::string cutErr;
      CADShape cut =
          topo::BooleanNamed(BooleanOp::Subtract, working, workingMap, box,
                             boxMap, GetId(), cutMap, &cutErr);
      if (!cut.IsValid()) {
        SetError(cutErr.empty()
                     ? "could not set the panel back to the bend line"
                     : cutErr);
        return false;
      }
      working = std::move(cut);
      workingMap = std::move(cutMap);
    }

    // Relief: where the bend line stops INSIDE the panel, the metal needs a
    // cut to move into or it tears the panel beside it. `tear` and `none` cut
    // nothing — one is a decision, the other a claim that the bend line runs
    // edge to edge.
    const bool cuts = relief == sheet::ReliefStyle::Rectangular ||
                      relief == sheet::ReliefStyle::Obround;
    if (cuts) {
      for (int end = 0; end < 2; ++end) {
        const double inset = end == 0 ? offsetStart : offsetEnd;
        if (inset <= 1e-9)
          continue; // the bend line reaches the panel's own edge here
        const double s0 = end == 0 ? a0 - reliefW : a1;
        const double s1 = end == 0 ? a0 : a1 + reliefW;
        std::vector<CADShape> tools;
        const double back = setback - reliefD;
        if (relief == sheet::ReliefStyle::Obround) {
          tools.push_back(FrameBox(frame, origin, back + reliefW / 2.0,
                                   setback + eps, -t - eps, eps, s0, s1));
          const gp_Pnt centre =
              At(frame, origin, back + reliefW / 2.0, eps, (s0 + s1) / 2.0);
          tools.push_back(CADShape(
              BRepPrimAPI_MakeCylinder(gp_Ax2(centre, frame.up.Reversed()),
                                       reliefW / 2.0, t + 2.0 * eps)
                  .Shape()));
        } else {
          tools.push_back(FrameBox(frame, origin, back, setback + eps, -t - eps,
                                   eps, s0, s1));
        }
        for (const CADShape& tool : tools) {
          ElementMap toolMap, outMap;
          std::string cutErr;
          CADShape cut =
              topo::BooleanNamed(BooleanOp::Subtract, working, workingMap, tool,
                                 toolMap, GetId(), outMap, &cutErr);
          if (!cut.IsValid()) {
            SetError(cutErr.empty() ? "bend relief could not be cut" : cutErr);
            return false;
          }
          working = std::move(cut);
          workingMap = std::move(outMap);
        }
      }
    }

    // Name the flange's own faces before the fuse, so the bend's cylinders
    // are referenceable afterwards: a prism's topology does not change with
    // its dimensions, which is what makes primitive-style names stable here.
    ElementMap flangeMap;
    topo::NamePrimitiveElements(flange, std::to_string(GetId()) + "/f",
                                flangeMap);
    std::string fuseErr;
    ElementMap fusedMap;
    CADShape fused =
        topo::BooleanNamed(BooleanOp::Union, working, workingMap, flange,
                           flangeMap, GetId(), fusedMap, &fuseErr);
    if (!fused.IsValid()) {
      SetError(fuseErr.empty() ? "the flange would not join the panel"
                               : fuseErr);
      return false;
    }

    // Record the bend. This is what the flat pattern is computed FROM — the
    // geometry is only the picture of it.
    BendRegion bend;
    bend.featureId = GetId();
    const gp_Pnt axisPt = At(frame, origin, 0.0, R, a0);
    bend.axisOrigin = {axisPt.X(), axisPt.Y(), axisPt.Z()};
    bend.axisDir = {frame.e.X(), frame.e.Y(), frame.e.Z()};
    bend.radius = R;
    bend.width = a1 - a0;
    bend.angle = theta;
    // The sense of the fold about the axis, stated frame-independently: the
    // wall turns from `outward` toward `up`, so the rotation is right-handed
    // about the axis exactly when outward x up runs with it.
    bend.up =
        gp_Vec(frame.outward).Crossed(gp_Vec(frame.up)).Dot(gp_Vec(frame.e)) >
        0.0;
    bend.kFactor = rules.EffectiveKFactor(R);
    bend.kSource = rules.KSource(R);
    bend.parentFace = ref;
    bend.parentDir = {frame.outward.X(), frame.outward.Y(), frame.outward.Z()};
    bend.upDir = {frame.up.X(), frame.up.Y(), frame.up.Z()};
    // The two cylindrical faces of the bend, found by their radii on the
    // flange we just named.
    int fi = 0;
    for (TopExp_Explorer ex(flange.GetShape(), TopAbs_FACE); ex.More();
         ex.Next(), ++fi) {
      BRepAdaptor_Surface surf(TopoDS::Face(ex.Current()));
      if (surf.GetType() != GeomAbs_Cylinder)
        continue;
      const double rad = surf.Cylinder().Radius();
      const std::string* name = flangeMap.NameOf(fi);
      if (name == nullptr)
        continue;
      if (std::abs(rad - R) < 1e-6)
        bend.innerFace = ElementRef{ref.featureId, *name};
      else if (std::abs(rad - (R + t)) < 1e-6)
        bend.outerFace = ElementRef{ref.featureId, *name};
    }

    body->shape = std::move(fused);
    body->elementMap = std::move(fusedMap);
    body->bends.push_back(std::move(bend));
  }
  return true;
}
