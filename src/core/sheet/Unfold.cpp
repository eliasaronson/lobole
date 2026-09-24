#include "core/sheet/Unfold.h"
#include "cad/NonDestructiveBoolean.h"

#include "core/ElementHistory.h"

#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepAlgoAPI_Common.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepAlgoAPI_Section.hxx>
#include <BRepBuilderAPI_GTransform.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepBuilderAPI_MakeSolid.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepBuilderAPI_Sewing.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepClass3d_SolidClassifier.hxx>
#include <BRepFill.hxx>
#include <BRepGProp.hxx>
#include <BRepLib.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <BRepPrimAPI_MakeRevol.hxx>
#include <BRepTools.hxx>
#include <BRepTools_WireExplorer.hxx>
#include <BRep_Tool.hxx>
#include <GCPnts_AbscissaPoint.hxx>
#include <GCPnts_UniformAbscissa.hxx>
#include <GProp_GProps.hxx>
#include <Geom2d_BSplineCurve.hxx>
#include <GeomConvert.hxx>
#include <Geom_BSplineCurve.hxx>
#include <Geom_CylindricalSurface.hxx>
#include <Geom_TrimmedCurve.hxx>
#include <IntCurvesFace_ShapeIntersector.hxx>
#include <ShapeAnalysis_FreeBounds.hxx>
#include <ShapeFix_Solid.hxx>
#include <Standard_Failure.hxx>
#include <TColStd_Array1OfInteger.hxx>
#include <TColStd_Array1OfReal.hxx>
#include <TColgp_Array1OfPnt2d.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_HSequenceOfShape.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shell.hxx>
#include <TopoDS_Wire.hxx>
#include <gp_Ax3.hxx>
#include <gp_Cylinder.hxx>
#include <gp_Lin.hxx>
#include <gp_Pln.hxx>
#include <gp_Trsf.hxx>
#include <limits>

#include <algorithm>
#include <cmath>
#include <deque>
#include <set>

namespace sheet {
namespace {

gp_Pnt P(const glm::dvec3& v) { return gp_Pnt(v.x, v.y, v.z); }
gp_Dir D(const glm::dvec3& v) { return gp_Dir(v.x, v.y, v.z); }
glm::dvec3 G(const gp_Pnt& p) { return {p.X(), p.Y(), p.Z()}; }
glm::dvec3 G(const gp_Dir& d) { return {d.X(), d.Y(), d.Z()}; }

gp_Trsf ToTrsf(const glm::dmat4& m) {
  gp_Trsf t;
  // glm is column-major: m[col][row].
  t.SetValues(m[0][0], m[1][0], m[2][0], m[3][0], m[0][1], m[1][1], m[2][1],
              m[3][1], m[0][2], m[1][2], m[2][2], m[3][2]);
  return t;
}

glm::dmat4 FromTrsf(const gp_Trsf& t) {
  glm::dmat4 m(1.0);
  for (int r = 1; r <= 3; ++r)
    for (int c = 1; c <= 4; ++c)
      m[c - 1][r - 1] = t.Value(r, c);
  return m;
}

double Volume(const CADShape& s) { return s.IsValid() ? s.Volume() : 0.0; }
bool Sound(const CADShape& s);

double Area(const TopoDS_Face& f) {
  GProp_GProps props;
  BRepGProp::SurfaceProperties(f, props);
  return props.Mass();
}

CADShape Transformed(const CADShape& s, const gp_Trsf& t) {
  if (!s.IsValid())
    return {};
  BRepBuilderAPI_Transform tr(s.GetShape(), t, Standard_True);
  return tr.IsDone() ? CADShape(tr.Shape()) : CADShape{};
}

// Plain booleans for the throwaway geometry — ideal shapes and cutters carry
// no names worth keeping. An empty or failed result comes back invalid.
CADShape Cut(const CADShape& a, const CADShape& b) {
  if (!a.IsValid())
    return {};
  if (!b.IsValid())
    return a;
  BRepAlgoAPI_Cut op;
  if (!cad::BuildNonDestructive(op, a.GetShape(), b.GetShape()))
    return {};
  CADShape out(op.Shape());
  return Volume(out) > 1e-12 ? out : CADShape{};
}

CADShape Common(const CADShape& a, const CADShape& b) {
  if (!a.IsValid() || !b.IsValid())
    return {};
  BRepAlgoAPI_Common op;
  if (!cad::BuildNonDestructive(op, a.GetShape(), b.GetShape()))
    return {};
  CADShape out(op.Shape());
  return Volume(out) > 1e-12 ? out : CADShape{};
}

// A closed planar polygon face through `pts`.
TopoDS_Face PolygonFace(const std::vector<gp_Pnt>& pts) {
  BRepBuilderAPI_MakePolygon poly;
  for (const gp_Pnt& p : pts)
    poly.Add(p);
  poly.Close();
  if (!poly.IsDone())
    return {};
  BRepBuilderAPI_MakeFace face(poly.Wire(), Standard_True);
  return face.IsDone() ? face.Face() : TopoDS_Face{};
}

TopoDS_Wire PolygonWire(const std::vector<gp_Pnt>& pts) {
  BRepBuilderAPI_MakePolygon poly;
  for (const gp_Pnt& p : pts)
    poly.Add(p);
  poly.Close();
  return poly.Wire();
}

// A prism of the rectangle spanned from `origin` by `a` and `b`, swept along
// `sweep`. A prism rather than a gp_Ax2 box because the frames here are not
// guaranteed right-handed, and an Ax2's implied third axis would then point
// the wrong way on half the bends.
CADShape Block(const gp_Pnt& origin, const gp_Vec& a, const gp_Vec& b,
               const gp_Vec& sweep) {
  const TopoDS_Face face =
      PolygonFace({origin, origin.Translated(a), origin.Translated(a + b),
                   origin.Translated(b)});
  if (face.IsNull())
    return {};
  return CADShape(BRepPrimAPI_MakePrism(face, sweep).Shape());
}

// Everything on the +`n` side of the plane through `at`, out to `size`.
CADShape HalfSpace(const gp_Pnt& at, const gp_Dir& n, double size) {
  const gp_Dir a = std::abs(n.Z()) < 0.9
                       ? gp_Dir(gp_Vec(n).Crossed(gp_Vec(0, 0, 1)))
                       : gp_Dir(gp_Vec(n).Crossed(gp_Vec(1, 0, 0)));
  const gp_Dir b(gp_Vec(n).Crossed(gp_Vec(a)));
  const gp_Vec va = gp_Vec(a) * size, vb = gp_Vec(b) * size;
  return Block(at.Translated(-va - vb), va * 2.0, vb * 2.0, gp_Vec(n) * size);
}

bool Contains(const TopoDS_Shape& shape, const gp_Pnt& p) {
  BRepClass3d_SolidClassifier classify(shape, p, 1e-6);
  return classify.State() == TopAbs_IN || classify.State() == TopAbs_ON;
}

// The connected solid of `shape` that holds any of `probes`, with the names
// its faces carried on the whole. Several probes, because a hole is allowed
// to sit exactly where the first one looked.
bool ComponentAt(const CADShape& shape, const ElementMap& map,
                 const std::vector<gp_Pnt>& probes, CADShape& outShape,
                 ElementMap& outMap) {
  if (!shape.IsValid())
    return false;
  std::vector<TopoDS_Shape> solids;
  for (TopExp_Explorer ex(shape.GetShape(), TopAbs_SOLID); ex.More(); ex.Next())
    solids.push_back(ex.Current());
  TopTools_IndexedMapOfShape allFaces;
  TopExp::MapShapes(shape.GetShape(), TopAbs_FACE, allFaces);
  for (const gp_Pnt& probe : probes) {
    for (const TopoDS_Shape& solid : solids) {
      if (!Contains(solid, probe))
        continue;
      outShape = CADShape(solid);
      outMap = ElementMap{};
      // The component's faces are the SAME subshapes, so their names come
      // across by looking each one up in the whole's face order.
      int i = 0;
      for (TopExp_Explorer ex(solid, TopAbs_FACE); ex.More(); ex.Next(), ++i) {
        const int whole = allFaces.FindIndex(ex.Current());
        if (whole == 0)
          continue;
        if (const std::string* n = map.NameOf(whole - 1))
          outMap.names[i] = *n;
      }
      return true;
    }
  }
  return false;
}

// Points along a segment of the axis, middle first — where to look for
// material when a cut might be sitting at any one of them.
std::vector<gp_Pnt> Along(const gp_Pnt& base, const gp_Dir& axis,
                          double width) {
  std::vector<gp_Pnt> out;
  for (double f : {0.5, 0.3, 0.7, 0.1, 0.9, 0.02, 0.98})
    out.push_back(base.Translated(gp_Vec(axis) * (f * width)));
  return out;
}

// One bend's geometry, in the coordinates its record is written in.
struct BendFrame {
  gp_Pnt C;         // on the axis, at the start of the span
  gp_Dir pd, up, A; // parent direction, picked-face normal, axis
  gp_Dir n;         // pd x up: a right-handed turn about this FOLDS
  gp_Dir cd;        // the child's direction
  gp_Pnt Tf;        // C - up*R: the tangent point on the picked face
  double R = 0, t = 0, theta = 0, K = 0, BA = 0, width = 0;

  [[nodiscard]] gp_Vec W(double phi) const {
    return gp_Vec(pd) * std::sin(phi) - gp_Vec(up) * std::cos(phi);
  }
  // Unfold: rotate the child back into the parent's plane, slide it out by
  // the allowance. local = slide * rot(-theta).
  [[nodiscard]] gp_Trsf Local() const {
    gp_Trsf rot;
    rot.SetRotation(gp_Ax1(C, n), -theta);
    gp_Trsf slide;
    slide.SetTranslation(gp_Vec(pd) * BA);
    return slide * rot;
  }

  // Flat (in this frame) to bent: the neutral-axis map the allowance itself
  // comes from. Rigid on either side of the band, so the three pieces meet.
  [[nodiscard]] gp_Pnt Wrap(const gp_Pnt& q) const {
    const gp_Vec d(Tf, q);
    const double u = d.Dot(gp_Vec(pd));
    if (u <= 0.0)
      return q;
    if (u >= BA)
      return q.Transformed(Local().Inverted());
    const double v = d.Dot(gp_Vec(up));
    const double w = d.Dot(gp_Vec(A));
    const double phi = u / (R + K * t);
    return C.Translated(W(phi) * (R - v) + gp_Vec(A) * w);
  }
  // Bent to flat: the inverse, for a point on the bend.
  [[nodiscard]] gp_Pnt Unwrap(const gp_Pnt& p) const {
    const gp_Vec d(C, p);
    const double w = d.Dot(gp_Vec(A));
    const gp_Vec rv = d - gp_Vec(A) * w;
    const double phi = std::atan2(rv.Dot(gp_Vec(pd)), -rv.Dot(gp_Vec(up)));
    const double r = rv.Magnitude();
    return Tf.Translated(gp_Vec(pd) * (phi * (R + K * t)) +
                         gp_Vec(up) * (R - r) + gp_Vec(A) * w);
  }

  // The bend as a brake makes it: the full annular sector, width long.
  [[nodiscard]] CADShape Sector() const {
    const TopoDS_Face face =
        PolygonFace({Tf, Tf.Translated(gp_Vec(up) * -t),
                     Tf.Translated(gp_Vec(up) * -t + gp_Vec(A) * width),
                     Tf.Translated(gp_Vec(A) * width)});
    if (face.IsNull())
      return {};
    return CADShape(BRepPrimAPI_MakeRevol(face, gp_Ax1(C, n), theta).Shape());
  }
  // The same bend laid flat: allowance x thickness x width.
  [[nodiscard]] CADShape Strip() const {
    return Block(Tf, gp_Vec(up) * -t, gp_Vec(A) * width, gp_Vec(pd) * BA);
  }
};

BendFrame FrameOf(const BendRegion& b, const SheetMetalRules& rules) {
  BendFrame f;
  f.C = P(b.axisOrigin);
  f.pd = D(b.parentDir);
  f.up = D(b.upDir);
  f.A = D(b.axisDir);
  f.n = gp_Dir(gp_Vec(f.pd).Crossed(gp_Vec(f.up)));
  f.cd = gp_Dir(gp_Vec(f.pd) * std::cos(b.angle) +
                gp_Vec(f.up) * std::sin(b.angle));
  f.R = b.radius;
  f.t = rules.thickness;
  f.theta = b.angle;
  f.K = b.kFactor > 0.0 ? b.kFactor : rules.EffectiveKFactor(b.radius);
  f.BA = BendAllowance(b.angle, b.radius, f.K, f.t);
  f.width = b.width;
  f.Tf = f.C.Translated(gp_Vec(f.up) * -f.R);
  return f;
}

// A bend's record, moved with the piece it rides on.
void Move(BendRegion& b, const gp_Trsf& m) {
  if (b.flat) {
    // The folded record stays in the coordinates it was written in; what
    // moved is where its parent panel sits, and the drawn crease line.
    b.parentPlace = FromTrsf(m * ToTrsf(b.parentPlace));
    b.flatA = G(P(b.flatA).Transformed(m));
    b.flatB = G(P(b.flatB).Transformed(m));
    return;
  }
  b.axisOrigin = G(P(b.axisOrigin).Transformed(m));
  b.axisDir = G(D(b.axisDir).Transformed(m));
  b.parentDir = G(D(b.parentDir).Transformed(m));
  b.upDir = G(D(b.upDir).Transformed(m));
  for (BendRegion::Trim& trim : b.trims) {
    trim.point = G(P(trim.point).Transformed(m));
    trim.normal = G(D(trim.normal).Transformed(m));
  }
}

// Where a bend's material is NOW, for asking which piece carries it: the
// middle of the arc for a folded bend, the middle of the band for a crease.
std::vector<gp_Pnt> ProbesOf(const BendRegion& b,
                             const SheetMetalRules& rules) {
  const BendFrame f = FrameOf(b, rules);
  if (!b.flat)
    return Along(f.C.Translated(f.W(f.theta / 2.0) * (f.R + f.t / 2.0)), f.A,
                 f.width);
  const gp_Trsf place = ToTrsf(b.parentPlace);
  std::vector<gp_Pnt> out;
  for (const gp_Pnt& p : Along(f.Tf.Translated(gp_Vec(f.pd) * (f.BA / 2.0) -
                                               gp_Vec(f.up) * (f.t / 2.0)),
                               f.A, f.width))
    out.push_back(p.Transformed(place));
  return out;
}

// Samples a wire in order, `step` apart, closed (the last point is not the
// first again).
std::vector<gp_Pnt> Sample(const TopoDS_Wire& wire, double step) {
  std::vector<gp_Pnt> pts;
  for (BRepTools_WireExplorer ex(wire); ex.More(); ex.Next()) {
    BRepAdaptor_Curve curve(ex.Current());
    const double len = GCPnts_AbscissaPoint::Length(curve);
    const int n = std::max(2, static_cast<int>(std::ceil(len / step)) + 1);
    GCPnts_UniformAbscissa ua(curve, n);
    std::vector<gp_Pnt> edge;
    if (ua.IsDone()) {
      for (int i = 1; i <= ua.NbPoints(); ++i)
        edge.push_back(curve.Value(ua.Parameter(i)));
    } else {
      edge = {curve.Value(curve.FirstParameter()),
              curve.Value(curve.LastParameter())};
    }
    if (ex.Current().Orientation() == TopAbs_REVERSED)
      std::reverse(edge.begin(), edge.end());
    edge.pop_back(); // the next edge starts here
    pts.insert(pts.end(), edge.begin(), edge.end());
  }
  return pts;
}

// Faces of `shape` lying on the cylinder of radius `r` about the frame axis.
std::vector<TopoDS_Face> OnCylinder(const CADShape& shape, const BendFrame& f,
                                    double r) {
  std::vector<TopoDS_Face> out;
  if (!shape.IsValid())
    return out;
  for (TopExp_Explorer ex(shape.GetShape(), TopAbs_FACE); ex.More();
       ex.Next()) {
    const TopoDS_Face face = TopoDS::Face(ex.Current());
    BRepAdaptor_Surface surf(face);
    if (surf.GetType() != GeomAbs_Cylinder)
      continue;
    const gp_Cylinder cyl = surf.Cylinder();
    if (std::abs(cyl.Radius() - r) > 1e-6)
      continue;
    if (!cyl.Axis().Direction().IsParallel(f.n, 1e-6))
      continue;
    if (gp_Lin(cyl.Axis()).Distance(f.C) > 1e-6)
      continue;
    out.push_back(face);
  }
  return out;
}

// Faces of `shape` lying in the plane through `at` with normal `n`.
std::vector<TopoDS_Face> OnPlane(const CADShape& shape, const gp_Pnt& at,
                                 const gp_Dir& n) {
  std::vector<TopoDS_Face> out;
  if (!shape.IsValid())
    return out;
  for (TopExp_Explorer ex(shape.GetShape(), TopAbs_FACE); ex.More();
       ex.Next()) {
    const TopoDS_Face face = TopoDS::Face(ex.Current());
    BRepAdaptor_Surface surf(face);
    if (surf.GetType() != GeomAbs_Plane)
      continue;
    const gp_Pln pln = surf.Plane();
    if (!pln.Axis().Direction().IsParallel(n, 1e-6) ||
        std::abs(pln.Distance(at)) > 1e-6)
      continue;
    out.push_back(face);
  }
  return out;
}

double SumArea(const std::vector<TopoDS_Face>& faces, double scale) {
  double a = 0.0;
  for (const TopoDS_Face& f : faces)
    a += Area(f) * scale;
  return a;
}

// Carrying a cut across a bend needs the cut to go RIGHT THROUGH the sheet:
// then its outline on the inside face and on the outside face are the same
// shape under the neutral-axis map, and one of them is all it takes. A pocket
// from one side has no such outline, and mapping it anyway would quietly turn
// it into a hole.
bool ThroughCut(double innerFlat, double outerFlat) {
  const double big = std::max(innerFlat, outerFlat);
  return big > 1e-9 && std::abs(innerFlat - outerFlat) <= 0.05 * big + 1e-6;
}

double Step(const BendFrame& f) { return std::clamp(f.t / 4.0, 0.05, 0.25); }

// The flat strip for a bend whose material is `slab` (folded, this frame):
// the ideal strip, less every through-cut the bend carries, laid flat along
// the NEUTRAL surface.
//
// Neutral rather than inside or outside, because a cut that is not square to
// the sheet — the bisector of a mitred corner — crosses the two faces along
// different outlines, and the blank is cut to the one the bend allowance
// itself is measured on. For a cut square to the sheet all three agree.
std::optional<CADShape> StripFor(const BendFrame& f, const CADShape& slab,
                                 int index, std::string& why) {
  const CADShape sector = f.Sector();
  const CADShape strip = f.Strip();
  const double tol = std::max(1e-6, 1e-6 * Volume(sector));
  if (Volume(Cut(slab, sector)) > tol) {
    why = "bend " + std::to_string(index) +
          " has material ADDED to it, which has no flat shape - move the "
          "boss onto a panel";
    return std::nullopt;
  }
  const CADShape removed = Cut(sector, slab);
  if (Volume(removed) <= tol)
    return strip;

  const double neutral = f.R + f.K * f.t;
  const double sA = gp_Vec(f.A).Dot(gp_Vec(f.n)) > 0.0 ? 1.0 : -1.0;
  const double v0 = std::min(0.0, sA * f.width) - 1.0;
  const double v1 = std::max(0.0, sA * f.width) + 1.0;
  const TopoDS_Face neutralFace = BRepBuilderAPI_MakeFace(
      new Geom_CylindricalSurface(gp_Ax3(f.C, f.n, f.up.Reversed()), neutral),
      -0.01, f.theta + 0.01, v0, v1, 1e-7);

  CADShape out = strip;
  const double eps = std::max(1e-3, f.t * 1e-3);
  for (TopExp_Explorer sx(removed.GetShape(), TopAbs_SOLID); sx.More();
       sx.Next()) {
    const CADShape plug(sx.Current());
    if (Volume(plug) <= tol)
      continue;
    // Through the sheet or not at all: a pocket from one side has no outline
    // the blank can be cut to, and laying one down anyway would quietly turn
    // it into a hole.
    if (OnCylinder(plug, f, f.R).empty() ||
        OnCylinder(plug, f, f.R + f.t).empty()) {
      why = "bend " + std::to_string(index) +
            " has a cut that does not go through the sheet; only a "
            "through-cut can be carried across a bend into the blank";
      return std::nullopt;
    }
    BRepAlgoAPI_Section section(plug.GetShape(), neutralFace, Standard_False);
    section.ComputePCurveOn2(Standard_True);
    section.Build();
    if (!section.IsDone()) {
      why = "a cut across bend " + std::to_string(index) +
            " could not be found on the bend's neutral surface";
      return std::nullopt;
    }
    Handle(TopTools_HSequenceOfShape) edges = new TopTools_HSequenceOfShape;
    for (TopExp_Explorer ex(section.Shape(), TopAbs_EDGE); ex.More(); ex.Next())
      edges->Append(ex.Current());
    Handle(TopTools_HSequenceOfShape) wires;
    ShapeAnalysis_FreeBounds::ConnectEdgesToWires(edges, 1e-5, Standard_False,
                                                  wires);
    if (wires.IsNull() || wires->Length() != 1) {
      why = "bend " + std::to_string(index) +
            (wires.IsNull() || wires->Length() == 0
                 ? " has a cut that could not be traced on it"
                 : " has a cut with an island inside it, which cannot be "
                   "carried across the bend yet");
      return std::nullopt;
    }
    // Laid down ON the plane: keep each point's (u, w) and set its height,
    // rather than trusting the third coordinate of the unwrap. The section's
    // points sit on the neutral cylinder only to the section's tolerance, and
    // a polygon a hundredth of a micron out of plane is not a face.
    std::vector<gp_Pnt> loop;
    for (const gp_Pnt& p : Sample(TopoDS::Wire(wires->Value(1)), Step(f))) {
      const gp_Vec d(f.Tf, f.Unwrap(p));
      loop.push_back(f.Tf.Translated(gp_Vec(f.pd) * d.Dot(gp_Vec(f.pd)) +
                                     gp_Vec(f.A) * d.Dot(gp_Vec(f.A)) +
                                     gp_Vec(f.up) * eps));
    }
    const TopoDS_Face flat = PolygonFace(loop);
    if (flat.IsNull()) {
      why = "a cut across bend " + std::to_string(index) +
            " could not be laid flat";
      return std::nullopt;
    }
    out = Cut(out, CADShape(BRepPrimAPI_MakePrism(flat, gp_Vec(f.up) *
                                                            -(f.t + 2.0 * eps))
                                .Shape()));
    if (!out.IsValid()) {
      why = "a cut across bend " + std::to_string(index) +
            " removed the whole strip";
      return std::nullopt;
    }
  }
  return out;
}

bool Sound(const CADShape& s) {
  return s.IsValid() && BRepCheck_Analyzer(s.GetShape()).IsValid();
}

// A through-cut drawn flat, wrapped onto the bend EXACTLY.
//
// In a cylinder's own parameters (angle, height) the neutral-axis map is
// affine: (u, w) -> (u / (R + K*t), w). An affine map of a B-spline is the
// same B-spline with its poles mapped, so every edge of the flat outline
// becomes an exact curve on the inside and outside cylinders, and the wall
// between them is a ruled face that is exactly radial. A loft between two
// sampled polygons approximates the same thing and is NOT a solid: polygons
// lying on a cylinder have no planar caps, and the ones a loft invents make
// the cut bend invalid while every volume still reads right.
CADShape WrappedCutter(const BendFrame& f, const TopoDS_Face& flatFace) {
  const double eps = std::max(1e-3, f.t * 1e-3);
  const double neutral = f.R + f.K * f.t;
  // U = the bend angle from the parent's tangent line, V = height along the
  // fold axis: X = -up, Y = pd, Z = pd x up.
  const gp_Ax3 frame(f.C, f.n, f.up.Reversed());
  const double sA = gp_Vec(f.A).Dot(gp_Vec(f.n)) > 0.0 ? 1.0 : -1.0;
  Handle(Geom_CylindricalSurface) inner =
      new Geom_CylindricalSurface(frame, f.R - eps);
  Handle(Geom_CylindricalSurface) outer =
      new Geom_CylindricalSurface(frame, f.R + f.t + eps);

  BRepBuilderAPI_MakeWire innerWire, outerWire;
  std::vector<TopoDS_Face> walls;
  for (BRepTools_WireExplorer ex(BRepTools::OuterWire(flatFace)); ex.More();
       ex.Next()) {
    const TopoDS_Edge& edge = ex.Current();
    double first = 0.0, last = 0.0;
    const Handle(Geom_Curve) c3d = BRep_Tool::Curve(edge, first, last);
    if (c3d.IsNull())
      return {};
    Handle(Geom_BSplineCurve) bs = GeomConvert::CurveToBSplineCurve(
        new Geom_TrimmedCurve(c3d, first, last));
    if (edge.Orientation() == TopAbs_REVERSED)
      bs->Reverse();
    TColgp_Array1OfPnt2d poles(1, bs->NbPoles());
    for (int i = 1; i <= bs->NbPoles(); ++i) {
      const gp_Vec d(f.Tf, bs->Pole(i));
      poles(i) = gp_Pnt2d(d.Dot(gp_Vec(f.pd)) / neutral,
                          sA * d.Dot(gp_Vec(f.A)) +
                              gp_Vec(f.C, f.Tf).Dot(gp_Vec(f.n)));
    }
    TColStd_Array1OfReal knots(1, bs->NbKnots());
    TColStd_Array1OfInteger mults(1, bs->NbKnots());
    bs->Knots(knots);
    bs->Multiplicities(mults);
    Handle(Geom2d_BSplineCurve) c2d;
    if (bs->IsRational()) {
      TColStd_Array1OfReal weights(1, bs->NbPoles());
      bs->Weights(weights);
      c2d = new Geom2d_BSplineCurve(poles, weights, knots, mults, bs->Degree());
    } else {
      c2d = new Geom2d_BSplineCurve(poles, knots, mults, bs->Degree());
    }
    TopoDS_Edge ein = BRepBuilderAPI_MakeEdge(c2d, inner);
    TopoDS_Edge eout = BRepBuilderAPI_MakeEdge(c2d, outer);
    BRepLib::BuildCurves3d(ein);
    BRepLib::BuildCurves3d(eout);
    innerWire.Add(ein);
    outerWire.Add(eout);
    walls.push_back(BRepFill::Face(ein, eout));
  }
  if (!innerWire.IsDone() || !outerWire.IsDone())
    return {};
  BRepBuilderAPI_Sewing sew(1e-5);
  sew.Add(BRepBuilderAPI_MakeFace(inner, innerWire.Wire(), Standard_True));
  sew.Add(BRepBuilderAPI_MakeFace(outer, outerWire.Wire(), Standard_True));
  for (const TopoDS_Face& w : walls)
    sew.Add(w);
  sew.Perform();
  TopoDS_Shell shell;
  for (TopExp_Explorer ex(sew.SewedShape(), TopAbs_SHELL); ex.More();
       ex.Next()) {
    shell = TopoDS::Shell(ex.Current());
    break;
  }
  if (shell.IsNull())
    return {};
  BRepBuilderAPI_MakeSolid solid(shell);
  if (!solid.IsDone())
    return {};
  ShapeFix_Solid fix(solid.Solid());
  fix.Perform();
  CADShape out(fix.Solid());
  // An inside-out solid measures negative; turning it round is the whole fix.
  if (out.IsValid() && out.Volume() < 0.0)
    out = CADShape(fix.Solid().Reversed());
  return Sound(out) && out.Volume() > 1e-12 ? out : CADShape{};
}

// The bend for a band whose material is `band` (flat, this frame): the ideal
// sector, less every through-cut the band carries, wrapped.
std::optional<CADShape> SectorFor(const BendFrame& f, const CADShape& band,
                                  const CADShape& trimmedSector,
                                  const CADShape& expectedStrip, int index,
                                  std::string& why) {
  // Where a mitre cut the bend, the trim is a PLANE through it, and no radial
  // wrap of a flat outline reproduces a plane. So the bend starts as the
  // ideal sector cut by its trims, exactly; the band is compared with what
  // the unfolder makes of THAT; and only what was cut beyond it — while the
  // bend lay flat — is wrapped.
  CADShape sector = trimmedSector;
  const CADShape& strip = expectedStrip;
  const double tol = std::max(1e-6, 1e-6 * Volume(f.Strip()));
  if (Volume(Cut(band, strip)) > tol) {
    why = "crease " + std::to_string(index) +
          " has material ADDED across it, which cannot be bent - move the "
          "boss onto a panel";
    return std::nullopt;
  }
  const CADShape removed = Cut(strip, band);
  if (Volume(removed) <= tol)
    return sector;
  const std::vector<TopoDS_Face> top = OnPlane(removed, f.Tf, f.up);
  const std::vector<TopoDS_Face> bottom =
      OnPlane(removed, f.Tf.Translated(gp_Vec(f.up) * -f.t), f.up);
  if (!ThroughCut(SumArea(top, 1.0), SumArea(bottom, 1.0))) {
    why = "crease " + std::to_string(index) +
          " has a cut that does not go through the sheet; only a "
          "through-cut can be carried across a bend";
    return std::nullopt;
  }
  CADShape out = sector;
  for (const TopoDS_Face& face : top) {
    const CADShape cutter = WrappedCutter(f, face);
    if (!cutter.IsValid()) {
      why = "a cut across crease " + std::to_string(index) +
            " could not be wrapped onto the bend";
      return std::nullopt;
    }
    out = Cut(out, cutter);
    if (!out.IsValid() || !Sound(out)) {
      why = "a cut across crease " + std::to_string(index) +
            " did not leave a sound bend";
      return std::nullopt;
    }
  }
  return out;
}

struct Named {
  CADShape shape;
  ElementMap map;
};

bool Fuse(Named& into, const CADShape& shape, const ElementMap& map,
          uint64_t tag) {
  if (!shape.IsValid() || shape.Volume() <= 1e-12)
    return true;
  if (!into.shape.IsValid()) {
    into.shape = shape;
    into.map = map;
    return true;
  }
  ElementMap out;
  std::string why;
  CADShape fused = topo::BooleanNamed(BooleanOp::Union, into.shape, into.map,
                                      shape, map, tag, out, &why);
  // A union can hand back an EMPTY shape without failing. The two inputs
  // together set a floor on what came out, so anything under it is a
  // boolean that did not happen, reported as one.
  const double floor = std::max(Volume(into.shape), Volume(shape));
  if (!fused.IsValid() || Volume(fused) < floor * (1.0 - 1e-6))
    return false;
  into.shape = std::move(fused);
  into.map = std::move(out);
  return true;
}

Named Subtract(const Named& a, const CADShape& b, uint64_t tag) {
  if (!b.IsValid() || b.Volume() <= 1e-12)
    return a;
  Named out;
  ElementMap none;
  std::string why;
  out.shape = topo::BooleanNamed(BooleanOp::Subtract, a.shape, a.map, b, none,
                                 tag, out.map, &why);
  return out;
}

Named Intersect(const Named& a, const CADShape& b, uint64_t tag) {
  Named out;
  ElementMap none;
  std::string why;
  out.shape = topo::BooleanNamed(BooleanOp::Intersect, a.shape, a.map, b, none,
                                 tag, out.map, &why);
  return out;
}

// Every name once. Splitting a body with one tag and fusing it back names
// both halves of a cut face alike, and a name that resolves to two elements
// resolves to whichever comes first — a pick lands on the wrong one. The
// second and later get "#2", "#3" in index order, which is stable across
// regenerations because the index order is.
void Uniquify(ElementMap& map) {
  const auto fix = [](std::map<int, std::string>& names) {
    std::map<std::string, int> seen;
    for (auto& [idx, name] : names) {
      const int n = ++seen[name];
      if (n > 1)
        name += "#" + std::to_string(n);
    }
  };
  fix(map.names);
  fix(map.edgeNames);
}

double SizeOf(const CADShape& s) {
  glm::vec3 lo, hi;
  s.GetBoundingBox(lo, hi);
  return glm::length(hi - lo) * 4.0 + 100.0;
}

} // namespace

namespace {

// Where everything went, for a caller that needs to map a point of the folded
// body onto the blank: each piece and its placement, and each bend's frame
// with its parent's placement.
struct Placement {
  std::vector<std::pair<CADShape, gp_Trsf>> pieces;
  struct Bent {
    BendFrame f;
    gp_Trsf parentPlace;
    std::vector<BendRegion::Trim> trims;
    uint64_t featureId = 0;
  };
  std::vector<Bent> bends;
};

std::optional<FlatResult> UnfoldImpl(const Body& body,
                                     const SheetMetalRules& rules,
                                     const ElementRef& fixedFace, uint64_t tag,
                                     const std::vector<uint64_t>& onlyFrom,
                                     Placement* placement, std::string* error);

} // namespace

std::optional<FlatResult> Unfold(const Body& body, const SheetMetalRules& rules,
                                 const ElementRef& fixedFace, uint64_t tag,
                                 const std::vector<uint64_t>& onlyFrom,
                                 std::string* error) {
  return UnfoldImpl(body, rules, fixedFace, tag, onlyFrom, nullptr, error);
}

namespace {

std::optional<FlatResult> UnfoldImpl(const Body& body,
                                     const SheetMetalRules& rules,
                                     const ElementRef& fixedFace, uint64_t tag,
                                     const std::vector<uint64_t>& onlyFrom,
                                     Placement* placement, std::string* error) {
  const auto fail = [&](const std::string& why) -> std::optional<FlatResult> {
    if (error)
      *error = why;
    return std::nullopt;
  };
  if (!body.shape.IsValid())
    return fail("the body has no geometry to flatten");

  // A blank is already flat, and saying so beats refusing.
  if (body.bends.empty()) {
    FlatResult flat;
    flat.shape = body.shape;
    flat.map = body.elementMap;
    if (placement != nullptr)
      placement->pieces.push_back({body.shape, gp_Trsf()});
    return flat;
  }

  const auto selected = [&](const BendRegion& b) {
    return !b.flat &&
           (onlyFrom.empty() || std::find(onlyFrom.begin(), onlyFrom.end(),
                                          b.featureId) != onlyFrom.end());
  };
  if (std::none_of(body.bends.begin(), body.bends.end(), selected)) {
    // Flattening a blank twice would cheerfully hand back a longer blank.
    return fail(
        std::all_of(body.bends.begin(), body.bends.end(),
                    [](const BendRegion& b) { return b.flat; })
            ? "this body is already flat (a flat pattern, or an Unfold with "
              "no Refold after it) - there is nothing left to unfold"
            : "none of the picked flanges made a bend that is still folded");
  }

  ArmOccSignalHandlers();
  const double size = SizeOf(body.shape);

  try {
    // ---- 1. Split, entirely in the body's current coordinates ------------
    // Every recorded plane is valid here and nowhere else, so all the cutting
    // happens before anything moves.
    struct Piece {
      Named part;
      gp_Trsf place;
      bool placed = false;
    };
    std::vector<Piece> pieces{
        {{body.shape, body.elementMap}, gp_Trsf(), false}};
    struct Link {
      size_t bend = 0;
      int parent = -1, child = -1;
      CADShape strip; // in the parent's frame; placed with the parent
      // Where each side's material is. Which PIECE holds it is settled only
      // once every cut is made: a jog puts two new bends between the root
      // and a flange that is older than both, so the piece an old bend was
      // cut from is not the piece its panel ends up in.
      std::vector<gp_Pnt> parentAt, childAt;
    };
    std::vector<Link> links;

    for (size_t bi = 0; bi < body.bends.size(); ++bi) {
      const BendRegion& b = body.bends[bi];
      if (!selected(b))
        continue;
      const BendFrame f = FrameOf(b, rules);
      const std::vector<gp_Pnt> mid = ProbesOf(b, rules);
      int pi = -1;
      for (size_t i = 0; i < pieces.size() && pi < 0; ++i)
        for (const gp_Pnt& p : mid)
          if (Contains(pieces[i].part.shape.GetShape(), p)) {
            pi = static_cast<int>(i);
            break;
          }
      if (pi < 0)
        return fail("bend " + std::to_string(bi + 1) +
                    " is not on this body any more - the flat pattern must "
                    "sit after every feature that reshapes the sheet");

      const Named whole = pieces[pi].part;
      // The child: whatever of this piece lies past the far tangent plane
      // AND is joined to the wall, one thickness out along it.
      Named child;
      {
        const Named side = Intersect(whole, HalfSpace(f.C, f.cd, size), tag);
        const std::vector<gp_Pnt> probes =
            Along(f.C.Translated(f.W(f.theta) * (f.R + f.t / 2.0) +
                                 gp_Vec(f.cd) * f.t),
                  f.A, f.width);
        if (!ComponentAt(side.shape, side.map, probes, child.shape,
                         child.map)) {
          // The wall is THERE — it just is not in this piece any more,
          // because an earlier bend's child took it: two flanges that touch
          // are one solid, so the corner is continuous metal. No blank folds
          // into that, and saying "no wall past it" sends the reader to look
          // for a missing wall they can see is present.
          const bool inBody =
              std::any_of(probes.begin(), probes.end(), [&](const gp_Pnt& p) {
                return Contains(body.shape.GetShape(), p);
              });
          if (inBody)
            return fail(
                "the wall past bend " + std::to_string(bi + 1) +
                " is joined to another panel, so the two cannot be folded "
                "apart - a flange running the full length of a wall meets "
                "the wall beside it at the corner and fuses to it. Inset it "
                "(offset_start / offset_end) by a bend radius and a "
                "thickness, or mitre the corner (corners: closed)");
          return fail("bend " + std::to_string(bi + 1) +
                      " has no wall past it to lay flat");
        }
      }
      // The bend's own material: between the two tangent planes and within
      // the span along the axis — bounded so that panel beside an inset
      // flange stays panel.
      CADShape slab = Common(whole.shape, HalfSpace(f.C, f.pd, size));
      slab = Common(slab, HalfSpace(f.C, f.cd.Reversed(), size));
      slab = Common(slab, HalfSpace(f.C, f.A, size));
      slab = Common(slab, HalfSpace(f.C.Translated(gp_Vec(f.A) * f.width),
                                    f.A.Reversed(), size));
      // A mitred corner: the neighbour's bend sits in this one's region, and
      // the trims are where the two were cut apart.
      for (const BendRegion::Trim& trim : b.trims)
        slab = Common(slab, HalfSpace(P(trim.point), D(trim.normal), size));
      if (!slab.IsValid())
        return fail("bend " + std::to_string(bi + 1) +
                    " has no material left in it to unfold");

      std::string why;
      const auto strip = StripFor(f, slab, static_cast<int>(bi + 1), why);
      if (!strip)
        return fail(why);

      // Everything that is neither the wall nor the bend stays with the
      // parent — never just the parent's connected piece, which would drop
      // a panel that happens to sit past the tangent plane.
      Named parent = Subtract(Subtract(whole, child.shape, tag), slab, tag);
      if (!parent.shape.IsValid())
        return fail("bend " + std::to_string(bi + 1) +
                    " could not be separated from its panel");

      pieces[pi].part = std::move(parent);
      pieces.push_back({std::move(child), gp_Trsf(), false});
      links.push_back({bi, pi, static_cast<int>(pieces.size() - 1), *strip,
                       Along(f.C.Translated(-gp_Vec(f.up) * (f.R + f.t / 2.0) -
                                            gp_Vec(f.pd) * f.t),
                             f.A, f.width),
                       Along(f.C.Translated(f.W(f.theta) * (f.R + f.t / 2.0) +
                                            gp_Vec(f.cd) * f.t),
                             f.A, f.width)});
    }

    for (Link& link : links) {
      const auto holder = [&](const std::vector<gp_Pnt>& at, int fallback) {
        for (const gp_Pnt& p : at)
          for (size_t i = 0; i < pieces.size(); ++i)
            if (Contains(pieces[i].part.shape.GetShape(), p))
              return static_cast<int>(i);
        return fallback;
      };
      link.parent = holder(link.parentAt, link.parent);
      link.child = holder(link.childAt, link.child);
    }

    // ---- 2. Walk the tree from the fixed panel, placing each piece -------
    int root = 0;
    if (fixedFace.IsSet()) {
      for (size_t i = 0; i < pieces.size(); ++i)
        if (pieces[i].part.map.Resolve(fixedFace.name)) {
          root = static_cast<int>(i);
          break;
        }
    }
    pieces[root].placed = true;

    FlatResult result;
    result.bends = body.bends;
    std::deque<int> queue{root};
    std::vector<bool> done(links.size(), false);
    while (!queue.empty()) {
      const int at = queue.front();
      queue.pop_front();
      for (size_t li = 0; li < links.size(); ++li) {
        Link& link = links[li];
        if (done[li])
          continue;
        // A link is walked from whichever end is already placed. Normally
        // that is the parent; when the fixed face is on a wall, it is the
        // child, and the parent swings the other way.
        const bool fromParent = link.parent == at;
        const bool fromChild = link.child == at && !pieces[link.parent].placed;
        if (!fromParent && !fromChild)
          continue;
        const BendFrame f = FrameOf(body.bends[link.bend], rules);
        if (fromParent) {
          pieces[link.child].place = pieces[at].place * f.Local();
          pieces[link.child].placed = true;
          queue.push_back(link.child);
        } else {
          pieces[link.parent].place = pieces[at].place * f.Local().Inverted();
          pieces[link.parent].placed = true;
          queue.push_back(link.parent);
        }
        done[li] = true;
      }
    }

    // Each crease: its allowance, where its parent went, and the line a
    // bend is drawn along — the middle of the band, the bend's own width.
    for (const Link& link : links) {
      const gp_Trsf& place = pieces[link.parent].place;
      const BendFrame f = FrameOf(body.bends[link.bend], rules);
      BendRegion& crease = result.bends[link.bend];
      crease.flat = true;
      crease.allowance = f.BA;
      crease.unfoldedBy = tag;
      crease.parentPlace = FromTrsf(place);
      const gp_Pnt a =
          f.Tf.Translated(gp_Vec(f.pd) * (f.BA / 2.0)).Transformed(place);
      crease.flatA = G(a);
      crease.flatB = G(a.Translated(gp_Vec(f.A).Transformed(place) * f.width));
    }
    // Everything this call did not flatten rides with its piece.
    for (size_t bi = 0; bi < body.bends.size(); ++bi) {
      if (selected(body.bends[bi]))
        continue;
      for (const Piece& piece : pieces) {
        bool here = false;
        for (const gp_Pnt& p : ProbesOf(body.bends[bi], rules))
          if (Contains(piece.part.shape.GetShape(), p)) {
            here = true;
            break;
          }
        if (here) {
          Move(result.bends[bi], piece.place);
          break;
        }
      }
    }

    // ---- 3. Place, and fuse ---------------------------------------------
    Named flat;
    for (const Piece& piece : pieces) {
      if (!piece.placed) {
        // A panel the walk never reached would vanish from the blank —
        // silently, and the blank would still look like a part.
        return fail("part of this body is not connected to the fixed panel "
                    "by any recorded bend, so it has no place in the blank");
      }
      if (!Fuse(flat, Transformed(piece.part.shape, piece.place),
                piece.part.map, tag))
        return fail("the flat pattern's panels would not join up");
    }
    for (size_t li = 0; li < links.size(); ++li) {
      ElementMap stripMap;
      topo::NamePrimitiveElements(
          links[li].strip, std::to_string(tag) + "/bend" + std::to_string(li),
          stripMap);
      if (!Fuse(flat,
                Transformed(links[li].strip, pieces[links[li].parent].place),
                stripMap, tag))
        return fail("a bend's flat strip would not join its panels");
    }
    if (!flat.shape.IsValid())
      return fail("the flat pattern came out empty");
    if (placement != nullptr) {
      for (const Piece& piece : pieces)
        placement->pieces.push_back({piece.part.shape, piece.place});
      for (const Link& link : links)
        placement->bends.push_back(
            {FrameOf(body.bends[link.bend], rules), pieces[link.parent].place,
             body.bends[link.bend].trims, body.bends[link.bend].featureId});
    }
    result.shape = std::move(flat.shape);
    result.map = std::move(flat.map);
    Uniquify(result.map);
    return result;
  } catch (const Standard_Failure& e) {
    return fail(std::string("flattening failed: ") + e.GetMessageString());
  }
}

} // namespace

std::optional<FlatResult> Refold(const Body& body, const SheetMetalRules& rules,
                                 uint64_t unfoldTag, uint64_t tag,
                                 std::string* error) {
  const auto fail = [&](const std::string& why) -> std::optional<FlatResult> {
    if (error)
      *error = why;
    return std::nullopt;
  };
  std::vector<size_t> todo;
  for (size_t i = 0; i < body.bends.size(); ++i)
    if (body.bends[i].flat && body.bends[i].unfoldedBy == unfoldTag)
      todo.push_back(i);
  if (todo.empty())
    return fail("nothing on this body was flattened by that Unfold - it is "
                "missing, suppressed, or already refolded");

  ArmOccSignalHandlers();
  try {
    FlatResult result;
    Named work{body.shape, body.elementMap};
    result.bends = body.bends;
    const double size = SizeOf(body.shape);

    // Leaves first: the creases were recorded parents-before-children, so
    // walking them backwards means a crease's child side holds only bends
    // that are already folded again.
    for (auto it = todo.rbegin(); it != todo.rend(); ++it) {
      const size_t ci = *it;
      const BendRegion crease = result.bends[ci];
      BendFrame f = FrameOf(crease, rules);
      const gp_Trsf F = ToTrsf(crease.parentPlace);
      const gp_Trsf Finv = F.Inverted();

      // The band and the child, where they are now.
      const gp_Pnt T0 = f.Tf.Transformed(F);
      const gp_Dir Dn = f.pd.Transformed(F);
      const gp_Dir Un = f.up.Transformed(F);
      const gp_Dir An = f.A.Transformed(F);
      const gp_Pnt T1 = T0.Translated(gp_Vec(Dn) * f.BA);

      // The band is exactly what the unfolder laid down for this bend: the
      // ideal strip, or — where a mitre trimmed the bend — the strip it made
      // of the trimmed sector. Any plane standing in for that outline is
      // exact only where it touches it, and whatever falls between is left
      // behind flat.
      CADShape sector0 = f.Sector();
      const double reach = (f.width + f.R + f.t + f.BA) * 4.0 + 100.0;
      for (const BendRegion::Trim& trim : crease.trims)
        sector0 =
            Common(sector0, HalfSpace(P(trim.point), D(trim.normal), reach));
      CADShape expected = f.Strip();
      if (!crease.trims.empty()) {
        std::string why;
        const auto trimmed =
            StripFor(f, sector0, static_cast<int>(ci + 1), why);
        if (!trimmed)
          return fail(why);
        expected = *trimmed;
      }
      const CADShape band = Common(work.shape, Transformed(expected, F));

      Named child;
      {
        const Named side = Intersect(work, HalfSpace(T1, Dn, size), tag);
        const std::vector<gp_Pnt> probes =
            Along(T1.Translated(gp_Vec(Dn) * f.t - gp_Vec(Un) * (f.t / 2.0)),
                  An, f.width);
        if (!ComponentAt(side.shape, side.map, probes, child.shape, child.map))
          return fail("crease " + std::to_string(ci + 1) +
                      " has no wall past it to fold up");
      }

      std::string why;
      const auto sector = SectorFor(f, Transformed(band, Finv), sector0,
                                    expected, static_cast<int>(ci + 1), why);
      if (!sector)
        return fail(why);

      // The child swings back about the bend, which stays where the parent
      // put it: M = F * local^-1 * F^-1.
      const gp_Trsf M = F * f.Local().Inverted() * Finv;

      // Every other bend the child carries swings with it.
      for (size_t bi = 0; bi < result.bends.size(); ++bi) {
        if (bi == ci)
          continue;
        for (const gp_Pnt& p : ProbesOf(result.bends[bi], rules))
          if (Contains(child.shape.GetShape(), p)) {
            Move(result.bends[bi], M);
            break;
          }
      }

      Named rest = Subtract(Subtract(work, child.shape, tag), band, tag);

      if (!rest.shape.IsValid())
        return fail("crease " + std::to_string(ci + 1) +
                    " could not be separated from its panel");
      ElementMap sectorMap;
      topo::NamePrimitiveElements(
          *sector, std::to_string(tag) + "/bend" + std::to_string(ci),
          sectorMap);
      if (!Fuse(rest, Transformed(child.shape, M), child.map, tag) ||
          !Fuse(rest, Transformed(*sector, F), sectorMap, tag))
        return fail("crease " + std::to_string(ci + 1) +
                    " would not fold back into one piece");
      work = std::move(rest);

      // The crease is a bend again, written where its parent now sits.
      BendRegion bend = crease;
      bend.flat = false;
      bend.allowance = 0.0;
      bend.unfoldedBy = 0;
      bend.flatA = bend.flatB = glm::dvec3(0.0);
      bend.parentPlace = glm::dmat4(1.0);
      Move(bend, F);
      result.bends[ci] = std::move(bend);
    }

    result.shape = std::move(work.shape);
    result.map = std::move(work.map);
    Uniquify(result.map);
    return result;
  } catch (const Standard_Failure& e) {
    return fail(std::string("refolding failed: ") + e.GetMessageString());
  }
}

namespace {

// Which region of the folded body a point belongs to, and where it lands on
// the blank: in a bend's wedge it is unwrapped by the neutral map, otherwise it
// rides rigidly with the piece that holds it. `region` remembers the last
// answer, so a point just off the sheet — a notch running out past an edge —
// is carried the same way as its neighbour.
std::optional<gp_Pnt> ToFlat(const Placement& pl, const gp_Pnt& p,
                             int& region) {
  const auto inBend = [&](const Placement::Bent& b) {
    const BendFrame& f = b.f;
    const gp_Vec d(f.C, p);
    const double w = d.Dot(gp_Vec(f.A));
    const gp_Vec rv = d - gp_Vec(f.A) * w;
    const double r = rv.Magnitude();
    const double phi = std::atan2(rv.Dot(gp_Vec(f.pd)), -rv.Dot(gp_Vec(f.up)));
    const double tol = 1e-6;
    if (w < -tol || w > f.width + tol || r < f.R - tol || r > f.R + f.t + tol ||
        phi < -tol || phi > f.theta + tol)
      return false;
    for (const BendRegion::Trim& trim : b.trims)
      if (gp_Vec(P(trim.point), p).Dot(gp_Vec(D(trim.normal))) < -tol)
        return false;
    return true;
  };
  const auto viaBend = [&](size_t i) {
    return pl.bends[i].f.Unwrap(p).Transformed(pl.bends[i].parentPlace);
  };
  for (size_t i = 0; i < pl.bends.size(); ++i)
    if (inBend(pl.bends[i])) {
      region = -static_cast<int>(i) - 1;
      return viaBend(i);
    }
  for (size_t i = 0; i < pl.pieces.size(); ++i)
    if (Contains(pl.pieces[i].first.GetShape(), p)) {
      region = static_cast<int>(i);
      return p.Transformed(pl.pieces[i].second);
    }
  if (region >= 0)
    return p.Transformed(pl.pieces[static_cast<size_t>(region)].second);
  if (region < 0 && region != std::numeric_limits<int>::min())
    return viaBend(static_cast<size_t>(-region - 1));
  return std::nullopt;
}

// Where the line through `p` along `dir` meets the body, and on which face:
// the sheet the sketch LOOKS AT when it looks at one, else the nearest.
//
// A normal cut carries each point of its outline along the sketch's normal
// (docs/sheetmetal/README.md#features), so a panel IN FRONT of the plane wins
// over a nearer one behind it. Taking the nearest either way put the firebox's
// burner ports — sketched inside the box, facing its front wall — through
// the BACK wall, which happened to be a millimetre and a half nearer the
// plane, and nothing anywhere said so. A plane with sheet on one side only
// still cuts it, which is the common case and what the pick means there.
std::optional<std::pair<gp_Pnt, TopoDS_Face>>
Hit(IntCurvesFace_ShapeIntersector& inter, const gp_Pnt& p, const gp_Dir& dir) {
  inter.Perform(gp_Lin(p, dir), -1e9, 1e9);
  if (!inter.IsDone() || inter.NbPnt() == 0)
    return std::nullopt;
  int best = 1;
  for (int i = 2; i <= inter.NbPnt(); ++i) {
    const bool aheadI = inter.WParameter(i) > 0;
    const bool aheadBest = inter.WParameter(best) > 0;
    if (aheadI != aheadBest) {
      if (aheadI)
        best = i;
      continue;
    }
    if (std::abs(inter.WParameter(i)) < std::abs(inter.WParameter(best)))
      best = i;
  }
  return std::make_pair(inter.Pnt(best), inter.Face(best));
}

// What every profile in ONE sketch shares: the ray tree its outlines are
// traced against, and the part laid flat.
//
// Both are per BODY and both are expensive — the intersector builds a tree
// over every face, and the unfold is a boolean per bend — and a sketch used
// to pay for one of each PER HOLE, times one ray cast per 0.25 mm of every
// outline. The firebox's thirteen holes unfolded the firebox thirteen times
// and loaded ~2,300 face trees to do it, which was 73% of the whole grill's
// build
// (docs/sheetmetal/README.md#normalcut-shares-its-ray-tree-and-unfolds-only-when-it-must).
//
// The unfold is built on demand and only if some outline needs it: it
// answers "which panel, or which bend, does this land on", and a hole square
// onto a flat panel — most holes — answers that from the face its samples
// hit. The tree is loaded on the body as it was BEFORE the first cut, which
// is where every outline of the sketch was traced from anyway.
struct CutShared {
  const Body& body;
  const SheetMetalRules& rules;
  uint64_t tag;
  IntCurvesFace_ShapeIntersector rays;
  std::optional<Placement> laid;
  std::string why;

  CutShared(const Body& b, const SheetMetalRules& r, uint64_t t)
      : body(b), rules(r), tag(t) {
    rays.Load(b.shape.GetShape(), 1e-7);
  }

  const Placement* Flat() {
    if (!laid) {
      Placement all;
      if (!UnfoldImpl(body, rules, {}, tag, {}, &all, &why))
        return nullptr;
      laid = std::move(all);
    }
    return &*laid;
  }
};

std::optional<FlatResult> NormalCutOne(const Body& body, CutShared& shared,
                                       const TopoDS_Face& profile,
                                       const gp_Dir& along, uint64_t tag,
                                       std::string* error) {
  const SheetMetalRules& rules = shared.rules;
  const auto fail = [&](const std::string& why) -> std::optional<FlatResult> {
    if (error)
      *error = why;
    return std::nullopt;
  };
  for (const BendRegion& b : body.bends)
    if (b.flat)
      return fail("this body has bends laid flat - a normal cut is made on "
                  "the folded part, so refold them first");
  ArmOccSignalHandlers();
  try {
    const double t = rules.thickness;
    const double eps = std::max(1e-3, t * 1e-3);
    const TopoDS_Wire outerWire = BRepTools::OuterWire(profile);
    std::vector<TopoDS_Wire> wires{outerWire};
    for (TopExp_Explorer wx(profile, TopAbs_WIRE); wx.More(); wx.Next())
      if (!wx.Current().IsSame(outerWire))
        wires.push_back(TopoDS::Wire(wx.Current()));

    // Each wire, sampled, and where each sample meets the sheet.
    struct Sample_ {
      std::optional<gp_Pnt> hit;
      TopoDS_Face face;
      gp_Pnt at;
    };
    std::vector<std::vector<Sample_>> samples;
    for (const TopoDS_Wire& wire : wires) {
      std::vector<Sample_> row;
      for (const gp_Pnt& p : Sample(wire, 0.25)) {
        Sample_ smp;
        smp.at = p;
        if (const auto h = Hit(shared.rays, p, along)) {
          smp.hit = h->first;
          smp.face = h->second;
        }
        row.push_back(smp);
      }
      samples.push_back(std::move(row));
    }

    // Lays each wire on the blank of `pl`, in the plane through `at` facing
    // `out`, and returns the cutter there: the outline's prism through the
    // sheet, less its islands. `split` reports an outline that landed on
    // two separate parts of the sheet.
    bool split = false;
    const auto cutterIn = [&](const Placement& pl, const gp_Pnt& at,
                              const gp_Dir& out) -> CADShape {
      const auto layDown = [&](const std::vector<Sample_>& row) {
        std::vector<gp_Pnt> loop;
        int region = std::numeric_limits<int>::min();
        for (const Sample_& smp : row) {
          if (!smp.hit && region == std::numeric_limits<int>::min())
            continue; // nothing known yet to carry an off-sheet point with
          const auto q = ToFlat(pl, smp.hit ? *smp.hit : smp.at, region);
          if (!q)
            continue;
          const double off = gp_Vec(at, *q).Dot(gp_Vec(out));
          loop.push_back(q->Translated(gp_Vec(out) * (eps - off)));
        }
        // Neighbouring samples are a quarter of a millimetre apart; laid on
        // the blank they stay close unless the outline landed on two
        // different parts of the sheet — the tip of a wall standing in the
        // way, say — and a polygon strung between them would cut a slot
        // across the part.
        const double jump = std::max(5.0, 10.0 * t);
        for (size_t k = 0; k < loop.size(); ++k)
          if (loop[k].Distance(loop[(k + 1) % loop.size()]) > jump) {
            split = true;
            return TopoDS_Face{};
          }
        return loop.size() >= 3 ? PolygonFace(loop) : TopoDS_Face{};
      };
      const gp_Vec through = gp_Vec(out) * -(t + 2.0 * eps);
      const TopoDS_Face outer = layDown(samples[0]);
      if (outer.IsNull())
        return {};
      CADShape cutter(BRepPrimAPI_MakePrism(outer, through).Shape());
      for (size_t w = 1; w < samples.size(); ++w) {
        // An island inside the outline stays: it comes out of the cutter.
        const TopoDS_Face island = layDown(samples[w]);
        if (!island.IsNull())
          cutter = Cut(
              cutter, CADShape(BRepPrimAPI_MakePrism(island, through).Shape()));
      }
      return cutter;
    };

    // Pass one: where does the outline land? The samples already name the
    // FACE each one met, and that answers it outright for the common case —
    // every sample on ONE planar face is one panel and no bend, which is
    // what a hole square onto a sheet is. The unfold below says the same
    // thing for the rest, and costs a boolean per bend to say it.
    TopoDS_Face landed;
    bool onePanel = true, anyHit = false;
    for (const std::vector<Sample_>& row : samples)
      for (const Sample_& smp : row) {
        if (!smp.hit)
          continue;
        anyHit = true;
        if (landed.IsNull())
          landed = smp.face;
        else if (!smp.face.IsSame(landed))
          onePanel = false;
      }
    if (!anyHit)
      return fail("the cut's outline does not land on the sheet");
    if (onePanel && BRepAdaptor_Surface(landed).GetType() != GeomAbs_Plane)
      onePanel = false; // a bend, or some other curved face: unfold to see

    std::set<int> pieces;
    std::vector<uint64_t> crossed;
    // Which flat piece, or which bends, the outline covers — the question
    // only the unfold can answer, and only asked when it has to be.
    const auto landing = [&](const Placement& all) {
      pieces.clear();
      crossed.clear();
      for (const std::vector<Sample_>& row : samples)
        for (const Sample_& smp : row) {
          if (!smp.hit)
            continue;
          int region = std::numeric_limits<int>::min();
          if (!ToFlat(all, *smp.hit, region))
            continue;
          if (region >= 0) {
            pieces.insert(region);
          } else {
            const uint64_t id =
                all.bends[static_cast<size_t>(-region - 1)].featureId;
            if (std::find(crossed.begin(), crossed.end(), id) == crossed.end())
              crossed.push_back(id);
          }
        }
    };
    std::string why;
    if (!onePanel) {
      const Placement* all = shared.Flat();
      if (all == nullptr)
        return fail(shared.why);
      landing(*all);
      if (pieces.empty() && crossed.empty())
        return fail("the cut's outline does not land on the sheet");
    }

    const auto result = [&](Named cut) -> std::optional<FlatResult> {
      FlatResult r;
      r.shape = std::move(cut.shape);
      r.map = std::move(cut.map);
      Uniquify(r.map);
      r.bends = body.bends;
      return r;
    };
    const auto checked = [&](const Named& cut, double before) -> std::string {
      if (!cut.shape.IsValid() || cut.shape.Volume() <= 1e-9)
        return "the cut would remove the whole sheet";
      if (cut.shape.Volume() >= before - 1e-9)
        return "the cut removed nothing from the sheet";
      return {};
    };

    if (crossed.empty()) {
      // One panel, no bend: lay the cutter out in the blank and move it
      // straight back onto the panel. Nothing is unfolded at all — which is
      // now true of the pass above as well, when every sample landed on the
      // same flat face.
      if (!onePanel && pieces.size() != 1)
        return fail("from this direction the cut's outline lands on more "
                    "than one part of the sheet - sketch it where it sees "
                    "only the face to be cut");
      const TopoDS_Face& face = landed;
      BRepAdaptor_Surface surf(face);
      if (surf.GetType() != GeomAbs_Plane)
        return fail("the cut lands on a curved face that is not a bend");
      gp_Dir out = surf.Plane().Axis().Direction();
      if (face.Orientation() == TopAbs_REVERSED)
        out.Reverse();

      // One flat panel. Where the sketch faces that panel square on — a
      // hole cut straight down onto a flat sheet, which is most of them —
      // the profile goes on AS DRAWN, carried along the sketch's direction
      // and prismed through the sheet. Sampling the wire and laying a
      // polygon down answers the same question, and it is what a cut across
      // a bend has to do, but on a panel it turns a hole into a 380-sided
      // pit: 383 faces where a bored hole has 7, in the file, in the DXF a
      // shop cuts from, and in every boolean afterwards.
      const gp_Pnt seat = surf.Plane().Location();
      const double denom = gp_Vec(out).Dot(gp_Vec(along));
      CADShape cutter;
      if (std::abs(std::abs(denom) - 1.0) < 1e-6) {
        const TopoDS_Wire wire = BRepTools::OuterWire(profile);
        BRepAdaptor_Curve probe(
            TopoDS::Edge(TopExp_Explorer(wire, TopAbs_EDGE).Current()));
        const gp_Pnt on = probe.Value(probe.FirstParameter());
        gp_Trsf onto;
        onto.SetTranslation(gp_Vec(along) *
                            (gp_Vec(on, seat).Dot(gp_Vec(out)) / denom));
        gp_Trsf lift;
        lift.SetTranslation(gp_Vec(out) * eps);
        const TopoDS_Shape laid =
            BRepBuilderAPI_Transform(profile, lift * onto, Standard_True)
                .Shape();
        cutter =
            CADShape(BRepPrimAPI_MakePrism(laid, gp_Vec(out) * -(t + 2.0 * eps))
                         .Shape());
      } else {
        // Oblique: the outline lands on the panel as a squashed copy of
        // itself, and the sampled polygon is what carries it there — laid
        // out in the blank, so this one does need the part laid flat.
        const Placement* flat = shared.Flat();
        if (flat == nullptr)
          return fail(shared.why);
        if (pieces.empty())
          landing(*flat);
        if (pieces.size() != 1)
          return fail("from this direction the cut's outline lands on more "
                      "than one part of the sheet - sketch it where it sees "
                      "only the face to be cut");
        const Placement& all = *flat;
        const gp_Trsf& place =
            all.pieces[static_cast<size_t>(*pieces.begin())].second;
        const CADShape flatCutter =
            cutterIn(all, seat.Transformed(place), out.Transformed(place));
        if (split)
          return fail("from this direction the cut's outline lands on more "
                      "than one part of the sheet - sketch it where it sees "
                      "only the face to be cut");
        cutter = Transformed(flatCutter, place.Inverted());
      }
      if (!cutter.IsValid())
        return fail("the cut's outline does not land on the sheet");
      Named cut = Subtract(Named{body.shape, body.elementMap}, cutter, tag);
      if (const std::string bad = checked(cut, body.shape.Volume());
          !bad.empty())
        return fail(bad);
      return result(std::move(cut));
    }

    // Pass two: unfold only the flanges whose bends the outline crosses, cut
    // flat there, and fold just those back.
    Placement some;
    auto flat = UnfoldImpl(body, rules, {}, tag, crossed, &some, &why);
    if (!flat)
      return fail(why);
    const Placement::Bent& first = some.bends.front();
    const CADShape cutter =
        cutterIn(some, first.f.Tf.Transformed(first.parentPlace),
                 first.f.up.Transformed(first.parentPlace));
    if (split)
      return fail("from this direction the cut's outline lands on more "
                  "than one part of the sheet - sketch it where it sees "
                  "only the face to be cut");
    if (!cutter.IsValid())
      return fail("the cut's outline does not land on the sheet");
    Named cut = Subtract(Named{flat->shape, flat->map}, cutter, tag);
    if (const std::string bad = checked(cut, flat->shape.Volume());
        !bad.empty())
      return fail(bad);
    Body laid = body;
    laid.shape = cut.shape;
    laid.elementMap = cut.map;
    laid.bends = flat->bends;
    return Refold(laid, rules, tag, tag, error);
  } catch (const Standard_Failure& e) {
    return fail(std::string("normal cut failed: ") + e.GetMessageString());
  }
}

} // namespace

std::optional<FlatResult> NormalCut(const Body& body,
                                    const SheetMetalRules& rules,
                                    const std::vector<TopoDS_Face>& profiles,
                                    const gp_Dir& along, uint64_t tag,
                                    std::string* error) {
  if (profiles.empty()) {
    if (error)
      *error = "the sketch has no closed profile to cut";
    return std::nullopt;
  }
  // Every region of the sketch, against ONE ray tree and at most one unfold.
  // Each cut is made on the body the last one left — three burner ports are
  // three holes in one pan — while what they share is read from the body as
  // it was before any of them: cutting a hole moves no panel and turns no
  // bend, so where an outline lands does not depend on the holes beside it.
  CutShared shared(body, rules, tag);
  Body running = body;
  std::optional<FlatResult> out;
  for (const TopoDS_Face& profile : profiles) {
    out = NormalCutOne(running, shared, profile, along, tag, error);
    if (!out)
      return std::nullopt;
    running.shape = out->shape;
    running.elementMap = out->map;
    running.bends = out->bends;
  }
  return out;
}

glm::dvec3 CreaseNormal(const BendRegion& bend, const glm::dmat4& placement) {
  const glm::dvec3 up(placement * bend.parentPlace *
                      glm::dvec4(bend.upDir, 0.0));
  const double len = glm::length(up);
  return len > 1e-12 ? up / len : glm::dvec3(0.0);
}

bool FoldsToward(const BendRegion& bend, const glm::dvec3& side) {
  return glm::dot(CreaseNormal(bend), side) > 0.0;
}

bool SquareTo(const BendRegion& bend, const glm::dvec3& side,
              const glm::dmat4& placement) {
  const double len = glm::length(side);
  if (len < 1e-12)
    return false;
  return std::abs(glm::dot(CreaseNormal(bend, placement), side / len)) >
         1.0 - 1e-6;
}

} // namespace sheet
