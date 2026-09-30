#include "core/HelicalSweep.h"

#include "core/features/SweepFeature.h"

#include <BRepAlgoAPI_Common.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepGProp.hxx>
#include <BRepLib.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRep_Builder.hxx>
#include <GProp_GProps.hxx>
#include <Geom2d_Line.hxx>
#include <Geom_CylindricalSurface.hxx>
#include <Standard_Failure.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <gp_Ax1.hxx>
#include <gp_Ax2.hxx>
#include <gp_Dir2d.hxx>
#include <gp_Trsf.hxx>

#include <algorithm>
#include <cmath>

namespace {

void Fail(std::string* why, const std::string& msg) {
  if (why)
    *why = msg;
}

} // namespace

TopoDS_Edge HelixEdge(const gp_Ax3& frame, double radius, double lead,
                      double turns, bool rightHanded, double startRad,
                      double z0) {
  if (!(radius > 1e-9) || !(lead > 1e-9) || !(turns > 1e-9))
    return TopoDS_Edge();
  Handle(Geom_CylindricalSurface) cyl =
      new Geom_CylindricalSurface(frame, radius);
  // Helix as a straight line in the cylinder's (u = angle, v = height)
  // parameter space: slope k = lead/2pi rises v by `lead` per 2pi of angle. A
  // unit 2D direction means the line's own parameter w maps u = w/|(1,k)|, so
  // w runs to 2pi*turns*|(1,k)| for exactly `turns` turns. Left-handed
  // reverses the angular sense (u decreases while the height still rises).
  const double k = lead / (2.0 * M_PI);
  const double norm = std::sqrt(1.0 + k * k);
  const double wLast = 2.0 * M_PI * turns * norm;
  const gp_Dir2d dir(rightHanded ? 1.0 : -1.0, k);
  Handle(Geom2d_Line) line2d = new Geom2d_Line(gp_Pnt2d(startRad, z0), dir);
  TopoDS_Edge edge = BRepBuilderAPI_MakeEdge(line2d, cyl, 0.0, wLast).Edge();
  BRepLib::BuildCurve3d(edge);
  return edge;
}

TopoDS_Wire HelixWire(const gp_Ax3& frame, double radius, double lead,
                      double turns, bool rightHanded, double startRad,
                      double z0) {
  if (!(turns > 1e-9) || !(lead > 1e-9))
    return TopoDS_Wire();
  BRepBuilderAPI_MakeWire wire;
  const double sense = rightHanded ? 1.0 : -1.0;
  double done = 0.0;
  while (done < turns - 1e-9) {
    const double t = std::min(1.0, turns - done);
    const TopoDS_Edge e =
        HelixEdge(frame, radius, lead, t, rightHanded,
                  startRad + sense * 2.0 * M_PI * done, z0 + lead * done);
    if (e.IsNull())
      return TopoDS_Wire();
    wire.Add(e);
    if (!wire.IsDone())
      return TopoDS_Wire();
    done += t;
  }
  return wire.Wire();
}

CADShape HelicalSweep(const HelicalSweepSpec& spec, std::string* why) {
  if (spec.profile.size() < 3) {
    Fail(why, "the helical profile needs at least three points");
    return CADShape();
  }
  if (!(spec.lead > 1e-9) || spec.starts < 1 || !(spec.z1 > spec.z0)) {
    Fail(why, "a helical sweep needs a positive lead, at least one start and "
              "a span of positive length");
    return CADShape();
  }
  double xMin = 1e300, xMax = -1e300, yMin = 1e300, yMax = -1e300;
  for (const gp_Pnt2d& p : spec.profile) {
    xMin = std::min(xMin, p.X());
    xMax = std::max(xMax, p.X());
    yMin = std::min(yMin, p.Y());
    yMax = std::max(yMax, p.Y());
  }
  if (!(xMin > 1e-9)) {
    Fail(why, "the helical profile must lie off the axis (radius > 0)");
    return CADShape();
  }
  if (yMax - yMin >= spec.lead / spec.starts) {
    Fail(why, "the helical profile is wider than the pitch, so neighbouring "
              "turns would overlap");
    return CADShape();
  }

  try {
    // Every point of [z0, z1] at every angle must lie under a complete
    // profile: start a lead below z0 (less the profile's top) and end a lead
    // above z1, and let the clip square the ends.
    const double hs = spec.z0 - yMax - spec.lead;
    const double he = spec.z1 - yMin + spec.lead;
    const double turns = (he - hs) / spec.lead;
    // The spine runs through the profile's middle; any radius would do (the
    // binormal frame carries the section rigidly), but inside the profile
    // keeps the sweep's own approximation local to it.
    const double rh = 0.5 * (xMin + xMax);
    const TopoDS_Wire spine = HelixWire(spec.frame, rh, spec.lead, turns,
                                        spec.rightHanded, spec.startRad, hs);
    if (spine.IsNull()) {
      Fail(why, "the helix could not be built");
      return CADShape();
    }

    // The section at the spine's start, in the axial half-plane there.
    const gp_Pnt o = spec.frame.Location();
    const gp_Dir d = spec.frame.Direction();
    const gp_Vec radial =
        gp_Vec(spec.frame.XDirection()) * std::cos(spec.startRad) +
        gp_Vec(spec.frame.YDirection()) * std::sin(spec.startRad);
    BRepBuilderAPI_MakePolygon poly;
    for (const gp_Pnt2d& p : spec.profile)
      poly.Add(o.Translated(gp_Vec(d) * (hs + p.Y()) + radial * p.X()));
    poly.Close();
    if (!poly.IsDone()) {
      Fail(why, "the helical profile is not a closed outline");
      return CADShape();
    }

    // Binormal = the axis: the section stays in the plane through the axis
    // as it goes round, so every point of it moves by the same screw motion.
    // Frenet on the per-turn helix is the fallback (what FreeCAD and the
    // fasteners workbench ship); corrected Frenet keeps the section normal
    // to the tangent, which tilts it by the lead angle, so it is not one.
    CADShape swept = SweepProfileAlongSpine(
        poly.Wire(), spine, SweepFrame::Binormal, TopoDS_Wire(), d);
    if (!swept.IsValid())
      swept = SweepProfileAlongSpine(poly.Wire(), spine, SweepFrame::Frenet);
    if (!swept.IsValid()) {
      Fail(why, "the helical sweep failed");
      return CADShape();
    }
    TopoDS_Shape one = swept.GetShape();
    // MakeSolid can close a shell whose orientation comes out inward; a
    // negative volume here would make the cut add material.
    {
      GProp_GProps props;
      BRepGProp::VolumeProperties(one, props);
      if (props.Mass() < 0.0)
        one.Reverse();
    }

    TopoDS_Compound all;
    BRep_Builder builder;
    builder.MakeCompound(all);
    for (int k = 0; k < spec.starts; k++) {
      if (k == 0) {
        builder.Add(all, one);
        continue;
      }
      gp_Trsf rot;
      rot.SetRotation(gp_Ax1(o, d), 2.0 * M_PI * k / spec.starts);
      builder.Add(all, BRepBuilderAPI_Transform(one, rot, true).Shape());
    }

    if (!spec.squareStart && !spec.squareEnd)
      return CADShape(all); // both ends run out into air: nothing to trim

    // Square ends: keep only what lies past each squared end, inside a
    // cylinder wider than the profile reaches. An end left open is not
    // trimmed (one cap plane fewer for the boolean to intersect: the clip is
    // the costlier half of building the tool).
    const double clipR = xMax * 1.5 + spec.lead;
    const double from = spec.squareStart ? spec.z0 : hs - spec.lead;
    const double to = spec.squareEnd ? spec.z1 : he + spec.lead;
    const gp_Ax2 base(o.Translated(gp_Vec(d) * from), d);
    const TopoDS_Shape slab =
        BRepPrimAPI_MakeCylinder(base, clipR, to - from).Shape();
    BRepAlgoAPI_Common common(all, slab);
    common.SetRunParallel(false);
    common.Build();
    if (!common.IsDone() || common.HasErrors()) {
      Fail(why, "clipping the helical sweep to its span failed");
      return CADShape();
    }
    const TopoDS_Shape clipped = common.Shape();
    GProp_GProps props;
    BRepGProp::VolumeProperties(clipped, props);
    if (!(props.Mass() > 0.0)) {
      Fail(why, "the clipped helical sweep is empty");
      return CADShape();
    }
    return CADShape(clipped);
  } catch (const Standard_Failure& e) {
    Fail(why, std::string("the helical sweep failed: ") + e.GetMessageString());
    return CADShape();
  }
}
