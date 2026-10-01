#include "core/io/ExportDxf.h"

#include "core/sheet/Unfold.h"

#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepBndLib.hxx>
#include <BRepGProp.hxx>
#include <BRepTools.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box.hxx>
#include <GCPnts_QuasiUniformDeflection.hxx>
#include <GProp_GProps.hxx>
#include <Standard_Failure.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Wire.hxx>
#include <gp_Pln.hxx>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <numbers>

namespace dxf {
namespace {

// One group code / value pair. DXF is a flat list of them, two lines each,
// and every entity is the same handful of codes.
struct Out {
  std::ofstream f;
  void P(int code, const std::string& v) { f << code << "\n" << v << "\n"; }
  void P(int code, double v) {
    f << code << "\n";
    // Fixed notation: some readers choke on 1e-07, and a coordinate that
    // round-trips is worth more than the three characters scientific saves.
    f << std::fixed;
    f.precision(6);
    f << v << "\n";
    f.unsetf(std::ios::floatfield);
  }
  void P(int code, int v) { f << code << "\n" << v << "\n"; }
};

// The blank's plane: the largest planar face of a flat body. Largest rather
// than first because a blank's two big faces are its front and back, and the
// rim faces are a thickness wide.
bool FlatFaceOf(const CADShape& body, TopoDS_Face& out, gp_Pln& plane) {
  double best = 0.0;
  bool found = false;
  for (TopExp_Explorer ex(body.GetShape(), TopAbs_FACE); ex.More(); ex.Next()) {
    const TopoDS_Face face = TopoDS::Face(ex.Current());
    BRepAdaptor_Surface surf(face);
    if (surf.GetType() != GeomAbs_Plane)
      continue;
    GProp_GProps props;
    BRepGProp::SurfaceProperties(face, props);
    if (props.Mass() > best) {
      best = props.Mass();
      out = face;
      plane = surf.Plane();
      found = true;
    }
  }
  return found;
}

// The 2D frame the file is written in: the blank's own plane, X along its
// first axis. A DXF has no third coordinate that anyone reads, so this is
// where the part stops being 3D.
struct Frame {
  gp_Pnt origin;
  gp_Dir x, y;
  [[nodiscard]] std::pair<double, double> To2D(const gp_Pnt& p) const {
    const gp_Vec v(origin, p);
    return {v.Dot(gp_Vec(x)), v.Dot(gp_Vec(y))};
  }
};

void WriteLine(Out& o, const Frame& f, const char* layer, const gp_Pnt& a,
               const gp_Pnt& b) {
  const auto [ax, ay] = f.To2D(a);
  const auto [bx, by] = f.To2D(b);
  o.P(0, std::string("LINE"));
  o.P(8, std::string(layer));
  o.P(10, ax);
  o.P(20, ay);
  o.P(30, 0.0);
  o.P(11, bx);
  o.P(21, by);
  o.P(31, 0.0);
}

// TEXT: what the machine etches. R12 has had it since 1988 and every nesting
// package reads it, so a mark needs no glyph outlines and no font — the
// machine draws the string itself.
void WriteText(Out& o, const char* layer, double x, double y, double height,
               const std::string& text) {
  o.P(0, std::string("TEXT"));
  o.P(8, std::string(layer));
  o.P(10, x);
  o.P(20, y);
  o.P(30, 0.0);
  o.P(40, height);
  o.P(1, text);
}

// An ARC is written CCW from start to end angle, which is what the format
// says and what every reader assumes; a circle whose axis points away from
// the frame's normal therefore has its angles swapped rather than its
// direction reversed.
void WriteArcOrCircle(Out& o, const Frame& f, const char* layer,
                      const BRepAdaptor_Curve& curve, bool closed) {
  const gp_Circ circ = curve.Circle();
  const auto [cx, cy] = f.To2D(circ.Location());
  if (closed) {
    o.P(0, std::string("CIRCLE"));
    o.P(8, std::string(layer));
    o.P(10, cx);
    o.P(20, cy);
    o.P(30, 0.0);
    o.P(40, circ.Radius());
    return;
  }
  const gp_Pnt a = curve.Value(curve.FirstParameter());
  const gp_Pnt b = curve.Value(curve.LastParameter());
  const auto [axp, ayp] = f.To2D(a);
  const auto [bxp, byp] = f.To2D(b);
  const double deg = 180.0 / std::numbers::pi;
  double s = std::atan2(ayp - cy, axp - cx) * deg;
  double e = std::atan2(byp - cy, bxp - cx) * deg;
  // Which way the arc actually runs in the FRAME, not in the curve's own
  // parameter space: a face read from the back reverses it.
  if (circ.Axis().Direction().Dot(gp_Dir(gp_Vec(f.x).Crossed(gp_Vec(f.y)))) <
      0.0)
    std::swap(s, e);
  if (s < 0.0)
    s += 360.0;
  if (e < 0.0)
    e += 360.0;
  o.P(0, std::string("ARC"));
  o.P(8, std::string(layer));
  o.P(10, cx);
  o.P(20, cy);
  o.P(30, 0.0);
  o.P(40, circ.Radius());
  o.P(50, s);
  o.P(51, e);
}

void WriteEdge(Out& o, const Frame& f, const char* layer,
               const TopoDS_Edge& edge) {
  BRepAdaptor_Curve curve(edge);
  switch (curve.GetType()) {
  case GeomAbs_Line:
    WriteLine(o, f, layer, curve.Value(curve.FirstParameter()),
              curve.Value(curve.LastParameter()));
    return;
  case GeomAbs_Circle:
    WriteArcOrCircle(o, f, layer, curve, BRep_Tool::IsClosed(edge));
    return;
  default:
    break;
  }
  // Everything else (a spline rim from a lofted profile) is approximated, and
  // the deflection is stated rather than left to a default: 10 microns is
  // under any laser's kerf, so the approximation cannot be what makes a part
  // out of tolerance.
  GCPnts_QuasiUniformDeflection pts(curve, 0.01);
  if (!pts.IsDone() || pts.NbPoints() < 2)
    return;
  for (int i = 1; i < pts.NbPoints(); ++i)
    WriteLine(o, f, layer, pts.Value(i), pts.Value(i + 1));
}

void WriteHeader(Out& o) {
  o.P(0, std::string("SECTION"));
  o.P(2, std::string("HEADER"));
  o.P(9, std::string("$ACADVER"));
  o.P(1, std::string("AC1009")); // R12: the most widely read dialect there is
  // Millimetres. A file that does not say costs an argument with the shop at
  // best and a part at 25.4x at worst.
  o.P(9, std::string("$INSUNITS"));
  o.P(70, 4);
  o.P(0, std::string("ENDSEC"));
}

void WriteTables(Out& o) {
  o.P(0, std::string("SECTION"));
  o.P(2, std::string("TABLES"));

  o.P(0, std::string("TABLE"));
  o.P(2, std::string("LTYPE"));
  o.P(70, 2);
  o.P(0, std::string("LTYPE"));
  o.P(2, std::string("CONTINUOUS"));
  o.P(70, 0);
  o.P(3, std::string("Solid line"));
  o.P(72, 65);
  o.P(73, 0);
  o.P(40, 0.0);
  o.P(0, std::string("LTYPE"));
  o.P(2, std::string("DASHED"));
  o.P(70, 0);
  o.P(3, std::string("__ __ __ __"));
  o.P(72, 65);
  o.P(73, 2);
  o.P(40, 15.0);
  o.P(49, 12.0);
  o.P(49, -3.0);
  o.P(0, std::string("ENDTAB"));

  o.P(0, std::string("TABLE"));
  o.P(2, std::string("LAYER"));
  o.P(70, 4);
  const auto layer = [&](const char* name, int colour, const char* ltype) {
    o.P(0, std::string("LAYER"));
    o.P(2, std::string(name));
    o.P(70, 0);
    o.P(62, colour);
    o.P(6, std::string(ltype));
  };
  // Red up, blue down: the convention every sheet shop's nesting software
  // and every fab portal reads, so a bend line is not something the operator
  // has to be told about in an email.
  layer("CUT", 7, "CONTINUOUS");
  layer("BEND_UP", 1, "DASHED");
  layer("BEND_DOWN", 5, "DASHED");
  layer("MARK", 3, "CONTINUOUS");
  o.P(0, std::string("ENDTAB"));
  o.P(0, std::string("ENDSEC"));
}

} // namespace

std::string WriteFlatPattern(const FlatDrawing& drawing,
                             const std::string& path) {
  if (!drawing.body.IsValid())
    return "there is no body to export";
  ArmOccSignalHandlers();
  try {
    TopoDS_Face face;
    gp_Pln plane;
    if (!FlatFaceOf(drawing.body, face, plane))
      return "this body has no flat face to take an outline from";

    // A blank is one gauge thick, so its two big faces are parallel and the
    // rest are a thickness wide. A folded part's biggest face is just one of
    // its panels, and its outline would be a picture of a part that cuts to
    // nothing — so refuse rather than write it.
    //
    // Measured ALONG THE FACE'S NORMAL, over the body's own vertices. The
    // first version projected the axis-aligned bounding box onto that normal
    // instead, which is right only for a blank lying square to the axes: a
    // plate at 45 degrees has a box mostly full of air, and the sum said it
    // was as thick as it was wide. A suspension tab welded to an angled tube
    // was refused for not being flat while the identical tab on a
    // straight-ahead tube went out fine.
    glm::dvec3 lo, hi;
    drawing.body.GetBoundingBox(lo, hi);
    const gp_Dir n = plane.Axis().Direction();
    const glm::dvec3 size = hi - lo;
    const double longest = std::max({size.x, size.y, size.z});
    double nearest = 0.0, furthest = 0.0;
    {
      TopTools_IndexedMapOfShape verts;
      TopExp::MapShapes(drawing.body.GetShape(), TopAbs_VERTEX, verts);
      const gp_Vec normal(n);
      for (int i = 1; i <= verts.Extent(); i++) {
        const gp_Pnt v = BRep_Tool::Pnt(TopoDS::Vertex(verts(i)));
        // Signed, as a dot product: a vertex ON the plane gives a zero
        // vector, and asking gp_Dir to normalise one of those throws.
        const double d = gp_Vec(plane.Location(), v).Dot(normal);
        if (i == 1) {
          nearest = furthest = d;
        } else {
          nearest = std::min(nearest, d);
          furthest = std::max(furthest, d);
        }
      }
    }
    const double across = furthest - nearest;
    if (across > longest * 0.2)
      return "this body is not flat - export the flat pattern (a FlatPattern "
             "feature, shown by its configuration), not the folded part";

    // The side the file is READ from, which is what UP and DOWN mean: the
    // side the first flange was picked on (its parent panel's face, carried
    // to where the blank lies). The largest face is either of the blank's
    // two, so its own normal would make every bend's layer a coin toss.
    gp_Dir reader = n;
    for (const BendRegion& b : drawing.creases) {
      if (!b.flat)
        continue;
      const glm::dvec3 up(b.parentPlace * glm::dvec4(b.upDir, 0.0));
      if (gp_Vec(up.x, up.y, up.z).Dot(gp_Vec(n)) < 0.0)
        reader = n.Reversed();
      break;
    }
    Frame frame;
    frame.origin = plane.Location();
    frame.x = plane.XAxis().Direction();
    frame.y = reader.Crossed(frame.x);

    Out o;
    o.f.open(path);
    if (!o.f)
      return "could not open '" + path + "' for writing";
    WriteHeader(o);
    WriteTables(o);
    o.P(0, std::string("SECTION"));
    o.P(2, std::string("ENTITIES"));

    // The outline and every hole: the face's own wires, outer and inner
    // alike. A hole is a closed loop on the same layer, which is exactly what
    // a cutter wants — it decides pierce order itself.
    for (TopExp_Explorer wx(face, TopAbs_WIRE); wx.More(); wx.Next())
      for (TopExp_Explorer ex(wx.Current(), TopAbs_EDGE); ex.More(); ex.Next())
        WriteEdge(o, frame, "CUT", TopoDS::Edge(ex.Current()));

    // The bend lines, each on the layer its direction says — as seen by
    // whoever reads the file, looking down the frame's normal.
    const glm::dvec3 side(reader.X(), reader.Y(), reader.Z());
    for (const BendRegion& b : drawing.creases) {
      if (!b.flat)
        continue;
      WriteLine(o, frame, sheet::FoldsToward(b, side) ? "BEND_UP" : "BEND_DOWN",
                gp_Pnt(b.flatA.x, b.flatA.y, b.flatA.z),
                gp_Pnt(b.flatB.x, b.flatB.y, b.flatB.z));
    }

    // The mark, inside the blank rather than beside it: a laser etches what
    // is on the part, and a string floating off the edge would be cut on
    // thin air. Placed at the blank's own centre, sized to a tenth of its
    // shorter side so it fits whatever the blank is, and clamped to
    // something a machine can actually draw.
    if (!drawing.mark.empty()) {
      Bnd_Box box;
      BRepBndLib::Add(face, box);
      if (!box.IsVoid()) {
        double x0, y0, z0, x1, y1, z1;
        box.Get(x0, y0, z0, x1, y1, z1);
        const auto [ax, ay] = frame.To2D(gp_Pnt(x0, y0, z0));
        const auto [bx, by] = frame.To2D(gp_Pnt(x1, y1, z1));
        const double w = std::abs(bx - ax), h = std::abs(by - ay);
        const double height = std::clamp(std::min(w, h) * 0.1, 3.0, 12.0);
        // Left of centre by half the string's width, so it reads centred:
        // R12 TEXT has an alignment field, and the simplest thing every
        // reader agrees on is a left-justified string placed where it goes.
        const double wide = 0.6 * height * drawing.mark.size();
        WriteText(o, "MARK", (ax + bx) * 0.5 - wide * 0.5,
                  (ay + by) * 0.5 - height * 0.5, height, drawing.mark);
      }
    }

    o.P(0, std::string("ENDSEC"));
    o.P(0, std::string("EOF"));
    o.f.close();
    return {};
  } catch (const Standard_Failure& e) {
    return std::string("DXF export failed: ") + e.GetMessageString();
  }
}

} // namespace dxf
