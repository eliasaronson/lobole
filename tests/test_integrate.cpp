// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

// cad::Integrate on the bodies where BRepGProp's fixed rule is in doubt,
// each against a true value derived here without it: a closed form, or a
// one-dimensional integral over the profile that a rule exact for its
// polynomial degree evaluates. Every fixture is placed off the origin and
// turned off the axes.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "SameCore.h"
#include "cad/Integrate.h"
#include "core/Document.h"
#include "core/features/ExtrudeFeature.h"
#include "core/features/SketchFeature.h"
#include "core/features/ThreadFeature.h"

#include <BRepAdaptor_Surface.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepBuilderAPI_Copy.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepGProp.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCone.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <BRepPrimAPI_MakeRevol.hxx>
#include <BRepPrimAPI_MakeSphere.hxx>
#include <BRepPrimAPI_MakeTorus.hxx>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <GC_MakeArcOfCircle.hxx>
#include <GProp_GProps.hxx>
#include <Geom2dAdaptor_Curve.hxx>
#include <Geom2dConvert.hxx>
#include <Geom2d_BSplineCurve.hxx>
#include <Geom2d_Curve.hxx>
#include <Geom2d_TrimmedCurve.hxx>
#include <Geom_BSplineCurve.hxx>
#include <Geom_BezierCurve.hxx>
#include <Geom_TrimmedCurve.hxx>
#include <TColStd_Array1OfInteger.hxx>
#include <TColStd_Array1OfReal.hxx>
#include <TColgp_Array1OfPnt.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_ListOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Wire.hxx>
#include <gp_Ax1.hxx>
#include <gp_Ax2.hxx>
#include <gp_Pln.hxx>
#include <gp_Trsf.hxx>
#include <math.hxx>
#include <math_Vector.hxx>

#include <array>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <functional>
#include <numbers>
#include <string>

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

constexpr double kPi = std::numbers::pi;

// Volume, first moments and the raw second moments (int r r^T) about the
// origin: what a true value is assembled from, part by part.
struct Moments {
  double v = 0.0;
  glm::dvec3 s{0.0};
  glm::dmat3 m{0.0};

  Moments operator+(const Moments& o) const {
    return {v + o.v, s + o.s, m + o.m};
  }
  Moments operator-(const Moments& o) const {
    return {v - o.v, s - o.s, m - o.m};
  }
  // The same solid moved by the rotation `r` then the translation `t`.
  Moments Moved(const glm::dmat3& r, const glm::dvec3& t) const {
    const glm::dvec3 rs = r * s;
    Moments out;
    out.v = v;
    out.s = rs + t * v;
    out.m = r * m * glm::transpose(r) + glm::outerProduct(rs, t) +
            glm::outerProduct(t, rs) + glm::outerProduct(t, t) * v;
    return out;
  }
  glm::dvec3 Centre() const { return s / v; }
  // The inertia tensor about the centre of mass (Ixy = -int x y).
  glm::dmat3 Inertia() const {
    const glm::dvec3 c = Centre();
    const glm::dmat3 mc = m - glm::outerProduct(c, c) * v;
    const double tr = mc[0][0] + mc[1][1] + mc[2][2];
    return glm::dmat3(tr) - mc;
  }
};

// Composite Gauss-Legendre over [a, b]: `pieces` spans of OCCT's 61-point
// table. Applied here only to integrands smooth on [a, b] (or polynomial),
// where it is exact to rounding.
double Quad(const std::function<double(double)>& f, double a, double b,
            int pieces = 16) {
  static const auto table = [] {
    math_Vector p(1, 61), w(1, 61);
    math::GaussPoints(61, p);
    math::GaussWeights(61, w);
    return std::pair{p, w};
  }();
  double sum = 0.0;
  for (int k = 0; k < pieces; k++) {
    const double x1 = a + (b - a) * k / pieces;
    const double x2 = a + (b - a) * (k + 1) / pieces;
    const double xm = 0.5 * (x1 + x2), xr = 0.5 * (x2 - x1);
    for (int i = 1; i <= 61; i++)
      sum += table.second(i) * xr * f(xm + xr * table.first(i));
  }
  return sum;
}

// The placement every fixture gets: a turn of 0.7 rad about (1, 2, 3) and a
// move to (12.3, -45.6, 7.8).
gp_Trsf Placement() {
  gp_Trsf rot, move;
  rot.SetRotation(gp_Ax1(gp_Pnt(0, 0, 0), gp_Dir(1, 2, 3)), 0.7);
  move.SetTranslation(gp_Vec(12.3, -45.6, 7.8));
  return move * rot;
}
TopoDS_Shape Placed(const TopoDS_Shape& s) {
  return s.Moved(TopLoc_Location(Placement()));
}
Moments Placed(const Moments& m) {
  const gp_Trsf t = Placement();
  glm::dmat3 r;
  for (int i = 0; i < 3; i++)
    for (int j = 0; j < 3; j++)
      r[j][i] = t.Value(i + 1, j + 1); // glm is column-major
  return m.Moved(r, glm::dvec3(t.Value(1, 4), t.Value(2, 4), t.Value(3, 4)));
}

double CpuSeconds() {
  timespec ts{};
  clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
  return double(ts.tv_sec) + 1e-9 * double(ts.tv_nsec);
}

// Today's mass properties of a shape: the fixed rule twice (about the
// origin, then about the centre) and the area once, as
// CADShape::ComputeMassProps does.
struct Today {
  double volume = 0.0, area = 0.0;
  glm::dvec3 centre{0.0};
  glm::dmat3 inertia{0.0};
  double seconds = 0.0;
};
Today TodaysRule(const TopoDS_Shape& s) {
  const double t0 = CpuSeconds();
  Today out;
  GProp_GProps v;
  BRepGProp::VolumeProperties(s, v);
  const gp_Pnt c = v.CentreOfMass();
  GProp_GProps about(c);
  BRepGProp::VolumeProperties(s, about);
  const gp_Mat in = about.MatrixOfInertia();
  GProp_GProps a;
  BRepGProp::SurfaceProperties(s, a);
  out.seconds = CpuSeconds() - t0;
  out.volume = v.Mass();
  out.area = a.Mass();
  out.centre = glm::dvec3(c.X(), c.Y(), c.Z());
  for (int i = 0; i < 3; i++)
    for (int j = 0; j < 3; j++)
      out.inertia[j][i] = in.Value(i + 1, j + 1);
  return out;
}

struct Run {
  cad::Integral result;
  double seconds = 0.0;
};
Run Measure(const TopoDS_Shape& s, cad::IntegrateOptions o = {}) {
  o.keepFaces = true;
  const double t0 = CpuSeconds();
  Run r;
  r.result = cad::Integrate(s, o);
  r.seconds = CpuSeconds() - t0;
  return r;
}

double MaxAbs(const glm::dmat3& m) {
  double x = 0.0;
  for (int i = 0; i < 3; i++)
    for (int j = 0; j < 3; j++)
      x = std::max(x, std::abs(m[j][i]));
  return x;
}
glm::dmat3 Tensor(const double in[3][3]) {
  glm::dmat3 m;
  for (int i = 0; i < 3; i++)
    for (int j = 0; j < 3; j++)
      m[j][i] = in[i][j];
  return m;
}

// Over the suspect faces: the largest gap between the fixed rule and the
// second estimate as a fraction of the body's volume scale, the most
// halvings one took, and their CPU time.
struct Spread {
  double gap = 0.0;
  int maxHalvings = 0;
  double secondSeconds = 0.0;
};
Spread SpreadOf(const cad::Integral& r) {
  Spread s;
  for (const cad::FaceReport& f : r.faces) {
    if (!f.suspect)
      continue;
    s.gap = std::max(s.gap, std::abs(f.fixed.volume - f.second.volume) /
                                r.scale.volume);
    s.maxHalvings = std::max(s.maxHalvings, f.halvings);
    s.secondSeconds += f.seconds;
  }
  return s;
}

// One line of the record the plan's thresholds are set from.
void Report(const char* name, double truth, const Today& today, const Run& run,
            const char* unit = "mm3") {
  const cad::Integral& r = run.result;
  double second = 0.0;
  for (const cad::FaceReport& f : r.faces)
    second += f.suspect ? f.second.volume : f.fixed.volume;
  const Spread s = SpreadOf(r);
  std::printf("[integrate] %-34s true %.9f %s | today %.9f (%+.2e) %.4f s | "
              "integrator %.9f (%+.2e) +- %.2e %.4f s | second %.9f | "
              "suspect %d/%zu, halvings <= %d, max gap %.2e of scale, "
              "second estimates %.4f s\n",
              name, truth, unit, today.volume, today.volume - truth,
              today.seconds, r.volume, r.volume - truth, r.volumeUncertainty,
              run.seconds, second, r.suspectFaces, r.faces.size(),
              s.maxHalvings, s.gap, s.secondSeconds);
  for (const cad::FaceReport& f : r.faces) {
    if (!f.suspect)
      continue;
    std::printf("[integrate]   face %3d %s%s fixed-second %.2e, changes",
                f.index, f.volumeReplaced ? "replaced" : "kept    ",
                f.converged ? "" : " UNCONVERGED",
                std::abs(f.fixed.volume - f.second.volume) / r.scale.volume);
    for (double c : f.volumeChanges)
      std::printf(" %.1e", c);
    std::printf("  (%.4f s)\n", f.seconds);
  }
}

// Every figure the integrator returns against a true one: the volume to
// `rel` of itself and within the stated uncertainty plus that much, the
// centre and inertia to `rel` of the body's size.
void CheckAgainst(const cad::Integral& r, const Moments& truth, double rel,
                  double areaTruth = -1.0) {
  const double v = truth.v;
  CHECK_THAT(r.volume, WithinRel(v, rel));
  CHECK(std::abs(r.volume - v) <= r.volumeUncertainty + rel * v);
  const glm::dvec3 c = truth.Centre();
  const double size = std::cbrt(v);
  for (int i = 0; i < 3; i++)
    CHECK_THAT(r.centerOfMass[i], WithinAbs(c[i], rel * size));
  const glm::dmat3 in = truth.Inertia();
  const double scale = MaxAbs(in);
  const glm::dmat3 got = Tensor(r.inertia);
  for (int i = 0; i < 3; i++)
    for (int j = 0; j < 3; j++)
      CHECK_THAT(got[j][i], WithinAbs(in[j][i], rel * scale));
  if (areaTruth > 0.0)
    CHECK_THAT(r.area, WithinRel(areaTruth, rel));
}

// The faces' figures sum to the solid's, and on a face that is not suspect
// the integrator kept the fixed rule's figure, bit for bit.
void CheckSums(const cad::Integral& r, const Today& today) {
  cad::FaceFigure sum, fixed;
  for (const cad::FaceReport& f : r.faces) {
    sum.volume += f.used.volume;
    sum.moment += f.used.moment;
    sum.area += f.used.area;
    fixed.volume += f.fixed.volume;
    fixed.area += f.fixed.area;
    if (!f.suspect) {
      CHECK(f.used.volume == f.fixed.volume);
      CHECK(f.used.area == f.fixed.area);
      CHECK(f.uncertainty.volume == 0.0);
    }
  }
  CHECK(sum.volume == r.sum.volume);
  CHECK(sum.area == r.sum.area);
  CHECK(sum.moment == r.sum.moment);
  CHECK(r.volume == r.sum.volume);
  // The fixed rule's faces, each about the box's centre, sum to what it
  // reads on the whole solid about its own point wherever it is exact.
  if (r.suspectFaces == 0) {
    CHECK_THAT(fixed.volume, WithinRel(today.volume, 1e-12));
    CHECK_THAT(fixed.area, WithinRel(today.area, 1e-12));
  }
}

// --- the fixtures and their true values -----------------------------------

// A solid cylinder of radius r from z0 to z1 about Z.
Moments Cylinder(double r, double z0, double z1) {
  Moments m;
  const double h = z1 - z0;
  m.v = kPi * r * r * h;
  m.s = {0, 0, m.v * 0.5 * (z0 + z1)};
  const double xx = kPi * std::pow(r, 4) / 4.0 * h;
  m.m[0][0] = xx;
  m.m[1][1] = xx;
  m.m[2][2] = kPi * r * r * (std::pow(z1, 3) - std::pow(z0, 3)) / 3.0;
  return m;
}

// The part of a round bar (radius 1, along +X at height zc, out to x = 37.2)
// that lies outside the cylinder x^2 + y^2 = 23^2: over the bar's section,
// at offset y = s and height zc + w, x runs from sqrt(529 - s^2) to 37.2.
// Integrated in a = asin(s), where the integrand is smooth.
Moments BarOutsideTube(double zc) {
  const double xe = 37.2, R2 = 23.0 * 23.0;
  const auto over = [&](auto&& g) {
    return Quad([&](double a) { return g(std::sin(a)) * std::cos(a); },
                -kPi / 2, kPi / 2);
  };
  // At offset s the section's chord is 2 c, c = sqrt(1 - s^2).
  const auto x0 = [&](double s) { return std::sqrt(R2 - s * s); };
  const auto c = [](double s) { return std::sqrt(std::max(0.0, 1 - s * s)); };
  Moments m;
  m.v = over([&](double s) { return 2 * c(s) * (xe - x0(s)); });
  const double sx =
      over([&](double s) { return 2 * c(s) * (xe * xe - x0(s) * x0(s)) / 2; });
  m.s = {sx, 0.0, zc * m.v};
  m.m[0][0] = over([&](double s) {
    return 2 * c(s) * (std::pow(xe, 3) - std::pow(x0(s), 3)) / 3;
  });
  m.m[1][1] = over([&](double s) { return 2 * c(s) * s * s * (xe - x0(s)); });
  const double ww = over(
      [&](double s) { return 2.0 / 3.0 * std::pow(c(s), 3) * (xe - x0(s)); });
  m.m[2][2] = ww + zc * zc * m.v;
  m.m[0][2] = m.m[2][0] = zc * sx;
  return m;
}

TopoDS_Shape Tube() {
  const TopoDS_Shape outer = BRepPrimAPI_MakeCylinder(23.0, 20.0).Shape();
  const TopoDS_Shape inner = BRepPrimAPI_MakeCylinder(21.5, 20.0).Shape();
  return BRepAlgoAPI_Cut(outer, inner).Shape();
}
TopoDS_Shape Bar(double turn) {
  gp_Ax2 ax(gp_Pnt(22.2, 0, 10), gp_Dir(1, 0, 0));
  TopoDS_Shape bar = BRepPrimAPI_MakeCylinder(ax, 1.0, 15.0).Shape();
  gp_Trsf t;
  t.SetRotation(gp_Ax1(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1)), turn);
  return bar.Moved(TopLoc_Location(t));
}
glm::dmat3 TurnZ(double a) {
  glm::dmat3 r(1.0);
  r[0][0] = std::cos(a);
  r[0][1] = std::sin(a);
  r[1][0] = -std::sin(a);
  r[1][1] = std::cos(a);
  return r;
}

// A cylinder of radius r, cut by the plane through (0, 0, h) whose normal
// leans `lean` from Z toward X; below the plane is kept.
Moments ObliqueCylinder(double r, double h, double lean) {
  const double t = std::tan(lean), A = kPi * r * r,
               X2 = kPi * std::pow(r, 4) / 4;
  Moments m;
  m.v = A * h;
  m.s = {-t * X2, 0.0, (h * h * A + t * t * X2) / 2.0};
  m.m[0][0] = h * X2;
  m.m[1][1] = h * X2;
  m.m[2][2] = (std::pow(h, 3) * A + 3 * h * t * t * X2) / 3.0;
  m.m[0][2] = m.m[2][0] = -h * t * X2;
  return m;
}
TopoDS_Shape ObliqueCylinderShape(double r, double h, double lean) {
  const TopoDS_Shape cyl = BRepPrimAPI_MakeCylinder(r, 2 * h).Shape();
  // A box standing on the plane, turned with it.
  TopoDS_Shape box =
      BRepPrimAPI_MakeBox(gp_Pnt(-4 * r, -4 * r, 0), 8 * r, 8 * r, 4 * h)
          .Shape();
  gp_Trsf turn, lift;
  turn.SetRotation(gp_Ax1(gp_Pnt(0, 0, 0), gp_Dir(0, 1, 0)), lean);
  lift.SetTranslation(gp_Vec(0, 0, h));
  box = box.Moved(TopLoc_Location(lift * turn));
  return BRepAlgoAPI_Cut(cyl, box).Shape();
}

// The profile (15,30)-(0,30)-(0,0)-(20,0) closed by the cubic Bezier
// (20,0),(30,10),(10,20),(15,30), in the XZ plane, revolved a full turn
// about Z.
const double kBezR[4] = {20, 30, 10, 15};
double BezierR(double t) {
  const double u = 1 - t;
  return u * u * u * kBezR[0] + 3 * u * u * t * kBezR[1] +
         3 * u * t * t * kBezR[2] + t * t * t * kBezR[3];
}
double BezierDR(double t) {
  const double u = 1 - t;
  return 3 *
         (u * u * (kBezR[1] - kBezR[0]) + 2 * u * t * (kBezR[2] - kBezR[1]) +
          t * t * (kBezR[3] - kBezR[2]));
}
TopoDS_Shape BezierRevolve() {
  TColgp_Array1OfPnt poles(1, 4);
  for (int i = 0; i < 4; i++)
    poles(i + 1) = gp_Pnt(kBezR[i], 0, 10.0 * i);
  const Handle(Geom_BezierCurve) bez = new Geom_BezierCurve(poles);
  BRepBuilderAPI_MakeWire w;
  w.Add(BRepBuilderAPI_MakeEdge(gp_Pnt(15, 0, 30), gp_Pnt(0, 0, 30)).Edge());
  w.Add(BRepBuilderAPI_MakeEdge(gp_Pnt(0, 0, 30), gp_Pnt(0, 0, 0)).Edge());
  w.Add(BRepBuilderAPI_MakeEdge(gp_Pnt(0, 0, 0), gp_Pnt(20, 0, 0)).Edge());
  w.Add(BRepBuilderAPI_MakeEdge(bez).Edge());
  const TopoDS_Face f = BRepBuilderAPI_MakeFace(w.Wire(), true).Face();
  return BRepPrimAPI_MakeRevol(f, gp_Ax1(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1)))
      .Shape();
}
// z = 30 t is linear, so every moment is one integral in t of a polynomial.
Moments BezierRevolveTruth() {
  Moments m;
  const auto dz = 30.0;
  const auto r = BezierR;
  m.v = Quad([&](double t) { return kPi * std::pow(r(t), 2) * dz; }, 0, 1, 1);
  m.s.z = Quad([&](double t) { return kPi * std::pow(r(t), 2) * 30 * t * dz; },
               0, 1, 1);
  const double xx =
      Quad([&](double t) { return kPi * std::pow(r(t), 4) / 4 * dz; }, 0, 1, 1);
  m.m[0][0] = xx;
  m.m[1][1] = xx;
  m.m[2][2] = Quad(
      [&](double t) {
        return kPi * std::pow(r(t), 2) * std::pow(30 * t, 2) * dz;
      },
      0, 1, 1);
  return m;
}
double BezierRevolveArea() {
  // The two disks and the side, 2 pi r ds along the Bezier.
  return kPi * 20 * 20 + kPi * 15 * 15 +
         Quad(
             [&](double t) {
               return 2 * kPi * BezierR(t) * std::hypot(BezierDR(t), 30.0);
             },
             0, 1);
}

// The half disc of radius 14 centred 14 from the axis, on its far side,
// revolved 270 degrees about Z: a horn torus's outer half.
TopoDS_Shape HornTorus() {
  const Handle(Geom_TrimmedCurve) arc =
      GC_MakeArcOfCircle(gp_Pnt(14, 0, -14), gp_Pnt(28, 0, 0),
                         gp_Pnt(14, 0, 14))
          .Value();
  BRepBuilderAPI_MakeWire w;
  w.Add(BRepBuilderAPI_MakeEdge(arc).Edge());
  w.Add(BRepBuilderAPI_MakeEdge(gp_Pnt(14, 0, 14), gp_Pnt(14, 0, -14)).Edge());
  const TopoDS_Face f = BRepBuilderAPI_MakeFace(w.Wire(), true).Face();
  return BRepPrimAPI_MakeRevol(f, gp_Ax1(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1)),
                               1.5 * kPi)
      .Shape();
}
// In polar coordinates about the disc's centre (rho, phi), the profile point
// is r = 14 + rho cos phi, z = rho sin phi, phi in [-pi/2, pi/2]; revolved
// through psi in [0, 3 pi / 2], dV = r dpsi dA.
Moments HornTorusTruth() {
  const double th = 1.5 * kPi;
  const auto area = [&](auto&& g) {
    return Quad(
        [&](double phi) {
          return Quad(
              [&](double rho) {
                const double r = 14 + rho * std::cos(phi),
                             z = rho * std::sin(phi);
                return g(r, z) * rho;
              },
              0, 14, 1);
        },
        -kPi / 2, kPi / 2, 1);
  };
  const double r1 = area([](double r, double) { return r; });
  const double r2 = area([](double r, double) { return r * r; });
  const double r3 = area([](double r, double) { return r * r * r; });
  const double rz2 = area([](double r, double z) { return r * z * z; });
  Moments m;
  m.v = th * r1;
  m.s = {r2 * std::sin(th), r2 * (1 - std::cos(th)), 0.0};
  // int cos^2 psi, sin^2 psi, sin cos over [0, th].
  const double cc = th / 2 + std::sin(2 * th) / 4;
  const double ss = th / 2 - std::sin(2 * th) / 4;
  const double sc = std::pow(std::sin(th), 2) / 2;
  m.m[0][0] = r3 * cc;
  m.m[1][1] = r3 * ss;
  m.m[0][1] = m.m[1][0] = r3 * sc;
  m.m[2][2] = th * rz2;
  return m;
}

// A Menger sponge of side L, level n: the cube less its tunnels, cut in one
// Boolean.
TopoDS_Shape SpongeCut(double L, int n) {
  const TopoDS_Shape cube = BRepPrimAPI_MakeBox(L, L, L).Shape();
  TopTools_ListOfShape tools;
  for (int level = 1; level <= n; level++) {
    const int cells = int(std::pow(3, level - 1));
    const double cell = L / cells, hole = cell / 3;
    for (int i = 0; i < cells; i++)
      for (int j = 0; j < cells; j++) {
        const double a = i * cell + hole, b = j * cell + hole;
        tools.Append(BRepPrimAPI_MakeBox(gp_Pnt(-1, a, b), L + 2, hole, hole));
        tools.Append(BRepPrimAPI_MakeBox(gp_Pnt(a, -1, b), hole, L + 2, hole));
        tools.Append(BRepPrimAPI_MakeBox(gp_Pnt(a, b, -1), hole, hole, L + 2));
      }
  }
  TopTools_ListOfShape args;
  args.Append(cube);
  BRepAlgoAPI_Cut cut;
  cut.SetArguments(args);
  cut.SetTools(tools);
  cut.Build();
  return cut.Shape();
}
// The level-3 sponge of side 135 as the kernel case builds it, twenty
// level-2 sponges touching face to face, here gathered into a compound
// rather than joined: the integral over their faces is the same, and the
// join is a minute of Booleans this test is not about.
TopoDS_Shape Sponge() {
  const TopoDS_Shape sub = SpongeCut(45.0, 2);
  TopoDS_Compound all;
  BRep_Builder b;
  b.MakeCompound(all);
  for (int i = 0; i < 3; i++)
    for (int j = 0; j < 3; j++)
      for (int k = 0; k < 3; k++) {
        if ((i == 1) + (j == 1) + (k == 1) >= 2)
          continue;
        gp_Trsf t;
        t.SetTranslation(gp_Vec(45.0 * i, 45.0 * j, 45.0 * k));
        b.Add(all, sub.Moved(TopLoc_Location(t)));
      }
  return all;
}
// Self-similar: twenty cubes of a third the side, each a sponge one level
// down, at the positions of a 3x3x3 grid with at most one zero coordinate.
Moments SpongeTruth(double L, int level) {
  Moments m;
  if (level == 0) {
    m.v = L * L * L;
    m.s = glm::dvec3(m.v * L / 2);
    const double c = m.v * L * L / 3;  // int x^2 over [0, L]^3
    const double c2 = m.v * L * L / 4; // int x y
    for (int i = 0; i < 3; i++)
      for (int j = 0; j < 3; j++)
        m.m[j][i] = i == j ? c : c2;
    return m;
  }
  const Moments sub = SpongeTruth(L / 3, level - 1);
  for (int i = 0; i < 3; i++)
    for (int j = 0; j < 3; j++)
      for (int k = 0; k < 3; k++) {
        if ((i == 1) + (j == 1) + (k == 1) >= 2)
          continue;
        m = m + sub.Moved(glm::dmat3(1.0), glm::dvec3(i, j, k) * (L / 3));
      }
  return m;
}

// A closed planar B-spline with many knots, extruded: its side is a surface
// of extrusion whose u direction crosses every knot, bounded by lines.
Handle(Geom_BSplineCurve) KnottedLoop() {
  // A periodic cubic through 24 poles on a wavy ring.
  const int n = 24;
  TColgp_Array1OfPnt poles(1, n);
  for (int i = 0; i < n; i++) {
    const double a = 2 * kPi * i / n;
    const double r = 20 + 3 * std::cos(5 * a) + (i % 2 ? 1.5 : 0.0);
    poles(i + 1) = gp_Pnt(r * std::cos(a), r * std::sin(a), 0);
  }
  TColStd_Array1OfReal knots(1, n + 1);
  TColStd_Array1OfInteger mults(1, n + 1);
  for (int i = 0; i <= n; i++) {
    knots(i + 1) = i;
    mults(i + 1) = 1;
  }
  return new Geom_BSplineCurve(poles, knots, mults, 3, true);
}
TopoDS_Shape KnottedPrism(double h) {
  const TopoDS_Edge e = BRepBuilderAPI_MakeEdge(KnottedLoop()).Edge();
  const TopoDS_Wire w = BRepBuilderAPI_MakeWire(e).Wire();
  const TopoDS_Face f = BRepBuilderAPI_MakeFace(w, true).Face();
  return BRepPrimAPI_MakePrism(f, gp_Vec(0, 0, h)).Shape();
}
// The section's area and moments by Green's theorem on the curve itself,
// span by span (a polynomial on each), then times the height.
Moments KnottedPrismTruth(double h) {
  const Handle(Geom_BSplineCurve) c = KnottedLoop();
  double A = 0, Sx = 0, Sy = 0, Ixx = 0, Iyy = 0, Ixy = 0;
  for (int k = 1; k < c->NbKnots(); k++) {
    const auto at = [&](double t, auto&& g) {
      gp_Pnt p;
      gp_Vec d;
      c->D1(t, p, d);
      return g(p.X(), p.Y(), d.X(), d.Y());
    };
    const double a = c->Knot(k), b = c->Knot(k + 1);
    A += Quad(
        [&](double t) {
          return at(t,
                    [](double x, double, double, double dy) { return x * dy; });
        },
        a, b, 1);
    Sx += Quad(
        [&](double t) {
          return at(t, [](double x, double, double, double dy) {
            return x * x / 2 * dy;
          });
        },
        a, b, 1);
    Sy += Quad(
        [&](double t) {
          return at(t, [](double, double y, double dx, double) {
            return -y * y / 2 * dx;
          });
        },
        a, b, 1);
    Ixx += Quad(
        [&](double t) {
          return at(t, [](double x, double, double, double dy) {
            return x * x * x / 3 * dy;
          });
        },
        a, b, 1);
    Iyy += Quad(
        [&](double t) {
          return at(t, [](double, double y, double dx, double) {
            return -y * y * y / 3 * dx;
          });
        },
        a, b, 1);
    Ixy += Quad(
        [&](double t) {
          return at(t, [](double x, double y, double, double dy) {
            return x * x / 2 * y * dy;
          });
        },
        a, b, 1);
  }
  Moments m;
  m.v = A * h;
  m.s = {Sx * h, Sy * h, A * h * h / 2};
  m.m[0][0] = Ixx * h;
  m.m[1][1] = Iyy * h;
  m.m[2][2] = A * h * h * h / 3;
  m.m[0][1] = m.m[1][0] = Ixy * h;
  m.m[0][2] = m.m[2][0] = Sx * h * h / 2;
  m.m[1][2] = m.m[2][1] = Sy * h * h / 2;
  return m;
}

} // namespace

TEST_CASE("the integrator is right where the fixed rule is wrong: a bar "
          "through a tube's wall",
          "[integrate]") {
  const TopoDS_Shape tube = Tube();
  const TopoDS_Shape one = Placed(BRepAlgoAPI_Fuse(tube, Bar(0.0)).Shape());
  const Moments truth = Placed(Cylinder(23, 0, 20) - Cylinder(21.5, 0, 20) +
                               BarOutsideTube(10.0));
  const Today today = TodaysRule(one);
  const Run run = Measure(one);
  Report("tube + one bar", truth.v, today, run);
  // Today's rule reads the bar 0.019 mm^3 light.
  CHECK(std::abs(today.volume - truth.v) > 1e-2);
  CheckSums(run.result, today);
  CHECK(run.result.suspectFaces > 0);
  CHECK(run.result.unconverged.empty());
  CheckAgainst(run.result, truth, 1e-9);
  // The same shape, the same bits.
  const cad::Integral again = cad::Integrate(one);
  CHECK(again.volume == run.result.volume);
  CHECK(again.centerOfMass == run.result.centerOfMass);
  CHECK(again.inertia[0][1] == run.result.inertia[0][1]);
  // With no halving allowed, a trimmed face has not converged and says so.
  cad::IntegrateOptions none;
  none.maxHalvings = 0;
  const cad::Integral unsure = cad::Integrate(one, none);
  CHECK(!unsure.unconverged.empty());
  CHECK(unsure.volumeUncertainty > 0.0);
  CHECK(unsure.areaUncertainty > 0.0);
}

TEST_CASE("the integrator measures the tube and nine bars to the closed form",
          "[integrate]") {
  TopoDS_Shape body = Tube();
  Moments truth = Cylinder(23, 0, 20) - Cylinder(21.5, 0, 20);
  for (int k = 0; k < 9; k++) {
    const double turn = 2 * kPi * k / 9;
    body = BRepAlgoAPI_Fuse(body, Bar(turn)).Shape();
    truth = truth + BarOutsideTube(10.0).Moved(TurnZ(turn), glm::dvec3(0.0));
  }
  CHECK_THAT(truth.v, WithinAbs(4595.6754, 1e-4));
  body = Placed(body);
  truth = Placed(truth);
  const Today today = TodaysRule(body);
  const Run run = Measure(body);
  Report("tube + nine bars", truth.v, today, run);
  CHECK(std::abs(today.volume - truth.v) > 0.1);
  CheckSums(run.result, today);
  CHECK(run.result.unconverged.empty());
  CheckAgainst(run.result, truth, 1e-9);
}

TEST_CASE("a cylinder cut on a slant: the mechanism", "[integrate]") {
  const double r = 10, h = 20, lean = 0.5;
  const TopoDS_Shape conic = ObliqueCylinderShape(r, h, lean);
  // The same body with the planar face's ellipse p-curve swapped for the
  // same ellipse as a rational B-spline.
  TopoDS_Shape swapped = BRepBuilderAPI_Copy(conic).Shape();
  int swaps = 0;
  for (TopExp_Explorer f(swapped, TopAbs_FACE); f.More(); f.Next()) {
    const TopoDS_Face face = TopoDS::Face(f.Current());
    if (BRepAdaptor_Surface(face).GetType() != GeomAbs_Plane)
      continue;
    for (TopExp_Explorer e(face, TopAbs_EDGE); e.More(); e.Next()) {
      const TopoDS_Edge edge = TopoDS::Edge(e.Current());
      double a, b;
      const Handle(Geom2d_Curve) pc =
          BRep_Tool::CurveOnSurface(edge, face, a, b);
      if (pc.IsNull() || Geom2dAdaptor_Curve(pc).GetType() != GeomAbs_Ellipse)
        continue;
      const Handle(Geom2d_BSplineCurve) bs =
          Geom2dConvert::CurveToBSplineCurve(new Geom2d_TrimmedCurve(pc, a, b));
      BRep_Builder().UpdateEdge(edge, bs, face, BRep_Tool::Tolerance(edge));
      BRep_Builder().Range(edge, face, bs->FirstParameter(),
                           bs->LastParameter());
      swaps++;
    }
  }
  REQUIRE(swaps > 0);
  const Moments truth = Placed(ObliqueCylinder(r, h, lean));
  const Today todayConic = TodaysRule(Placed(conic));
  const Today todaySwapped = TodaysRule(Placed(swapped));
  const Run conicRun = Measure(Placed(conic));
  const Run swappedRun = Measure(Placed(swapped));
  Report("slanted cylinder", truth.v, todayConic, conicRun);
  Report("slanted cylinder, B-spline ellipse", truth.v, todaySwapped,
         swappedRun);
  CheckSums(swappedRun.result, todaySwapped);
  CheckAgainst(conicRun.result, truth, 1e-9);
  CheckAgainst(swappedRun.result, truth, 1e-9);
  CHECK_THAT(swappedRun.result.volume, WithinRel(conicRun.result.volume, 1e-9));
}

TEST_CASE("a revolved Bezier profile and a horn torus keep their figures",
          "[integrate]") {
  {
    const TopoDS_Shape s = Placed(BezierRevolve());
    const Moments truth = Placed(BezierRevolveTruth());
    CHECK_THAT(truth.v, WithinAbs(34131.160186501, 1e-6));
    const Today today = TodaysRule(s);
    const Run run = Measure(s);
    Report("revolved Bezier profile", truth.v, today, run);
    CheckSums(run.result, today);
    CheckAgainst(run.result, truth, 1e-9, BezierRevolveArea());
  }
  {
    const TopoDS_Shape s = Placed(HornTorus());
    const Moments truth = Placed(HornTorusTruth());
    CHECK_THAT(truth.v, WithinAbs(28932.176098892, 1e-6));
    const Today today = TodaysRule(s);
    const Run run = Measure(s);
    Report("horn torus, 270 deg", truth.v, today, run);
    CheckSums(run.result, today);
    CheckAgainst(run.result, truth, 1e-9);
  }
}

TEST_CASE("an extruded spline loop", "[integrate]") {
  const double h = 12.0;
  const TopoDS_Shape s = Placed(KnottedPrism(h));
  const Moments truth = Placed(KnottedPrismTruth(h));
  const Today today = TodaysRule(s);
  const Run run = Measure(s);
  Report("extruded 24-span spline", truth.v, today, run);
  for (const cad::FaceReport& f : run.result.faces)
    std::printf("[integrate]   face %d suspect %d fixed %.9f second %.9f "
                "area %.6f / %.6f halvings %d\n",
                f.index, int(f.suspect), f.fixed.volume, f.second.volume,
                f.fixed.area, f.second.area, f.halvings);
  std::printf("[integrate]   today's centre off by %.2e, integrator's %.2e\n",
              glm::length(today.centre - truth.Centre()),
              glm::length(run.result.centerOfMass - truth.Centre()));
  CheckSums(run.result, today);
  CheckAgainst(run.result, truth, 1e-9);
}

TEST_CASE("a modelled M10 thread to a shoulder", "[integrate][slow]") {
  Document doc;
  auto collar =
      std::make_unique<SketchFeature>("Collar", SketchPlane(PlaneType::XY));
  collar->AddEntity(sketch::Circle{{0, 0}, 8.0});
  doc.AddFeature(std::move(collar));
  doc.AddFeature(std::make_unique<ExtrudeFeature>(
      "CollarPad", doc.GetFeature(0)->GetId(), 10.0, ExtrudeMode::NewBody));
  auto shaft = std::make_unique<SketchFeature>(
      "Shaft",
      SketchPlane(glm::vec3(0, 0, 10), glm::vec3(0, 0, 1), glm::vec3(1, 0, 0)));
  shaft->AddEntity(sketch::Circle{{0, 0}, 5.0});
  doc.AddFeature(std::move(shaft));
  doc.AddFeature(std::make_unique<ExtrudeFeature>(
      "ShaftPad", doc.GetFeature(2)->GetId(), 25.0, ExtrudeMode::Add));
  REQUIRE(doc.GetBodies().size() == 1);
  const Body& body = doc.GetBodies()[0];
  std::string face;
  int i = 0;
  for (TopExp_Explorer ex(body.shape.GetShape(), TopAbs_FACE); ex.More();
       ex.Next(), i++) {
    BRepAdaptor_Surface s(TopoDS::Face(ex.Current()));
    if (s.GetType() == GeomAbs_Cylinder &&
        std::abs(s.Cylinder().Radius() - 5.0) < 1e-9)
      face = body.elementMap.names.at(i);
  }
  REQUIRE(!face.empty());
  auto th = std::make_unique<ThreadFeature>(
      "Thread", ElementRef{body.createdBy, face}, "M10", 1.5);
  th->modeled = true;
  doc.AddFeature(std::move(th));
  INFO(doc.GetFeature(4)->GetError());
  REQUIRE(doc.GetFeature(4)->GetError().empty());
  const TopoDS_Shape s = Placed(doc.GetBodies()[0].shape.GetShape());
  // tests/kernel_cases/solidcraft/ladder/06-threads/004-external-m10-to-shoulder
  const double truth = 3639.157562673;
  const SameCore held;
  const Today today = TodaysRule(s);
  const Run run = Measure(s);
  GProp_GProps adaptive;
  BRepGProp::VolumeProperties(s, adaptive, 1e-5);
  Report("M10 thread to a shoulder", truth, today, run);
  std::printf("[integrate]   the adaptive rule (1e-5) %.9f (%+.2e)\n",
              adaptive.Mass(), adaptive.Mass() - truth);
  CheckSums(run.result, today);
  CHECK(run.result.unconverged.empty());
  // The modelled groove is SMLib's screw sweep, 6.7e-8 of the volume over
  // the analytic one; the integrator and the adaptive rule agree on it.
  CHECK_THAT(run.result.volume, WithinRel(truth, 1e-7));
  CHECK_THAT(run.result.volume, WithinRel(adaptive.Mass(), 1e-10));
  // 57 of 75 faces suspect: one fixed pass, one area pass and the second
  // estimates cost 1.3 times today's three passes.
  CHECK(run.seconds < 2.0 * today.seconds);
}

TEST_CASE("a level-3 Menger sponge costs no more than today's rule",
          "[integrate][slow]") {
  const TopoDS_Shape s = Placed(Sponge());
  const Moments truth = Placed(SpongeTruth(135.0, 3));
  CHECK_THAT(truth.v, WithinRel(1e6, 1e-12));
  const SameCore held;
  const Today today = TodaysRule(s);
  const Run run = Measure(s);
  Report("menger sponge level 3", truth.v, today, run);
  CHECK(run.result.suspectFaces == 0);
  // Nothing suspect: one volume pass and one area pass where today takes
  // two and one (0.67 times its CPU time measured).
  CHECK(run.seconds < today.seconds);
  CheckSums(run.result, today);
  CheckAgainst(run.result, truth, 1e-9);
}

TEST_CASE("where the fixed rule is exact the two estimates agree",
          "[integrate]") {
  // Every face of these is bounded by lines and conics on an elementary
  // surface. Given a second estimate as if each were suspect, the gap
  // between the two is the floor the agreement threshold sits above.
  struct Case {
    const char* name;
    TopoDS_Shape shape;
    double volume;
  };
  const double r = 7.0, h = 11.0;
  const Case cases[] = {
      {"box", BRepPrimAPI_MakeBox(gp_Pnt(1, 2, 3), 10, 20, 30).Shape(), 6000},
      {"cylinder", BRepPrimAPI_MakeCylinder(r, h).Shape(), kPi * r * r * h},
      {"cone", BRepPrimAPI_MakeCone(r, 2.0, h).Shape(),
       kPi * h / 3 * (r * r + 2 * r + 4)},
      {"sphere", BRepPrimAPI_MakeSphere(r).Shape(), 4.0 / 3 * kPi * r * r * r},
      {"torus", BRepPrimAPI_MakeTorus(20.0, r).Shape(),
       2 * kPi * kPi * 20 * r * r},
      {"half torus", BRepPrimAPI_MakeTorus(20.0, r, kPi).Shape(),
       kPi * kPi * 20 * r * r},
      {"horn torus", HornTorus(), HornTorusTruth().v},
  };
  double worst = 0.0;
  for (const Case& c : cases) {
    cad::IntegrateOptions o;
    o.everyFaceSuspect = true;
    const Run run = Measure(Placed(c.shape), o);
    const cad::Integral& res = run.result;
    double gap = 0.0, areaGap = 0.0;
    for (const cad::FaceReport& f : res.faces) {
      gap = std::max(gap, std::abs(f.fixed.volume - f.second.volume) /
                              res.scale.volume);
      areaGap = std::max(areaGap, std::abs(f.fixed.area - f.second.area) /
                                      res.scale.area);
      for (int i = 0; i < 3; i++)
        gap = std::max(gap, std::abs(f.fixed.moment[i] - f.second.moment[i]) /
                                res.scale.moment.x);
      for (int i = 0; i < 6; i++)
        gap = std::max(gap, std::abs(f.fixed.inertia[i] - f.second.inertia[i]) /
                                res.scale.inertia[0]);
      CHECK(f.converged);
    }
    std::printf("[integrate] exact %-12s volume %+.2e of itself, largest gap "
                "%.2e (volume, moments, inertia) %.2e (area) of the scale\n",
                c.name, (res.volume - c.volume) / c.volume, gap, areaGap);
    CHECK_THAT(res.volume, WithinRel(c.volume, 1e-12));
    worst = std::max({worst, gap, areaGap});
  }
  CHECK(worst < cad::IntegrateOptions{}.agreement);
}
