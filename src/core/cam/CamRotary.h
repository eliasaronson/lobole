#pragma once

#include "core/Feature.h" // Body
#include "core/cam/CamJob.h"

#include <glm/glm.hpp>

#include <vector>

// The stock for a job whose A axis MOVES (docs/cam/rotary-plan.md, D1).
//
// A height map is z(x, y): the stock as a surface seen from the spindle. It
// cannot represent a part that turns, which is why `docs/cam/simulation.md`
// excluded continuous 4-axis by name and why the G-code parser refuses a file
// whose A moves rather than verifying it as though the part stood still.
//
// This is the same idea in the coordinates a rotary job actually works in:
// r(x, theta), the stock as a RADIUS field about the rotary axis. Everything
// that made the height map trustworthy carries over -- the tool's real shape
// comes from the same `TipHeight`, material only ever comes off, and ONE
// model feeds both the picture and the verdict. What changes is the surface
// it is measured from.
//
// Surveyed before writing (~/Documents/CAD/CAM): FreeCAD leaves its height-map
// simulator untouched and verifies rotary output with 1,873 lines of G-code
// regression tests instead; Kiri:Moto wrote a SECOND simulator, a Manifold CSG
// solid with the tool rotated into the part's frame; blendercam left its 2-D
// height map reachable from a 4-axis path with no guard, which is a verdict
// about a program nobody checked. A radius field is the answer that keeps one
// engine, and its limit is the one every surveyed rotary op imposes anyway:
// the stock is a cylinder on one axis.
namespace cam {

// The rotary axis is SETUP SPACE +X through (0, 0, 0). Not a choice made
// here: `SetupToModel` composes `frame * R_x(A)`, so that is where the
// trunnion is, and the setup frame is what a job uses to put a part on it.
//
// `theta` is measured in the PART's own frame, from +Y toward +Z. At machine
// angle A the part's theta appears in setup space at theta + A -- which is
// the whole kinematic relationship this file needs, and is why the carve can
// take the A of each move without transforming anything else.
struct RadiusMap {
  double x0 = 0.0;  // the low end along the rotary axis
  double res = 0.5; // mm per cell along X
  int nx = 0;
  int nTheta = 0;       // cells around; theta step is 2*pi / nTheta
  std::vector<float> r; // mm from the axis

  [[nodiscard]] float At(int i, int j) const {
    return r[static_cast<size_t>(j) * static_cast<size_t>(nx) +
             static_cast<size_t>(i)];
  }
  void Set(int i, int j, float v) {
    r[static_cast<size_t>(j) * static_cast<size_t>(nx) +
      static_cast<size_t>(i)] = v;
  }
  [[nodiscard]] double CellX(int i) const { return x0 + (i + 0.5) * res; }
  // Radians, at the cell's centre.
  [[nodiscard]] double CellTheta(int j) const {
    return (j + 0.5) * 2.0 * 3.14159265358979323846 / nTheta;
  }
  // How wide a cell is, in mm, at radius `rad` -- what the angular step
  // actually costs at the surface being measured. The caller derives
  // `nTheta` from this rather than typing one, which is the defaults rule:
  // an angular step is only a resolution once you say at what radius.
  [[nodiscard]] double ArcAt(double rad) const {
    return nTheta > 0 ? rad * 2.0 * 3.14159265358979323846 / nTheta : 0.0;
  }
};

// The blank: a cylinder of `radius` from `x0` to `x1` on the axis. `nTheta`
// is derived so that a cell's arc at the blank's own surface is about `res`,
// because an angular grid finer than the axial one measures nothing extra and
// a coarser one is the resolution of the whole check.
[[nodiscard]] RadiusMap MakeRotaryStock(double x0, double x1, double radius,
                                        double res);

// The designed part as a radius field on the same grid: the FURTHEST point of
// the bodies along each (x, theta) ray, or 0 where the ray misses.
//
// Bodies arrive in the part's own frame, which for a continuous job is setup
// space at A = 0. Sampled rather than rasterised: a triangle is not a triangle
// after the map to (x, theta), and one spanning the axis wraps. Each triangle
// is walked at half a cell and its samples splatted, so the error is bounded
// by the surface's slope across half a cell -- the same order as the planar
// grid's own quantisation, and stated here because it is not obvious.
[[nodiscard]] RadiusMap SampleModelRotary(const std::vector<Body>& bodies,
                                          const RadiusMap& like);

// Lowers `stock` where a tool sweeping from `from` to `to` passes, while the
// part turns from `aFrom` to `aTo` degrees. THE one place rotary material is
// removed, for the same reason `CarveSweep` is for the planar map: the
// picture and the verdict must not be able to disagree about it.
//
// The tool points along setup-space -Z and its shape is `TipHeight`, exactly
// as in 3 axes. A null tool is a 2 mm flat mill (the file states a T, not a
// shape).
void CarveSweepRotary(const glm::dvec3& from, const glm::dvec3& to,
                      double aFrom, double aTo, const Tool* tool,
                      RadiusMap& stock);

} // namespace cam
