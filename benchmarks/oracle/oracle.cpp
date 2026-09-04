// bench_oracle — read an exported STEP file, print name-independent geometric
// facts as JSON. Nothing else.
//
// WHY IT LINKS NO DESIGNER CODE
// -----------------------------
// The point of a benchmark oracle is to be able to CONTRADICT the app. An
// oracle that asks the app "did that work?" is the app marking its own
// homework, and this project has already been bitten by exactly that: the GPU
// topology solver returned zero displacement and reported convergence. So this
// is a separate binary over OpenCASCADE's STEP reader — it shares the kernel
// (there is only one on this machine) but none of the feature, regeneration or
// toponaming code, which is where the defects a benchmark is meant to catch
// actually live.
//
// It also reports only facts a NAME cannot fake: solid count, bounding box,
// volume, surface area, centre of mass, the cylindrical faces grouped into
// real holes/bosses, and the planar faces that are not axis-aligned. An agent
// that names a feature "M6 clearance hole" and drills nothing scores zero here.
//
//     bench_oracle part.step            -> facts on stdout
//
// Exit status is 0 whenever the file was read; a file that cannot be read
// prints {"ok": false, "error": ...} and exits 1.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include <BRepAdaptor_Surface.hxx>
#include <BRepBndLib.hxx>
#include <BRepGProp.hxx>
#include <BRepLProp_SLProps.hxx>
#include <BRepTools.hxx>
#include <Bnd_Box.hxx>
#include <GProp_GProps.hxx>
#include <IFSelect_ReturnStatus.hxx>
#include <Interface_Static.hxx>
#include <STEPControl_Reader.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>
#include <gp_Cylinder.hxx>
#include <gp_Dir.hxx>
#include <gp_Pln.hxx>
#include <gp_Pnt.hxx>

#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace {

// Grouping keys are rounded, so a hole split into two half-cylinder faces by
// the modeller lands in one group instead of reading as two holes.
constexpr double kGrid = 1e-3;

double Snap(double v) {
  const double s = std::round(v / kGrid) * kGrid;
  return s == 0.0 ? 0.0 : s; // no "-0"
}

// A direction and its opposite describe the same axis. Canonicalise the sign
// so the two halves of a through hole cannot land in different groups.
gp_Dir Canonical(const gp_Dir& d) {
  const double x = d.X(), y = d.Y(), z = d.Z();
  const double lead = std::abs(x) > 1e-6 ? x : (std::abs(y) > 1e-6 ? y : z);
  return lead < 0 ? gp_Dir(-x, -y, -z) : d;
}

json Vec(const gp_Dir& d) {
  return json::array({Snap(d.X()), Snap(d.Y()), Snap(d.Z())});
}
json Vec(const gp_Pnt& p) {
  return json::array({Snap(p.X()), Snap(p.Y()), Snap(p.Z())});
}

// Outward normal of a face at its UV centre, with the face's orientation
// applied — the surface's own normal ignores REVERSED, and a hole is exactly
// a cylinder whose face is reversed relative to the material.
bool FaceNormal(const TopoDS_Face& face, gp_Pnt* at, gp_Dir* out) {
  Standard_Real u0, u1, v0, v1;
  BRepTools::UVBounds(face, u0, u1, v0, v1);
  BRepLProp_SLProps props(BRepAdaptor_Surface(face), (u0 + u1) / 2,
                          (v0 + v1) / 2, 1, 1e-6);
  if (!props.IsNormalDefined())
    return false;
  gp_Dir n = props.Normal();
  if (face.Orientation() == TopAbs_REVERSED)
    n.Reverse();
  *at = props.Value();
  *out = n;
  return true;
}

struct CylGroup {
  double radius = 0;
  gp_Dir axis;
  gp_Pnt foot; // the axis line's closest point to the origin
  bool internal = false;
  int faces = 0;
  double lo = 1e300, hi = -1e300; // extent along the axis
};

struct PlaneGroup {
  gp_Dir normal;
  double offset = 0; // signed distance from the origin along the normal
  int faces = 0;
  double area = 0;
};

const char* SurfaceKind(GeomAbs_SurfaceType t) {
  switch (t) {
  case GeomAbs_Plane:
    return "plane";
  case GeomAbs_Cylinder:
    return "cylinder";
  case GeomAbs_Cone:
    return "cone";
  case GeomAbs_Sphere:
    return "sphere";
  case GeomAbs_Torus:
    return "torus";
  case GeomAbs_BezierSurface:
    return "bezier";
  case GeomAbs_BSplineSurface:
    return "bspline";
  case GeomAbs_SurfaceOfRevolution:
    return "revolution";
  case GeomAbs_SurfaceOfExtrusion:
    return "extrusion";
  default:
    return "other";
  }
}

} // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::printf(
        "{\"ok\": false, \"error\": \"usage: bench_oracle FILE.step\"}\n");
    return 1;
  }
  const std::string path = argv[1];

  STEPControl_Reader reader;
  // Read the file's own units instead of assuming mm: a benchmark that
  // silently rescales grades the wrong part and calls it a pass.
  Interface_Static::SetCVal("xstep.cascade.unit", "MM");
  if (reader.ReadFile(path.c_str()) != IFSelect_RetDone) {
    std::printf("{\"ok\": false, \"error\": \"cannot read STEP: %s\"}\n",
                path.c_str());
    return 1;
  }
  reader.TransferRoots();
  const TopoDS_Shape shape = reader.OneShape();
  if (shape.IsNull()) {
    std::printf("{\"ok\": false, \"error\": \"STEP holds no shape\"}\n");
    return 1;
  }

  json out;
  out["ok"] = true;
  out["file"] = path;

  int solids = 0, shells = 0;
  for (TopExp_Explorer e(shape, TopAbs_SOLID); e.More(); e.Next())
    solids++;
  for (TopExp_Explorer e(shape, TopAbs_SHELL); e.More(); e.Next())
    shells++;
  out["solids"] = solids;
  out["shells"] = shells;

  Bnd_Box box;
  BRepBndLib::AddOptimal(shape, box);
  if (!box.IsVoid()) {
    Standard_Real xa, ya, za, xb, yb, zb;
    box.Get(xa, ya, za, xb, yb, zb);
    out["bbox"]["min"] = json::array({Snap(xa), Snap(ya), Snap(za)});
    out["bbox"]["max"] = json::array({Snap(xb), Snap(yb), Snap(zb)});
    out["bbox"]["size"] =
        json::array({Snap(xb - xa), Snap(yb - ya), Snap(zb - za)});
  }

  GProp_GProps vol, surf;
  BRepGProp::VolumeProperties(shape, vol);
  BRepGProp::SurfaceProperties(shape, surf);
  out["volume"] = Snap(vol.Mass());
  out["area"] = Snap(surf.Mass());
  out["com"] = Vec(vol.CentreOfMass());

  // Per-solid facts, ranked by volume descending (T4/T5): an assembly
  // exported as several solids is graded by WHERE each one is, and the rank
  // is a stable name for "the big one" / "the small one" that no feature
  // name can fake. The aggregate com above cannot see a part that moved
  // while another moved the opposite way.
  {
    std::vector<std::pair<double, json>> each;
    for (TopExp_Explorer e(shape, TopAbs_SOLID); e.More(); e.Next()) {
      GProp_GProps sv;
      BRepGProp::VolumeProperties(e.Current(), sv);
      Bnd_Box sb;
      BRepBndLib::AddOptimal(e.Current(), sb);
      json one;
      one["volume"] = Snap(sv.Mass());
      one["com"] = Vec(sv.CentreOfMass());
      if (!sb.IsVoid()) {
        Standard_Real xa, ya, za, xb, yb, zb;
        sb.Get(xa, ya, za, xb, yb, zb);
        one["bbox_size"] =
            json::array({Snap(xb - xa), Snap(yb - ya), Snap(zb - za)});
      }
      each.emplace_back(sv.Mass(), one);
    }
    std::sort(each.begin(), each.end(),
              [](const auto& a, const auto& b) { return a.first > b.first; });
    json arr = json::array();
    for (const auto& [v, one] : each)
      arr.push_back(one);
    out["solid_details"] = arr;
  }

  std::map<std::string, int> by_kind;
  std::map<std::string, CylGroup> cyls;
  std::map<std::string, PlaneGroup> planes;
  int faces = 0;

  for (TopExp_Explorer e(shape, TopAbs_FACE); e.More(); e.Next()) {
    const TopoDS_Face face = TopoDS::Face(e.Current());
    faces++;
    BRepAdaptor_Surface sa(face);
    by_kind[SurfaceKind(sa.GetType())]++;

    gp_Pnt at;
    gp_Dir normal;
    const bool has_normal = FaceNormal(face, &at, &normal);

    if (sa.GetType() == GeomAbs_Cylinder) {
      const gp_Cylinder cyl = sa.Cylinder();
      const gp_Dir axis = Canonical(cyl.Axis().Direction());
      const gp_Pnt loc = cyl.Axis().Location();
      // Foot of the axis line: the point on it nearest the origin, so two
      // faces of the same bore share a key wherever their surfaces were cut.
      const double t = loc.XYZ().Dot(axis.XYZ());
      const gp_Pnt foot(loc.X() - t * axis.X(), loc.Y() - t * axis.Y(),
                        loc.Z() - t * axis.Z());
      bool internal = false;
      if (has_normal) {
        // Radial vector from the axis to the sampled point. The material is
        // outside a hole, so a hole's outward normal points back at the axis.
        const double s = at.XYZ().Dot(axis.XYZ()) - foot.XYZ().Dot(axis.XYZ());
        const gp_Vec radial(at.X() - (foot.X() + s * axis.X()),
                            at.Y() - (foot.Y() + s * axis.Y()),
                            at.Z() - (foot.Z() + s * axis.Z()));
        internal = radial.Dot(gp_Vec(normal)) < 0;
      }
      char key[256];
      std::snprintf(key, sizeof(key), "%.3f|%.3f,%.3f,%.3f|%.3f,%.3f,%.3f|%d",
                    Snap(cyl.Radius()), Snap(axis.X()), Snap(axis.Y()),
                    Snap(axis.Z()), Snap(foot.X()), Snap(foot.Y()),
                    Snap(foot.Z()), internal ? 1 : 0);
      CylGroup& g = cyls[key];
      g.radius = cyl.Radius();
      g.axis = axis;
      g.foot = foot;
      g.internal = internal;
      g.faces++;
      Bnd_Box fb;
      BRepBndLib::AddOptimal(face, fb);
      if (!fb.IsVoid()) {
        Standard_Real xa, ya, za, xb, yb, zb;
        fb.Get(xa, ya, za, xb, yb, zb);
        for (int i = 0; i < 8; i++) {
          const gp_Pnt c(i & 1 ? xb : xa, i & 2 ? yb : ya, i & 4 ? zb : za);
          const double p = c.XYZ().Dot(axis.XYZ());
          g.lo = std::min(g.lo, p);
          g.hi = std::max(g.hi, p);
        }
      }
    } else if (sa.GetType() == GeomAbs_Plane && has_normal) {
      const double offset = at.XYZ().Dot(normal.XYZ());
      char key[256];
      std::snprintf(key, sizeof(key), "%.3f,%.3f,%.3f|%.3f", Snap(normal.X()),
                    Snap(normal.Y()), Snap(normal.Z()), Snap(offset));
      PlaneGroup& g = planes[key];
      g.normal = normal;
      g.offset = offset;
      g.faces++;
      GProp_GProps fp;
      BRepGProp::SurfaceProperties(face, fp);
      g.area += fp.Mass();
    }
  }

  out["faces"] = faces;
  out["faces_by_kind"] = json::object();
  for (const auto& [kind, n] : by_kind)
    out["faces_by_kind"][kind] = n;

  // Cylinder groups: one entry per distinct (radius, axis line, side), which
  // is one entry per real hole or boss. Sorted biggest-first so a diff of two
  // runs is readable.
  std::vector<CylGroup> cyl_list;
  for (auto& [key, g] : cyls)
    cyl_list.push_back(g);
  std::sort(
      cyl_list.begin(), cyl_list.end(),
      [](const CylGroup& a, const CylGroup& b) { return a.radius > b.radius; });
  out["cylinders"] = json::array();
  for (const CylGroup& g : cyl_list) {
    out["cylinders"].push_back(
        {{"radius", Snap(g.radius)},
         {"diameter", Snap(2 * g.radius)},
         {"axis", Vec(g.axis)},
         {"axis_point", Vec(g.foot)},
         {"internal", g.internal},
         {"faces", g.faces},
         {"length", g.hi > g.lo ? Snap(g.hi - g.lo) : 0.0}});
  }

  // The same holes collapsed by (diameter, side): "four M6 clearance holes"
  // is a claim about a COUNT, and this is the row that answers it.
  std::map<std::string, json> by_size;
  for (const CylGroup& g : cyl_list) {
    char key[64];
    std::snprintf(key, sizeof(key), "%.3f|%d", Snap(2 * g.radius),
                  g.internal ? 1 : 0);
    if (!by_size.count(key))
      by_size[key] = {{"diameter", Snap(2 * g.radius)},
                      {"internal", g.internal},
                      {"count", 0}};
    by_size[key]["count"] = by_size[key]["count"].get<int>() + 1;
  }
  out["cylinder_groups"] = json::array();
  for (auto& [key, g] : by_size)
    out["cylinder_groups"].push_back(g);

  // Planar faces, and the subset that is not axis-aligned. A "45 degree
  // gusset" that came out square shows up here and nowhere else.
  out["planes"] = json::array();
  int inclined = 0;
  for (const auto& [key, g] : planes) {
    const bool axis_aligned = std::abs(std::abs(g.normal.X()) - 1) < 1e-3 ||
                              std::abs(std::abs(g.normal.Y()) - 1) < 1e-3 ||
                              std::abs(std::abs(g.normal.Z()) - 1) < 1e-3;
    if (!axis_aligned)
      inclined++;
    out["planes"].push_back({{"normal", Vec(g.normal)},
                             {"offset", Snap(g.offset)},
                             {"area", Snap(g.area)},
                             {"faces", g.faces},
                             {"axis_aligned", axis_aligned}});
  }
  out["inclined_planes"] = inclined;

  std::printf("%s\n", out.dump(2).c_str());
  return 0;
}
