#pragma once

#include "cad/CADKernel.h"

#include <TopoDS_Edge.hxx>
#include <TopoDS_Wire.hxx>
#include <gp_Ax3.hxx>
#include <gp_Pnt2d.hxx>

#include <string>
#include <vector>

// Helical geometry shared by the modelled thread and the Curve3D helix
// (docs/modeling/holes-and-threads.md#modelled-threads). A screw thread and a
// worm are the same solid with a different axial profile: a closed outline in
// the half-plane through the axis, carried round and along it by one screw
// motion. So the profile is the caller's and everything helical is here.

// One helical edge on a cylinder of `radius` about `frame` (its Direction is
// the axis, its XDirection where angle 0 is): a straight line in the
// cylinder's (angle, height) parameter space, rising `lead` per turn, starting
// `startRad` round from the XDirection at height `z0` and running `turns`
// turns. Right-handed turns counter-clockwise about the axis as it rises.
// The 3D curve is materialised. Null edge on bad input.
[[nodiscard]] TopoDS_Edge HelixEdge(const gp_Ax3& frame, double radius,
                                    double lead, double turns, bool rightHanded,
                                    double startRad, double z0 = 0.0);

// The same helix as a wire of one edge per turn (the last one partial). A
// single B-spline approximating many turns loses accuracy turn by turn and
// the sweep along it drifts; per-turn edges keep each approximation to one
// turn, which is what FreeCAD's long helix does for the same reason.
[[nodiscard]] TopoDS_Wire HelixWire(const gp_Ax3& frame, double radius,
                                    double lead, double turns, bool rightHanded,
                                    double startRad, double z0 = 0.0);

struct HelicalSweepSpec {
  // Origin on the axis, Direction the axis, XDirection where start 0 begins.
  gp_Ax3 frame;
  // The axial span (heights along the axis from the frame's origin). The
  // sweep runs a lead past each end, and a squared end is clipped flat
  // there. An end that is not squared is left running on past it by up to a
  // lead and the profile's height: for an end that opens into air, where the
  // cut through it is all that matters and the clip would only cost time.
  double z0 = 0.0, z1 = 0.0;
  bool squareStart = true, squareEnd = true;
  double lead = 0.0; // axial advance of ONE start per turn
  int starts = 1;    // each start is the profile turned 2*pi/starts further
  bool rightHanded = true;
  double startRad = 0.0; // phase of start 0 about the axis
  // Pin the phase by height instead of `startRad`: the profile's y = 0 passes
  // the frame's XDirection (angle 0) at `phaseHeight`, and every lead/starts
  // above and below it. What a worm and the hob that cuts its wheel need, so
  // a tooth stands where the pair's conventions say
  // (docs/modeling/worm-gears.md#phase).
  bool pinPhase = false;
  double phaseHeight = 0.0;
  // The closed axial profile, in the half-plane at the start angle: x is the
  // distance from the axis (> 0), y the height relative to the helix point.
  // Its x range is the solid's radius range. Straight segments between the
  // points; the last joins the first.
  std::vector<gp_Pnt2d> profile;
};

// The profile swept along the helix by a pure screw motion (MakePipeShell in
// binormal mode with the axis as binormal, which keeps the section in the
// axial plane), every start, clipped at its squared ends. The result is a solid
// or a compound of disjoint solids, ready to cut or fuse. Invalid shape and
// `why` on failure; never throws.
[[nodiscard]] CADShape HelicalSweep(const HelicalSweepSpec& spec,
                                    std::string* why = nullptr);
