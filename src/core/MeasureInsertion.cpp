// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/MeasureInsertion.h"

#include "cad/NonDestructiveBoolean.h"
#include "core/Feature.h"

#include <BRepAlgoAPI_Common.hxx>
#include <BRepBndLib.hxx>
#include <BRepExtrema_DistShapeShape.hxx>
#include <BRepGProp.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRep_Builder.hxx>
#include <Bnd_Box.hxx>
#include <GProp_GProps.hxx>
#include <Standard_Failure.hxx>
#include <TopLoc_Location.hxx>
#include <TopoDS_Compound.hxx>
#include <gp_Ax3.hxx>
#include <gp_Quaternion.hxx>
#include <gp_Trsf.hxx>

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

namespace Measure {

namespace {

// The clash check's fuzz (Measure.cpp's kContactFuzz): faces that only
// touch share a face, and an unfuzzed boolean over them returns a sliver.
constexpr double kFuzz = 1.0e-3;
// An overlap below this is the boolean's noise, not matter in matter.
constexpr double kVolumeTol = 1.0e-9;
// The distance query's noise on analytic faces, mm.
constexpr double kGapTol = 1.0e-6;

gp_Trsf Rigid(const glm::dmat4& m) {
  const glm::dquat q = glm::normalize(glm::quat_cast(glm::dmat3(m)));
  gp_Trsf t;
  t.SetRotation(gp_Quaternion(q.x, q.y, q.z, q.w));
  t.SetTranslationPart(gp_Vec(m[3][0], m[3][1], m[3][2]));
  return t;
}

// One compound of a side's matter, placed and then carried into the
// axis's frame (the axis is that frame's Z through its origin).
TopoDS_Compound Side(const std::vector<Body>& bodies, const glm::dmat4& xf,
                     const gp_Trsf& toAxis, bool& any) {
  BRep_Builder bb;
  TopoDS_Compound c;
  bb.MakeCompound(c);
  const TopLoc_Location loc(toAxis * Rigid(xf));
  for (const Body& b : bodies)
    if (!b.reference && !b.shape.GetShape().IsNull()) {
      bb.Add(c, b.shape.GetShape().Moved(loc));
      any = true;
    }
  return c;
}

Bnd_Box BoxOf(const TopoDS_Shape& s) {
  Bnd_Box b;
  BRepBndLib::Add(s, b, false);
  return b;
}

struct Range {
  double lo = 0.0, hi = 0.0;
};

Range ZOf(const Bnd_Box& b) {
  double x0, y0, z0, x1, y1, z1;
  b.Get(x0, y0, z0, x1, y1, z1);
  return {z0, z1};
}

double VolumeOf(const TopoDS_Shape& s) {
  GProp_GProps p;
  BRepGProp::VolumeProperties(s, p);
  return std::max(0.0, p.Mass());
}

double AreaOf(const TopoDS_Shape& s) {
  GProp_GProps p;
  BRepGProp::SurfaceProperties(s, p);
  return std::max(0.0, p.Mass());
}

struct Reading {
  bool interfering = false;
  double depthMm = 0.0;   // allowance - gap; the gap negative when overlapped
  double volumeMm3 = 0.0; // the model's own overlap
};

struct KernelError {
  std::string what;
};

// Do a and b interfere: overlap in the model, or (with an allowance) come
// nearer than it. With an allowance the distance is asked first: apart, and
// neither inside the other, they cannot overlap, and only faces that meet
// need the boolean's volume.
Reading Interferes(const TopoDS_Shape& a, const TopoDS_Shape& b,
                   double allowance) {
  Reading r;
  if (allowance > 0.0) {
    BRepExtrema_DistShapeShape dist;
    dist.SetMultiThread(true);
    dist.LoadS1(a);
    dist.LoadS2(b);
    dist.SetFlag(Extrema_ExtFlag_MIN);
    if (!dist.Perform() || !dist.IsDone())
      throw KernelError{"the distance query failed"};
    const double gap = dist.Value();
    if (gap > kFuzz && !dist.InnerSolution()) {
      // A gap exactly the allowance is line to line as made, not a press:
      // the distance query's own noise is kept on the free side of it.
      if (gap < allowance - kGapTol) {
        r.interfering = true;
        r.depthMm = allowance - gap;
      }
      return r;
    }
  }
  BRepAlgoAPI_Common common;
  if (!cad::BuildNonDestructive(common, a, b, kFuzz) || common.Shape().IsNull())
    throw KernelError{"the overlap boolean failed"};
  const double v = VolumeOf(common.Shape());
  if (v > kVolumeTol) {
    r.interfering = true;
    r.volumeMm3 = v;
    // The overlap of a slide is a thin skin; twice its volume over its
    // area is its thickness.
    const double area = AreaOf(common.Shape());
    r.depthMm = allowance + (area > 0.0 ? 2.0 * v / area : 0.0);
  } else if (allowance > 0.0) {
    // Faces on faces: no gap at all.
    r.interfering = true;
    r.depthMm = allowance;
  }
  return r;
}

struct Sides {
  TopoDS_Shape moving, fixed;
  Bnd_Box movingBox, fixedBox;
  double allowance = 0.0;
};

// Does `a` (the moving side, placed) interfere with the fixed side?
Reading Read(const Sides& s, const TopoDS_Shape& a) {
  return Interferes(a, s.fixed, s.allowance);
}

gp_Trsf Pose(double u, double rotationDeg, bool positive) {
  gp_Trsf rot;
  rot.SetRotation(gp_Ax1(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1)),
                  rotationDeg * std::numbers::pi / 180.0);
  gp_Trsf tr;
  tr.SetTranslation(gp_Vec(0.0, 0.0, positive ? u : -u));
  return tr * rot;
}

bool BoxesMeet(const Bnd_Box& a, const Bnd_Box& b, double gap) {
  Bnd_Box g = a;
  g.Enlarge(gap);
  return !g.IsOut(b);
}

// The press at the seat: the axial length over which the seated part and
// the fixed set interfere, read slice by slice (each `step` thick).
double PressAt(const Sides& s, const gp_Trsf& seat, double step,
               const std::atomic<bool>* cancel) {
  const TopoDS_Shape a = s.moving.Moved(TopLoc_Location(seat));
  const Bnd_Box ab = s.movingBox.Transformed(seat);
  if (!BoxesMeet(ab, s.fixedBox, s.allowance))
    return 0.0;
  const Range za = ZOf(ab), zb = ZOf(s.fixedBox);
  const double lo = std::max(za.lo, zb.lo), hi = std::min(za.hi, zb.hi);
  if (hi <= lo)
    return 0.0;
  double ax0, ay0, az0, ax1, ay1, az1, bx0, by0, bz0, bx1, by1, bz1;
  ab.Get(ax0, ay0, az0, ax1, ay1, az1);
  s.fixedBox.Get(bx0, by0, bz0, bx1, by1, bz1);
  const double x0 = std::min(ax0, bx0) - 1.0, x1 = std::max(ax1, bx1) + 1.0;
  const double y0 = std::min(ay0, by0) - 1.0, y1 = std::max(ay1, by1) + 1.0;
  // Each slice counted by its own thickness: the last is a part one.
  double pressed = 0.0;
  for (double z = lo; z < hi - 1e-9; z += step) {
    if (cancel && cancel->load())
      return 0.0;
    const TopoDS_Shape slab =
        BRepPrimAPI_MakeBox(gp_Pnt(x0, y0, z),
                            gp_Pnt(x1, y1, std::min(z + step, hi)))
            .Shape();
    BRepAlgoAPI_Common cut;
    if (!cad::BuildNonDestructive(cut, a, slab) || cut.Shape().IsNull())
      throw KernelError{"slicing the seated part failed"};
    if (VolumeOf(cut.Shape()) <= kVolumeTol)
      continue;
    if (Read(s, cut.Shape()).interfering)
      pressed += std::min(z + step, hi) - z;
  }
  return pressed;
}

} // namespace

std::expected<InsertionReport, std::string>
Insertion(const std::vector<Body>& moving, const glm::dmat4& xfMoving,
          const std::vector<Body>& fixed, const glm::dmat4& xfFixed,
          const glm::dvec3& axisPoint, const glm::dvec3& axisDir,
          const InsertionOptions& opt, const std::atomic<bool>* cancel,
          std::atomic<double>* fraction) {
  const double len = glm::length(axisDir);
  if (!(len > 1e-12))
    return std::unexpected("the axis has no direction");
  if (opt.samples < 2)
    return std::unexpected("samples is at least 2");
  if (opt.rotations < 1)
    return std::unexpected("rotations is at least 1");
  if (opt.allowanceMm < 0.0)
    return std::unexpected("allowance_mm is not negative: it is how much "
                           "nearer the faces come as made");
  // A thread goes on by turning along its helix, which a straight slide
  // reads as the flanks forced through each other the whole way.
  const auto threaded = [](const std::vector<Body>& side) {
    return std::any_of(side.begin(), side.end(), [](const Body& b) {
      return !b.reference && (!b.threads.empty() || !b.threadRegions.empty());
    });
  };
  if (threaded(moving) && threaded(fixed))
    return std::unexpected(
        "both sides carry modelled threads: a threaded pair goes on by "
        "turning, which a straight slide does not judge");
  InsertionReport rep;
  rep.axisPoint = axisPoint;
  rep.axisDir = axisDir / len;
  rep.allowanceMm = opt.allowanceMm;
  rep.samples = opt.samples;
  try {
    const gp_Ax3 ax(gp_Pnt(axisPoint.x, axisPoint.y, axisPoint.z),
                    gp_Dir(rep.axisDir.x, rep.axisDir.y, rep.axisDir.z));
    gp_Trsf toAxis;
    toAxis.SetTransformation(ax);
    bool anyA = false, anyB = false;
    Sides s;
    s.moving = Side(moving, xfMoving, toAxis, anyA);
    s.fixed = Side(fixed, xfFixed, toAxis, anyB);
    if (!anyA || !anyB)
      return std::unexpected("one side places no visible matter (hidden, or "
                             "a part with no bodies)");
    s.movingBox = BoxOf(s.moving);
    s.fixedBox = BoxOf(s.fixed);
    s.allowance = opt.allowanceMm;
    // Each side clipped, once, to what the other can ever come near: a
    // cylinder about the axis as wide as the other reaches from it (and a
    // millimetre more than the allowance), which a turn about the axis
    // keeps. A housing round a wheel, a plate round an arbor, is then the
    // part of it by the axis, and every step's boolean is on that.
    {
      const auto reach = [](const Bnd_Box& b) {
        double x0, y0, z0, x1, y1, z1;
        b.Get(x0, y0, z0, x1, y1, z1);
        return std::hypot(std::max(std::abs(x0), std::abs(x1)),
                          std::max(std::abs(y0), std::abs(y1)));
      };
      const Range a0 = ZOf(s.movingBox), b0 = ZOf(s.fixedBox);
      const double g = opt.allowanceMm + 1.0;
      const double lo = std::min(a0.lo, b0.lo), hi = std::max(a0.hi, b0.hi);
      const double span = hi - lo;
      const auto clip = [&](TopoDS_Shape& sh, Bnd_Box& box, double r) {
        if (r >= reach(box))
          return;
        const TopoDS_Shape cyl =
            BRepPrimAPI_MakeCylinder(
                gp_Ax2(gp_Pnt(0, 0, lo - 2.0 * span - g), gp_Dir(0, 0, 1)), r,
                5.0 * span + 2.0 * g)
                .Shape();
        BRepAlgoAPI_Common c;
        if (!cad::BuildNonDestructive(c, sh, cyl) || c.Shape().IsNull())
          throw KernelError{"clipping a side to the axis failed"};
        sh = c.Shape();
        box = BoxOf(sh);
      };
      const double ra = reach(s.movingBox) + g, rb = reach(s.fixedBox) + g;
      clip(s.moving, s.movingBox, rb);
      clip(s.fixed, s.fixedBox, ra);
    }
    if (s.movingBox.IsVoid() || s.fixedBox.IsVoid()) {
      // Nothing of either ever comes near the other.
      InsertionTry t;
      t.pass = true;
      rep.best = t;
      rep.tries.push_back(t);
      rep.pass = true;
      if (fraction)
        fraction->store(1.0);
      return rep;
    }
    const Range za = ZOf(s.movingBox), zb = ZOf(s.fixedBox);

    std::vector<bool> sides;
    if (opt.entry != InsertionEntry::Negative)
      sides.push_back(true);
    if (opt.entry != InsertionEntry::Positive)
      sides.push_back(false);
    // One step for every try, from the longer of the two travels, so the
    // two sides read at one resolution.
    const auto travelOf = [&](bool positive) {
      const double d = positive ? zb.hi - za.lo : za.hi - zb.lo;
      return std::max(0.0, d + opt.allowanceMm + kFuzz);
    };
    double longest = 0.0;
    for (bool p : sides)
      longest = std::max(longest, travelOf(p));
    rep.stepMm = longest / opt.samples;
    rep.leadInMm = opt.leadInMm >= 0.0 ? opt.leadInMm : rep.stepMm;

    // Every try's work, for the fraction: its slices and its steps.
    const double perTry =
        2.0 * opt.samples + 1.0; // a press read and a sweep, about as long
    const double total =
        perTry * static_cast<double>(sides.size() * opt.rotations);
    double done = 0.0;
    const auto advance = [&](double n) {
      done += n;
      if (fraction)
        fraction->store(std::min(0.99, done / total));
    };

    bool haveBest = false;
    // The seated press and reading, per side: a turned try is judged
    // against the press where the part is placed, and seats only where it
    // interferes there no more than the placed part does.
    struct Seated {
      double pressMm = 0.0;
      Reading at;
    };
    std::vector<Seated> seated(sides.size());
    const auto run = [&](size_t side, double rotationDeg) {
      const bool positive = sides[side];
      InsertionTry t;
      t.positive = positive;
      t.rotationDeg = rotationDeg;
      t.travelMm = travelOf(positive);
      if (rep.stepMm <= 0.0 || t.travelMm <= 0.0) {
        t.pass = true;
        return t;
      }
      const double step = rep.stepMm;
      if (rotationDeg == 0.0) {
        // A seat that does not interfere at all has no press to read.
        seated[side].at = Read(s, s.moving);
        seated[side].pressMm =
            seated[side].at.interfering
                ? PressAt(s, Pose(0.0, 0.0, positive), step, cancel)
                : 0.0;
        advance(opt.samples);
      } else {
        const Reading r = Read(s, s.moving.Moved(TopLoc_Location(
                                      Pose(0.0, rotationDeg, positive))));
        const Reading& at = seated[side].at;
        if (r.depthMm > at.depthMm + 1e-6 ||
            r.volumeMm3 > at.volumeMm3 + 1e-6) {
          t.seats = false;
          t.worstDepthMm = r.depthMm;
          t.worstVolumeMm3 = r.volumeMm3;
          advance(perTry);
          return t;
        }
      }
      t.pressMm = seated[side].pressMm;
      int hits = 0;
      int deeper = 0;
      // Forced deeper than the seat is pressed: a bind, however short.
      const double seatDepth = seated[side].at.depthMm;
      const double deeperTol = 0.5 * s.allowance + kFuzz;
      const int n = static_cast<int>(std::ceil(t.travelMm / step - 1e-9));
      for (int k = n; k >= 1; k--) {
        if (cancel && cancel->load())
          throw KernelError{"cancelled"};
        const double u = k * step;
        const gp_Trsf pose = Pose(u, rotationDeg, positive);
        advance(1.0);
        if (!BoxesMeet(s.movingBox.Transformed(pose), s.fixedBox, s.allowance))
          continue;
        const Reading r = Read(s, s.moving.Moved(TopLoc_Location(pose)));
        if (!r.interfering)
          continue;
        hits++;
        if (r.depthMm > seatDepth + deeperTol)
          deeper++;
        t.fromMm = std::max(t.fromMm, u);
        t.worstDepthMm = std::max(t.worstDepthMm, r.depthMm);
        t.worstVolumeMm3 = std::max(t.worstVolumeMm3, r.volumeMm3);
        // A try that cannot beat the best found is abandoned.
        if (haveBest && hits * step - t.pressMm > rep.best.excessMm) {
          t.complete = false;
          break;
        }
      }
      t.interferingMm = hits * step;
      t.deeperMm = deeper * step;
      t.excessMm = std::max(0.0, t.interferingMm - t.pressMm);
      t.pass = t.complete && t.excessMm <= rep.leadInMm + 1e-9 &&
               t.deeperMm <= rep.leadInMm + 1e-9;
      return t;
    };
    const auto consider = [&](const InsertionTry& t) {
      rep.tries.push_back(t);
      if (t.seats && t.complete &&
          (!haveBest || t.excessMm < rep.best.excessMm)) {
        rep.best = t;
        haveBest = true;
      }
      if (t.pass) {
        rep.pass = true;
        rep.best = t;
      }
    };
    // Turned by `deg`, is either side the same matter again?
    const auto symmetric = [&](double deg) {
      gp_Trsf rot = Pose(0.0, deg, true);
      for (const TopoDS_Shape* sh : {&s.moving, &s.fixed}) {
        const double v = VolumeOf(*sh);
        BRepAlgoAPI_Common c;
        if (!cad::BuildNonDestructive(c, *sh, sh->Moved(TopLoc_Location(rot)),
                                      kFuzz) ||
            c.Shape().IsNull())
          throw KernelError{"the symmetry boolean failed"};
        if (std::abs(VolumeOf(c.Shape()) - v) <= 1e-6 * std::max(1.0, v))
          return true;
      }
      return false;
    };
    // The seated orientation first, from each side; then the others.
    for (size_t i = 0; i < sides.size() && !rep.pass; i++)
      consider(run(i, 0.0));
    for (int k = 1; k < opt.rotations && !rep.pass; k++) {
      const double deg = 360.0 * k / opt.rotations;
      if (symmetric(deg)) {
        advance(perTry * sides.size());
        continue;
      }
      for (size_t i = 0; i < sides.size() && !rep.pass; i++)
        consider(run(i, deg));
    }
    if (fraction)
      fraction->store(1.0);
  } catch (const KernelError& e) {
    return std::unexpected(e.what);
  } catch (const Standard_Failure& e) {
    return std::unexpected(std::string("the kernel failed: ") +
                           (e.GetMessageString() ? e.GetMessageString() : ""));
  }
  return rep;
}

} // namespace Measure
