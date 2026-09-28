#pragma once

#include "core/Feature.h" // Body
#include "core/cam/CamGcode.h"
#include "core/cam/CamJob.h"
#include "core/cam/CamVerify.h" // Verdict

#include <glm/glm.hpp>

#include <string>
#include <vector>

// The stock for a job whose A axis MOVES
// (docs/cam/rotary.md#verifying-a-turning-part).
//
// A height map is z(x, y): the stock as a surface seen from the spindle. It
// cannot represent a part that turns, which is why a file whose A moves is
// routed here rather than verified as though the part stood still.
//
// This is the same idea in the coordinates a rotary job actually works in:
// r(x, theta), the stock as a RADIUS field about the rotary axis. Everything
// that made the height map trustworthy carries over -- the tool's real shape
// comes from the same `TipHeight`, material only ever comes off, and ONE
// model feeds both the picture and the verdict. What changes is the surface
// it is measured from.
//
// Surveyed before writing (other open-source CAM): FreeCAD leaves its
// height-map simulator untouched and verifies rotary output with 1,873 lines of
// G-code regression tests instead; Kiri:Moto wrote a SECOND simulator, a
// Manifold CSG solid with the tool rotated into the part's frame; blendercam
// left its 2-D height map reachable from a 4-axis path with no guard, which is
// a verdict about a program nobody checked. A radius field is the answer that
// keeps one engine, and its limit is the one every surveyed rotary op imposes
// anyway: the stock is a cylinder on one axis.
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
  // The widest radius this map can ever hold: the blank's, set when it is
  // made. Material only comes off, so it stays a true upper bound for the
  // map's whole life -- which is what lets the carve use it without looking.
  // It used to scan every cell for it on every move: ~190,000 cells times
  // the rolling pin's 644,160 moves, before a single cell was carved.
  double rMax = 0.0;

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

// The cutter-location RADIUS surface: for each (x, theta), the radius at
// which this tool's TIP must sit for the cutter to touch the part without
// cutting into it. The rotary twin of `DropCut`, and the same arithmetic --
//
//   r_CL(x, th) = max over cells p within the tool's radius of
//                   ( r_model(p) - TipHeight(tool, |p - (x, th)|) )
//
// -- with the lateral distance measured as an ARC, hypot(dx, r*dtheta),
// because that is the distance on the surface being cut. `TipHeight` is the
// same function the rotary carve verifies with, so the surface a pass is
// planned against and the shape the check removes cannot disagree about the
// cutter, exactly as in three axes.
//
// It assumes the tool contacts the part at the TOP, where the spindle's axis
// is radial. Every surveyed rotary drop cutter assumes it (FreeCAD rotates
// the STL by -theta and drops along Z, which is the same statement), and a
// 4-axis machine cutting anywhere else is a machine whose A is not being used
// to present the work.
[[nodiscard]] RadiusMap RotaryCL(const RadiusMap& model, const Tool& tool);

// The verdict, from the rotary pair. Same `Verdict` the height map fills, so
// the panel, the RPC and `Explain` need no second vocabulary -- with
// `rotary` set, which is what makes an angle print as an angle.
//
// Compared only where the part IS: a cell whose designed radius is zero is a
// ray that misses the model, which is the blank beyond the part's ends and
// not leftover anybody asked about. Their share is `untouchedFraction`.
[[nodiscard]] Verdict CompareRotary(const RadiusMap& machined,
                                    const RadiusMap& designed,
                                    const OtherSetups* others = nullptr);

// Everything the rotary check produced, for a caller that wants to show it.
struct RotaryInspection {
  Verdict verdict;
  RadiusMap machined;
  RadiusMap designed;
  bool ok = false; // false = nothing to check (no bodies, no rotary blank)
  std::string why; // why not, when ok is false
};

// The blank's radius for a part reaching `partRadius` from the axis: that
// plus the setup's radial margin, the larger of Y and Z because a box's
// margins are all a `Stock` can state. The rule MakeRotaryBlank builds with,
// and the one a roughing layer starts from, so the two cannot disagree.
[[nodiscard]] double RotaryBlankRadius(const Setup& setup, double partRadius);

// The blank a rotary job is cut from, in setup space: the part's extent along
// the axis and its furthest point from it, plus the setup's margins. ONE
// function for the verifier and the simulator, so the picture and the verdict
// start from the same stock -- the one-engine rule, applied to the thing both
// begin with. Empty, with `why`, when there is no part on the axis.
[[nodiscard]] RadiusMap MakeRotaryBlank(const Setup& setup,
                                        const std::vector<Body>& bodies,
                                        double res, std::string* why = nullptr);

// The whole loop for a file whose A MOVES: parse, turn the blank, compare.
//
// The blank is DERIVED, not typed: the part's own extent along the axis and
// its furthest point from it, plus the setup's stock margins -- the radial
// one taken as the larger of the Y and Z margins, because a box's margins
// are the only ones a `Stock` can state and a cylinder has one radius. A
// round stock MODE is the honest fix and is a format change
// (docs/cam/rotary.md#limits).
[[nodiscard]] RotaryInspection
InspectRotary(const std::string& gcode, const Setup& setup,
              const std::vector<Body>& bodies, double res = 0.5,
              const OtherSetups* others = nullptr);

} // namespace cam
