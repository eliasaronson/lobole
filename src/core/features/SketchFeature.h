#pragma once

#include "cad/SketchPlane.h"
#include "core/Feature.h"

#include <array>
#include <cmath>
#include <glm/glm.hpp>
#include <map>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace sketch {

// Sketch entities live in the plane's UV space. Each carries a stable `id`
// (1-based; 0 means unassigned) so constraints survive insert/delete — see
// PointRef below.
// `construction`: reference geometry (centerlines, ref circles). It drives
// constraints and snapping but is excluded from the extrude profile and drawn
// dashed/dimmed.
struct Point {
  glm::dvec2 p;
  uint32_t id = 0;
  bool construction = false;
};
struct Line {
  glm::dvec2 a, b;
  uint32_t id = 0;
  bool construction = false;
};
struct Circle {
  glm::dvec2 center;
  double radius;
  uint32_t id = 0;
  bool construction = false;
};
// An arc runs **counter-clockwise** from `startAngle` to `endAngle` (radians,
// measured from the +U axis) — the model is CCW-always; OCC's CW form is
// produced only at shape-build time. `endAngle` is normalized so the CCW sweep
// `endAngle - startAngle` lies in (0, 2π).
struct Arc {
  glm::dvec2 center;
  double radius;
  double startAngle;
  double endAngle;
  uint32_t id = 0;
  bool construction = false;
};
// A full ellipse: major axis is `rotation` radians from the +U axis, and
// rMajor >= rMinor (a file saying otherwise is refused on load). `which` 0
// addresses the centre, 3/4 the major-axis ends (+û, −û) and 5/6 the
// minor-axis ends (+v̂, −v̂) — solver points tied to the ellipse, so a radius
// is a distance from the centre to an axis end and dragging an end resizes
// and turns it (docs/sketching/README.md#ellipses-and-splines).
struct Ellipse {
  glm::dvec2 center;
  double rMajor;
  double rMinor;
  double rotation;
  uint32_t id = 0;
  bool construction = false;
};
// An elliptical arc: the ellipse (center, rMajor, rMinor, rotation) swept CCW
// from `startParam` to `endParam` (the ellipse parameter t of the point
// C + rMajor·cos t·û + rMinor·sin t·v̂, radians). Like Arc it is CCW-always —
// `endParam` is normalized so the sweep (endParam − startParam) lies in
// (0, 2π]. `which` 0 = centre, 1 = start point, 2 = end point, 3..6 its
// ellipse's axis ends as for Ellipse. This is to Ellipse what Arc is to
// Circle.
struct EllipseArc {
  glm::dvec2 center;
  double rMajor;
  double rMinor;
  double rotation;
  double startParam;
  double endParam;
  uint32_t id = 0;
  bool construction = false;
};
// A non-rational, clamped, uniform B-spline through its control `poles`
// (the fitted-spline entity a parametric import carries; sketch phase L).
// `which` addresses pole k, so each pole is a movable solver point
// (Coincident/Distance/Fixed on a pole work); the solver also knows the
// curve, so a point can lie on it and a curve can be tangent to its ends.
// The curve interpolates its first and last poles (clamped), so those poles
// ARE its ends; a spline whose first and last poles are equal is closed.
// `degree` is cubic, reduced to poles-1 when there are too few poles. Weights
// and an explicit knot vector are not stored in v1 — they are the
// clamped-uniform defaults (see BSplineKnots), keeping serialization small and
// every pole addressable by `PointRef::which` (uint8_t; poles capped at 255).
struct BSpline {
  std::vector<glm::dvec2> poles;
  int degree = 3;
  uint32_t id = 0;
  bool construction = false;
};

// A one-line text label (v137, docs/sketching/README.md#text): one entity,
// rigid glyph geometry, one free point — the `anchor` (`which` 0). Its glyphs
// are NEVER stored: they are regenerated from these fields by
// sketch::BuildTextGeometry (core/sketch/SketchText.h). `which` 1..4 are the
// corners of the label's box (bottom-left, bottom-right, top-right, top-left,
// from the FONT's ascender/descender and the label's advance), which the
// solver reads but does not move: they ride the anchor, so dimensioning one
// places the label.
enum class TextHAlign : uint8_t { Left, Center, Right };
enum class TextVAlign : uint8_t { Baseline, Bottom, Middle, Top };
struct Text {
  glm::dvec2 anchor{0.0};
  std::string text;          // UTF-8, one line
  std::string font = "sans"; // a bundled font by name (sketch::TextFontNames)
  double height = 0.0;       // CAP height, sketch units (mm)
  double angle = 0.0;        // radians CCW from +U (the baseline direction)
  TextHAlign halign = TextHAlign::Left;
  TextVAlign valign = TextVAlign::Baseline;
  double spacing = 0.0; // extra tracking between characters, mm
  uint32_t id = 0;
  bool construction = false;
};

// UV of a text label's anchor (which 0) or box corner (which 1..4). Defined in
// core/sketch/SketchText.cpp: the box needs the font's metrics. A label whose
// layout fails (unknown font, missing glyph) answers its anchor.
glm::dvec2 TextPointUV(const Text& text, uint8_t which);

// Rectangles are not a primitive: they decompose into four Lines with shared
// corners (see SketchFeature::AddRectangle) so every side is editable and
// trimmable. Legacy files with a rigid "rect" entity are migrated on load.
using Entity =
    std::variant<Point, Line, Circle, Arc, Ellipse, EllipseArc, BSpline, Text>;

// A reference image ("underlay", v49): a raster (PNG/JPEG/BMP) placed on the
// sketch plane to trace geometry over. It is deliberately NOT a sketch Entity —
// it carries no curve, contributes nothing to the profile, the constraint
// solver, or the regen geometry, and never extrudes. Placement is a rigid 2D
// affine in plane-UV space: image pixel (px, py) maps to
//   origin + R(rotation) * (vec2(px, py) * unitsPerPixel).
// The raw file bytes are NOT stored here — they live on the owning Document's
// asset store keyed by `asset` (Document::GetAsset), so they survive undo/redo
// and the renderer can decode each asset into a texture once. Persisted out-of-
// band — an assets/ file in the directory format, a base64 blob in the single-
// file form — so the feature JSON stays small and diff-friendly.
// `widthPx`/`heightPx` are cached so sizing and calibration never need an image
// decoder (headless builds have none).
struct ReferenceImage {
  uint32_t id = 0;              // stable within the sketch (1-based; 0 = none)
  std::string asset;            // Document asset key for the pixel data
  glm::dvec2 origin{0.0, 0.0};  // plane-UV of the image's (0,0) pixel corner
  double rotation = 0.0;        // radians CCW about `origin`
  double unitsPerPixel = 1.0;   // plane units per image pixel (the scale)
  double opacity = 0.5;         // 0..1 blend over the sketch background
  int widthPx = 0;              // cached pixel dimensions
  int heightPx = 0;
  bool hidden = false;
  // v166: drawn whenever its sketch is — not only while that sketch is being
  // edited — and in quick stills, so a model can be checked against the plan
  // it was traced from (docs/sketching/README.md#reference-images-underlays).
  // `hidden` still wins.
  bool showAlways = false;
};

// Effective degree of a spline: cubic by default, reduced so a spline with few
// poles stays valid (degree <= poles-1). 0 for a degenerate (<2 poles) spline.
inline int BSplineDegree(const BSpline& s) {
  const int n = static_cast<int>(s.poles.size());
  if (n < 2)
    return 0;
  return std::min(s.degree > 0 ? s.degree : 3, n - 1);
}

// The clamped-uniform knot vector for `n` poles of degree `p` (length n+p+1):
// p+1 zeros, interior knots 1..(n-p-1), then p+1 copies of (n-p). The curve's
// parameter domain is [0, n-p]. This is exactly the knot vector OCC builds for
// a clamped BSpline with the same poles/degree, so the header evaluator below
// and the OCC profile edge agree.
inline std::vector<double> BSplineKnots(int n, int p) {
  std::vector<double> U;
  U.reserve(n + p + 1);
  for (int i = 0; i <= p; i++)
    U.push_back(0.0);
  const int interior = n - p - 1;
  for (int i = 1; i <= interior; i++)
    U.push_back(static_cast<double>(i));
  const double last = static_cast<double>(interior + 1);
  for (int i = 0; i <= p; i++)
    U.push_back(last);
  return U;
}

// Evaluate the clamped B-spline at global parameter u in [0, n-p] (de Boor).
inline glm::dvec2 BSplineEvalUV(const BSpline& s, double u) {
  const int n = static_cast<int>(s.poles.size());
  if (n == 0)
    return glm::dvec2(0.0);
  const int p = BSplineDegree(s);
  if (p == 0)
    return s.poles[0];
  const std::vector<double> U = BSplineKnots(n, p);
  const double umax = U[n]; // = n - p
  if (u <= 0.0)
    return s.poles.front();
  if (u >= umax)
    return s.poles.back();
  // Knot span k in [p, n-1] with U[k] <= u < U[k+1].
  int k = p;
  while (k < n - 1 && U[k + 1] <= u)
    k++;
  std::vector<glm::dvec2> d(p + 1);
  for (int j = 0; j <= p; j++)
    d[j] = s.poles[j + k - p];
  for (int r = 1; r <= p; r++) {
    for (int j = p; j >= r; j--) {
      const double denom = U[j + 1 + k - r] - U[j + k - p];
      const double a = denom > 1e-12 ? ((u - U[j + k - p]) / denom) : 0.0;
      d[j] = (1.0 - a) * d[j - 1] + a * d[j];
    }
  }
  return d[p];
}

// Samples the spline into a polyline (`segsPerSpan` segments per knot span).
inline std::vector<glm::dvec2> BSplineSampleUV(const BSpline& s,
                                               int segsPerSpan = 16) {
  std::vector<glm::dvec2> pts;
  const int n = static_cast<int>(s.poles.size());
  if (n == 0)
    return pts;
  const int p = BSplineDegree(s);
  if (p == 0) {
    pts = s.poles;
    return pts;
  }
  const double umax = static_cast<double>(n - p);
  const int total = std::max(2, segsPerSpan * (n - p));
  pts.reserve(total + 1);
  for (int i = 0; i <= total; i++)
    pts.push_back(BSplineEvalUV(s, umax * i / total));
  return pts;
}

// Builds an ellipse from three clicks: centre, a point on the major axis (sets
// rMajor + rotation), and a third point whose perpendicular distance to the
// major axis sets rMinor.
// A third click farther out than the second makes the SECOND axis the major
// one: the result is the same curve with the radii swapped and the rotation
// turned a quarter, because rMajor >= rMinor is what the model (and OCC's
// gp_Elips) holds.
inline Ellipse EllipseFromClicks(glm::dvec2 center, glm::dvec2 majorEnd,
                                 glm::dvec2 minorPt) {
  const glm::dvec2 maj = majorEnd - center;
  double rMajor = glm::length(maj);
  double rotation = std::atan2(maj.y, maj.x);
  // Perpendicular (minor-axis) direction, and the third point's distance to it.
  const glm::dvec2 minorDir(-std::sin(rotation), std::cos(rotation));
  double rMinor = std::abs(glm::dot(minorPt - center, minorDir));
  if (rMinor > rMajor) {
    std::swap(rMajor, rMinor);
    rotation += M_PI / 2.0;
  }
  return Ellipse{center, rMajor, rMinor, rotation};
}

// The `which` of an ellipse's (or elliptical arc's) axis ends.
inline constexpr uint8_t kEllipseMajorPos = 3; // C + rMajor·û
inline constexpr uint8_t kEllipseMajorNeg = 4; // C − rMajor·û
inline constexpr uint8_t kEllipseMinorPos = 5; // C + rMinor·v̂
inline constexpr uint8_t kEllipseMinorNeg = 6; // C − rMinor·v̂

// UV of an axis end (which 3..6) of the ellipse (center, radii, rotation);
// the centre for any other `which`.
inline glm::dvec2 EllipseAxisEndUV(glm::dvec2 center, double rMajor,
                                   double rMinor, double rotation,
                                   uint8_t which) {
  const glm::dvec2 u(std::cos(rotation), std::sin(rotation));
  const glm::dvec2 v(-u.y, u.x);
  switch (which) {
  case kEllipseMajorPos:
    return center + rMajor * u;
  case kEllipseMajorNeg:
    return center - rMajor * u;
  case kEllipseMinorPos:
    return center + rMinor * v;
  case kEllipseMinorNeg:
    return center - rMinor * v;
  default:
    return center;
  }
}

// UV of a point on the ellipse at parametric angle `t` (before/after rotation).
inline glm::dvec2 EllipsePointUV(const Ellipse& e, double t) {
  const double x = e.rMajor * std::cos(t), y = e.rMinor * std::sin(t);
  const double c = std::cos(e.rotation), s = std::sin(e.rotation);
  return e.center + glm::dvec2((c * x - s * y), (s * x + c * y));
}

// UV of a point on the elliptical arc at ellipse parameter `t`.
inline glm::dvec2 EllipseArcEvalUV(const EllipseArc& e, double t) {
  const double x = e.rMajor * std::cos(t), y = e.rMinor * std::sin(t);
  const double c = std::cos(e.rotation), s = std::sin(e.rotation);
  return e.center + glm::dvec2((c * x - s * y), (s * x + c * y));
}

// The ellipse parameter t of a point p on the ellipse (center/rMajor/rMinor/
// rotation) — the inverse of EllipseArcEvalUV. Used when a reflection has to
// re-derive the swept params of the mirrored arc.
inline double EllipseParamOfPoint(glm::dvec2 center, double rMajor,
                                  double rMinor, double rotation,
                                  glm::dvec2 p) {
  const double c = std::cos(rotation), s = std::sin(rotation);
  const glm::dvec2 d = p - center;
  const double lx = c * d.x + s * d.y;  // component along the major axis
  const double ly = -s * d.x + c * d.y; // along the minor axis
  return std::atan2(ly / rMinor, lx / rMajor);
}

// UV of one of an elliptical arc's referenced points: 0 = center, 1 = start,
// 2 = end (matching the CCW startParam/endParam). Mirrors ArcPointUV.
inline glm::dvec2 EllipseArcPointUV(const EllipseArc& e, uint8_t which) {
  if (which == 1)
    return EllipseArcEvalUV(e, e.startParam);
  if (which == 2)
    return EllipseArcEvalUV(e, e.endParam);
  return EllipseAxisEndUV(e.center, e.rMajor, e.rMinor, e.rotation, which);
}

// Samples the elliptical arc into a polyline along its CCW sweep. `segs` <= 0
// picks a count proportional to the swept angle (like Arc's tessellation).
inline std::vector<glm::dvec2> EllipseArcSampleUV(const EllipseArc& e,
                                                  int segs = 0) {
  double s = e.startParam, en = e.endParam;
  while (en <= s)
    en += 2.0 * M_PI; // CCW-always
  const double sweep = en - s;
  if (segs <= 0)
    segs =
        std::max(2, static_cast<int>(std::lround(32 * sweep / (2.0 * M_PI))));
  std::vector<glm::dvec2> pts;
  pts.reserve(segs + 1);
  for (int i = 0; i <= segs; i++)
    pts.push_back(EllipseArcEvalUV(e, s + sweep * i / segs));
  return pts;
}

// UV of one of an arc's referenced points: which 0 = center, 1 = start, 2 = end
// (start/end matching CCW startAngle/endAngle). Mirrors Circle's center = 0.
inline glm::dvec2 ArcPointUV(const Arc& arc, uint8_t which) {
  const double r = arc.radius;
  if (which == 1)
    return arc.center +
           r * glm::dvec2(std::cos(arc.startAngle), std::sin(arc.startAngle));
  if (which == 2)
    return arc.center +
           r * glm::dvec2(std::cos(arc.endAngle), std::sin(arc.endAngle));
  return arc.center;
}

// The stable id of an entity, regardless of which variant alternative it holds.
inline uint32_t EntityId(const Entity& e) {
  return std::visit([](const auto& g) { return g.id; }, e);
}
inline void SetEntityId(Entity& e, uint32_t id) {
  std::visit([id](auto& g) { g.id = id; }, e);
}
inline bool IsConstruction(const Entity& e) {
  return std::visit([](const auto& g) { return g.construction; }, e);
}
inline void SetConstruction(Entity& e, bool c) {
  std::visit([c](auto& g) { g.construction = c; }, e);
}

// Builds a CCW arc centred at `center`, starting at `start`, sweeping CCW to
// the ray toward `endDir`. Radius = |start - center|; the sweep is normalized
// into (0, 2π].
inline Arc ArcCenterStartEnd(glm::dvec2 center, glm::dvec2 start,
                             glm::dvec2 endDir) {
  const double r = glm::length(start - center);
  double a0 = std::atan2(start.y - center.y, start.x - center.x);
  double a1 = std::atan2(endDir.y - center.y, endDir.x - center.x);
  while (a1 <= a0)
    a1 += 2.0 * M_PI;
  return Arc{center, r, a0, a1};
}

// Builds the CCW arc through three points: from `p0` to `p1`, bulging through
// `through`. Returns nullopt if the points are collinear (no circumcircle).
inline std::optional<Arc> ArcFromThreePoints(glm::dvec2 p0, glm::dvec2 through,
                                             glm::dvec2 p1) {
  const double ax = p0.x, ay = p0.y, bx = through.x, by = through.y, cx = p1.x,
               cy = p1.y;
  const double d = 2.0 * (ax * (by - cy) + bx * (cy - ay) + cx * (ay - by));
  if (std::abs(d) < 1e-9)
    return std::nullopt;
  const double a2 = ax * ax + ay * ay, b2 = bx * bx + by * by,
               c2 = cx * cx + cy * cy;
  glm::dvec2 center(
      static_cast<double>((a2 * (by - cy) + b2 * (cy - ay) + c2 * (ay - by)) /
                          d),
      static_cast<double>((a2 * (cx - bx) + b2 * (ax - cx) + c2 * (bx - ax)) /
                          d));
  const double r = glm::length(p0 - center);
  auto ang = [&](glm::dvec2 p) {
    return std::atan2(p.y - center.y, p.x - center.x);
  };
  auto wrap = [](double x) {
    while (x < 0)
      x += 2.0 * M_PI;
    while (x >= 2.0 * M_PI)
      x -= 2.0 * M_PI;
    return x;
  };
  const double a0 = ang(p0), a1 = ang(p1), am = ang(through);
  const double sweepP0 = wrap(a1 - a0); // CCW p0 → p1
  if (wrap(am - a0) <= sweepP0)         // `through` lies on that CCW span
    return Arc{center, r, a0, a0 + sweepP0};
  return Arc{center, r, a1, a1 + (2.0 * M_PI - sweepP0)}; // the other way
}

// Reflects a point across the infinite line through a and b.
inline glm::dvec2 ReflectPoint(glm::dvec2 p, glm::dvec2 a, glm::dvec2 b) {
  glm::dvec2 d = b - a;
  double len2 = glm::dot(d, d);
  if (len2 < 1e-12)
    return p;                          // degenerate axis: identity
  double t = glm::dot(p - a, d) / len2; // projection param along the axis
  glm::dvec2 foot = a + t * d;
  return foot + (foot - p); // p mirrored through its foot on the axis
}

// Reflects an entity across the line a→b, preserving id/construction and the
// arc CCW-always invariant. A reflection reverses orientation, so an arc's
// swept endpoints are reflected and its start/end angles recomputed (the old
// start maps to the new *end*).
inline Entity ReflectEntity(const Entity& e, glm::dvec2 a, glm::dvec2 b) {
  Entity out = std::visit(
      [&](const auto& g) -> Entity {
        using T = std::decay_t<decltype(g)>;
        if constexpr (std::is_same_v<T, Point>)
          return Point{ReflectPoint(g.p, a, b)};
        else if constexpr (std::is_same_v<T, Line>)
          return Line{ReflectPoint(g.a, a, b), ReflectPoint(g.b, a, b)};
        else if constexpr (std::is_same_v<T, Circle>)
          return Circle{ReflectPoint(g.center, a, b), g.radius};
        else if constexpr (std::is_same_v<T, Arc>) {
          // Reflected start becomes the new end (winding flips).
          glm::dvec2 c = ReflectPoint(g.center, a, b);
          glm::dvec2 s = ReflectPoint(ArcPointUV(g, 1), a, b);
          glm::dvec2 en = ReflectPoint(ArcPointUV(g, 2), a, b);
          double a0 = std::atan2(en.y - c.y, en.x - c.x);
          double a1 = std::atan2(s.y - c.y, s.x - c.x);
          while (a1 <= a0)
            a1 += 2.0 * M_PI; // CCW-always
          return Arc{c, g.radius, a0, a1};
        } else if constexpr (std::is_same_v<T, Ellipse>) {
          // Ellipse: reflect the centre and the major-axis direction.
          glm::dvec2 c = ReflectPoint(g.center, a, b);
          glm::dvec2 maj = ReflectPoint(EllipsePointUV(g, 0.0), a, b);
          return Ellipse{c, g.rMajor, g.rMinor,
                         std::atan2(maj.y - c.y, maj.x - c.x)};
        } else if constexpr (std::is_same_v<T, EllipseArc>) {
          // Reflect centre + major axis; winding flips, so the reflected old
          // end becomes the new start and the params are re-derived.
          glm::dvec2 c = ReflectPoint(g.center, a, b);
          glm::dvec2 majTip = ReflectPoint(EllipseArcEvalUV(g, 0.0), a, b);
          double rot2 = std::atan2(majTip.y - c.y, majTip.x - c.x);
          glm::dvec2 s2 = ReflectPoint(EllipseArcPointUV(g, 1), a, b);
          glm::dvec2 e2 = ReflectPoint(EllipseArcPointUV(g, 2), a, b);
          double p0 = EllipseParamOfPoint(c, g.rMajor, g.rMinor, rot2, e2);
          double p1 = EllipseParamOfPoint(c, g.rMajor, g.rMinor, rot2, s2);
          while (p1 <= p0)
            p1 += 2.0 * M_PI; // CCW-always
          return EllipseArc{c, g.rMajor, g.rMinor, rot2, p0, p1};
        } else if constexpr (std::is_same_v<T, Text>) {
          // The anchor and the baseline direction reflect; the letters do
          // not — a mirrored label still reads (docs/sketching/README.md#text).
          Text t = g;
          t.anchor = ReflectPoint(g.anchor, a, b);
          const glm::dvec2 dir((std::cos(g.angle)), (std::sin(g.angle)));
          const glm::dvec2 tip = ReflectPoint(g.anchor + dir, a, b) - t.anchor;
          t.angle = std::atan2(tip.y, tip.x);
          return t;
        } else { // BSpline: reflect every pole
          BSpline bs = g;
          for (glm::dvec2& pole : bs.poles)
            pole = ReflectPoint(pole, a, b);
          return bs;
        }
      },
      e);
  SetEntityId(out, 0); // caller stamps a fresh id via AddEntity
  SetConstruction(out, IsConstruction(e));
  return out;
}

// Translates an entity by `t`, preserving id/construction (angles unchanged).
inline Entity TranslateEntity(const Entity& e, glm::dvec2 t) {
  Entity out = std::visit(
      [&](const auto& g) -> Entity {
        using T = std::decay_t<decltype(g)>;
        if constexpr (std::is_same_v<T, Point>)
          return Point{g.p + t};
        else if constexpr (std::is_same_v<T, Line>)
          return Line{g.a + t, g.b + t};
        else if constexpr (std::is_same_v<T, Circle>)
          return Circle{g.center + t, g.radius};
        else if constexpr (std::is_same_v<T, Arc>)
          return Arc{g.center + t, g.radius, g.startAngle, g.endAngle};
        else if constexpr (std::is_same_v<T, Ellipse>)
          return Ellipse{g.center + t, g.rMajor, g.rMinor, g.rotation};
        else if constexpr (std::is_same_v<T, EllipseArc>)
          return EllipseArc{g.center + t, g.rMajor,     g.rMinor,
                            g.rotation,   g.startParam, g.endParam};
        else if constexpr (std::is_same_v<T, Text>) {
          Text moved = g;
          moved.anchor += t;
          return moved;
        } else { // BSpline: translate every pole
          BSpline bs = g;
          for (glm::dvec2& pole : bs.poles)
            pole += t;
          return bs;
        }
      },
      e);
  SetEntityId(out, 0);
  SetConstruction(out, IsConstruction(e));
  return out;
}

// Rotates an entity by `ang` (radians, CCW) about `center`, preserving
// id/construction and the arc CCW-always invariant (adding `ang` to both arc
// angles keeps the sweep).
inline Entity RotateEntity(const Entity& e, glm::dvec2 center, double ang) {
  const double c = (std::cos(ang)), s = (std::sin(ang));
  auto rot = [&](glm::dvec2 p) {
    glm::dvec2 d = p - center;
    return center + glm::dvec2(c * d.x - s * d.y, s * d.x + c * d.y);
  };
  Entity out = std::visit(
      [&](const auto& g) -> Entity {
        using T = std::decay_t<decltype(g)>;
        if constexpr (std::is_same_v<T, Point>)
          return Point{rot(g.p)};
        else if constexpr (std::is_same_v<T, Line>)
          return Line{rot(g.a), rot(g.b)};
        else if constexpr (std::is_same_v<T, Circle>)
          return Circle{rot(g.center), g.radius};
        else if constexpr (std::is_same_v<T, Arc>)
          return Arc{rot(g.center), g.radius, g.startAngle + ang,
                     g.endAngle + ang};
        else if constexpr (std::is_same_v<T, Ellipse>)
          return Ellipse{rot(g.center), g.rMajor, g.rMinor, g.rotation + ang};
        else if constexpr (std::is_same_v<T, EllipseArc>)
          return EllipseArc{rot(g.center),    g.rMajor,     g.rMinor,
                            g.rotation + ang, g.startParam, g.endParam};
        else if constexpr (std::is_same_v<T, Text>) {
          Text turned = g;
          turned.anchor = rot(g.anchor);
          turned.angle = g.angle + ang;
          return turned;
        } else { // BSpline: rotate every pole
          BSpline bs = g;
          for (glm::dvec2& pole : bs.poles)
            pole = rot(pole);
          return bs;
        }
      },
      e);
  SetEntityId(out, 0);
  SetConstruction(out, IsConstruction(e));
  return out;
}

// UV of the point a ref addresses, for any alternative: Point → p; Line →
// a (which 0) / b (which 1); Circle → center; Arc → center/start/end per
// ArcPointUV. Single source of truth for RefUV / EntityPoints / EntityDots.
inline glm::dvec2 EntityPointUV(const Entity& e, uint8_t which) {
  return std::visit(
      [which](const auto& g) -> glm::dvec2 {
        using T = std::decay_t<decltype(g)>;
        if constexpr (std::is_same_v<T, Point>)
          return g.p;
        else if constexpr (std::is_same_v<T, Line>)
          return which == 1 ? g.b : g.a;
        else if constexpr (std::is_same_v<T, Circle>)
          return g.center;
        else if constexpr (std::is_same_v<T, Arc>)
          return ArcPointUV(g, which);
        else if constexpr (std::is_same_v<T, Ellipse>)
          return EllipseAxisEndUV(g.center, g.rMajor, g.rMinor, g.rotation,
                                  which); // centre, or an axis end (3..6)
        else if constexpr (std::is_same_v<T, EllipseArc>)
          return EllipseArcPointUV(g, which);
        else if constexpr (std::is_same_v<T, Text>)
          return which == 0 ? g.anchor : TextPointUV(g, which);
        else { // BSpline: which addresses pole k
          if (g.poles.empty())
            return glm::dvec2(0.0);
          return which < g.poles.size() ? g.poles[which] : g.poles.back();
        }
      },
      e);
}

// --- Expression-driven entity coordinates (Decision 2A) --------------------
// The scalar fields of an entity that may carry a driving expression:
//   Line   "ax" "ay" "bx" "by"   (endpoint coordinates)
//   Circle "cx" "cy" "r"         (center + radius)
//   Arc    "cx" "cy" "r"         (center + radius; angles deliberately not)
//   Point  "x" "y"
//   BSpline "p<k>x" "p<k>y"       (pole k, from 0)
//   Text   "x" "y" (anchor) "height" "angle_deg" "spacing" — the angle's
//          expression speaks DEGREES, as the angle dimension's does
// Ellipse fields are not expression-drivable: drive a distance from the
// centre to an axis end instead. Serialized as "<field>_expr" keys on the
// entity JSON; stored on SketchFeature::entityExprs.
std::vector<std::string> EntityExprFields(const Entity& e);
// {pole index, is x} for a spline field "p<k>x"/"p<k>y", else nullopt.
std::optional<std::pair<size_t, bool>> PoleField(const std::string& field);
// Read / write one such field. nullopt / false when the field doesn't apply
// to the entity kind.
std::optional<double> GetEntityField(const Entity& e, const std::string& field);
bool SetEntityField(Entity& e, const std::string& field, double value);

// A reference to a point on an entity. `entity` is the entity's stable **id**
// (not its index in the list — that shifts on insert/delete). `which` selects
// which point: Line 0 = a, 1 = b; Circle 0 = center; Arc 0 = center, 1 = start,
// 2 = end; Text 0 = anchor, 1..4 = its box corners (BL, BR, TR, TL). For
// whole-entity constraints (Horizontal on a line, Radius on a circle/arc),
// `which` is ignored and `a.entity` names the entity.
struct PointRef {
  uint32_t entity = 0;
  uint8_t which = 0;
};

// `which` for a SELECTION that means the whole entity rather than one of its
// points. Picking a line's endpoint and picking the line itself both used to
// store {id, 0}, so the drawing code — which could only compare the entity —
// highlighted the whole line whenever a vertex of it was selected. Real point
// indices are small (0, 1, 2), so a top-of-range sentinel separates them.
//
// SELECTION only. A constraint that references a line still uses {id, 0} and
// {id, 1} for its endpoints.
inline constexpr uint8_t kWholeEntity = 0xFF;

// Datum geometry: the sketch origin + X/Y axes. These are always available as
// constraint / mirror / snap references but are NOT stored in the entity list —
// they live in a reserved id block above any real entity id (entities are
// minted at random below kDatumBase, MintEntityId). `IndexOfEntity` returns
// SIZE_MAX for them, so every ref-by-index site must check IsDatum first and
// use the helpers below for their (fixed) geometry.
constexpr uint32_t kDatumBase = 0xFFFFFF00u;
constexpr uint32_t kOrigin = kDatumBase;    // point (0,0)
constexpr uint32_t kAxisX = kDatumBase + 1; // line (0,0) -> (1,0)  (+U)
constexpr uint32_t kAxisY = kDatumBase + 2; // line (0,0) -> (0,1)  (+V)

inline bool IsDatum(uint32_t id) { return id >= kDatumBase; }

// UV of a datum ref's point. Origin = (0,0); an axis's which 0 = origin, which
// 1 = its unit-direction end.
inline glm::dvec2 DatumPointUV(uint32_t entity, uint8_t which) {
  if (entity == kAxisX)
    return which == 1 ? glm::dvec2(1.0, 0.0) : glm::dvec2(0.0, 0.0);
  if (entity == kAxisY)
    return which == 1 ? glm::dvec2(0.0, 1.0) : glm::dvec2(0.0, 0.0);
  return glm::dvec2(0.0, 0.0); // origin
}

// The two points (a, b) defining a datum axis line (kAxisX / kAxisY).
inline std::pair<glm::dvec2, glm::dvec2> DatumAxisEndpoints(uint32_t entity) {
  if (entity == kAxisY)
    return {{0.0, 0.0}, {0.0, 1.0}};
  return {{0.0, 0.0}, {1.0, 0.0}}; // kAxisX
}

enum class ConstraintKind {
  Coincident,    // a, b are two points made equal
  Horizontal,    // a.entity is a line: its endpoints share y
  Vertical,      // a.entity is a line: its endpoints share x
  Parallel,      // a.entity, b.entity are two lines
  Perpendicular, // a.entity, b.entity are two lines
  Equal,         // a.entity, b.entity: equal length (lines) or radius (circles)
  Distance,      // a, b are points: |a-b| = value (driving dimension)
  Radius,        // a.entity is a circle: radius = value
  Angle,         // a.entity, b.entity are two lines: angle = value (radians)
  Tangent,       // a.entity is a line, b.entity is a circle
  PointOnLine,   // a is a point, b.entity is a line
  Symmetric,     // a, b are points symmetric about the line c.entity
  Midpoint,      // a is a point at the midpoint of the line b.entity
  Fixed,         // a is a point (or entity) pinned in place
  Concentric,    // a.entity, b.entity are circles sharing a center
  Collinear,     // a.entity, b.entity are two lines on the same infinite line
  DistanceX,     // a, b are points: b.x - a.x = value (driving dimension)
  DistanceY,     // a, b are points: b.y - a.y = value (driving dimension)
  Diameter,      // a.entity is a circle: diameter = value (radius = value/2)
  DistancePL,    // a is a point, b.entity a line: perpendicular distance =
                 // value (driving; also the parallel-lines distance, with a =
                 // an endpoint of the first line)
};

// Which kinds are driving dimensions (carry an editable numeric value + a
// canvas annotation with a `labelOffset`).
inline bool IsDimension(ConstraintKind k) {
  return k == ConstraintKind::Distance || k == ConstraintKind::DistanceX ||
         k == ConstraintKind::DistanceY || k == ConstraintKind::Radius ||
         k == ConstraintKind::Diameter || k == ConstraintKind::Angle ||
         k == ConstraintKind::DistancePL;
}

struct Constraint {
  ConstraintKind kind;
  PointRef a, b;
  double value = 0.0;          // Distance / Radius / Angle dimension
  PointRef c;                  // third ref (Symmetric: the symmetry line)
  glm::dvec2 labelOffset{0.0}; // dimension annotation placement (UV)
  // Optional expression driving `value` (empty = plain literal). Evaluated
  // against the document's parameter table on every regeneration; kept last
  // in the struct so existing aggregate initializers stay valid.
  std::string valueExpr;
};

enum class DerivedKind {
  LinearPattern,
  CircularPattern,
  // A LINKED MIRROR (v51): one reflected copy per source across an axis
  // line (or X/Y datum axis), replayed on every regeneration so the copies
  // track both their sources and the axis. Replaces the old
  // symmetry-constraint fan-out (which cluttered the constraint list and
  // could not be edited as one thing).
  Mirror,
  // A LINKED COPY (docs/sketching/README.md#linked-copies): pasted entities
  // that re-sync from their source until the link is broken. Same-sketch
  // sources replay in RegenerateDerived; cross-sketch sources (sourceSketchId
  // != 0) replay in SketchFeature::Execute, where the source sketch — required
  // earlier in the history — has already executed. Outputs are solver-fixed
  // anchors: target-sketch constraints may reference them, but their
  // position is owned by the sync.
  LinkedCopy,
};

// A *linked* generative op: it derives geometry from `sourceIds` and is
// replayed by SketchFeature::RegenerateDerived when the sources change.
// `instances` maps each (sourceId, k) to the stable id of the generated entity
// so refs survive a count change (the sketch-local analogue of SolveSpace's
// remap table). Baked ops leave no record.
struct DerivedOp {
  DerivedKind kind;
  std::vector<uint32_t> sourceIds;
  glm::dvec2 vec{0.0}; // Linear: per-step translation. Circular: centre (UV).
                       // LinkedCopy: the paste offset.
  double scalar = 0.0; // Circular: step angle (radians). Linear: unused.
  int count = 2;       // total instances including the seed (count-1 generated)
  // Linear only (v50): second grid direction. count2 = 1 keeps the pattern
  // 1D; instances flatten row-major (k = row*count + col), so the Inst
  // records need no new fields.
  glm::dvec2 vec2{0.0};
  int count2 = 1;
  // Mirror only (v51): the axis line entity (or X/Y datum axis id) the
  // copies reflect across — resolved fresh each regeneration.
  uint32_t axisEntity = 0;
  // Circular only (v51): optional centre REFERENCE — an entity point the
  // centre tracks (entity id + point index, like sketch::PointRef). 0 =
  // the baked `vec` coordinate.
  uint32_t centerEntity = 0;
  uint8_t centerWhich = 0;
  uint64_t sourceSketchId = 0; // LinkedCopy: cross-sketch source (0 = here)
  struct Inst {
    uint32_t sourceId = 0;
    int k = 0; // instance index 1..count-1
    uint32_t genId = 0;
  };
  std::vector<Inst> instances;
};

// External (projected) geometry: a reference to a face or edge of a body
// earlier in the history, projected onto the sketch plane as locked
// construction geometry (FreeCAD's ExternalGeometry / ShapeBinder). Unlike
// the old one-shot sketch-on-face bake, this is LIVE — SketchFeature::Execute
// re-projects it every regeneration, so the projected entities track the
// source when it moves. `genIds` are the construction entities it produced,
// kept stable across regens (like DerivedOp::Inst) so a user's constraints to
// them survive; the solver holds them FIXED (their position is owned by the
// source, not the solve).
enum class ExternalKind { Face, Edge };
struct ExternalGeom {
  ElementRef ref; // face (elementMap.names) or edge (edgeNames) of a body
  ExternalKind kind = ExternalKind::Face;
  std::vector<uint32_t> genIds; // generated construction entity ids (in order)
};

} // namespace sketch

// A 2D sketch in the parametric history. Owns its plane as a baked value —
// even when created on a model face, only the face's plane (center, normal)
// at creation time is stored, not a reference to the face itself. Persistent
// face references need topological naming (later milestone), so editing
// upstream features does NOT move dependent sketches yet.
//
// Contributes no bodies; downstream features (Extrude) consume its closed
// profiles, and the viewport renders its entities as line geometry.
class SketchFeature : public Feature {
public:
  SketchFeature(const std::string& name, const SketchPlane& plane);

  std::string TypeName() const override { return "Sketch"; }

  std::unique_ptr<Feature> Clone() const override {
    return std::make_unique<SketchFeature>(*this);
  }

  // Contributes no bodies, but when any dimension is expression-driven the
  // sketch re-solves here so downstream consumers (extrudes) see geometry
  // matching the current parameter values. A failed solve keeps the last
  // good entity positions rather than failing the history.
  bool Execute(RegenContext&) override;

  void VisitFeatureRefs(const RefVisitor& visit) const override {
    visit(planeRef);           // a DatumPlane this sketch tracks
    VisitRefs(faceRef, visit); // ...or a body face it is attached to
    for (const sketch::DerivedOp& op : derived)
      visit(op.sourceSketchId); // LinkedCopy across sketches
    for (const sketch::ExternalGeom& e : external)
      VisitRefs(e.ref, visit); // live projected geometry
  }

  // Dimensional-constraint (value, expression) pairs PLUS the
  // expression-driven entity coordinate fields (entityExprs); see
  // Feature::VisitExprs. Visiting the entity fields is what makes entity
  // expressions hash-visible: the refreshed coordinates land in the entity
  // JSON the regen cache keys on.
  void VisitExprs(const ExprVisitor& visit) override;

  // Appends `e` with a freshly stamped stable id and returns that id.
  uint32_t AddEntity(sketch::Entity e);

  // Appends a reference image (v49) with a freshly stamped stable id; returns
  // the id. If `img.asset` is empty it is assigned a stable default derived
  // from this feature's id and the new image id (the importer may override it).
  uint32_t AddImage(sketch::ReferenceImage img);
  // Index of the reference image with `id`, or SIZE_MAX if none.
  size_t IndexOfImage(uint32_t id) const;
  // Removes the reference image with `id`; returns true if one was removed.
  bool DeleteImage(uint32_t id);
  // Re-scales/aligns image `id` so its two picked plane-UV points `p1`, `p2`
  // sit `distance` plane units apart. `p1` is held fixed. When `alignAxis` is
  // 0 (horizontal) or 1 (vertical) the image is also rotated so the p1->p2 line
  // lands on that axis; any other value leaves the rotation unchanged. No-op
  // (returns false) if `id` is unknown or the two points coincide.
  bool CalibrateImage(uint32_t id, glm::dvec2 p1, glm::dvec2 p2,
                      double distance, int alignAxis = -1);
  // Index of the entity with the given id, or SIZE_MAX if none. Linear scan —
  // sketches hold few entities, and this avoids a desync-prone cached map.
  size_t IndexOfEntity(uint32_t id) const;

  // The profile-loop family of the loop `ids`: when that loop is exactly one
  // member of a linked pattern/mirror (the source, or one replication index),
  // every member of that op that forms a profile loop right now — else just
  // {ids}. A linked pattern is ONE thing (its copies regenerate from the
  // source), so picking one hole of a bolt circle as an extrude/revolve
  // profile means the bolt circle: a cut through the source but not its
  // copies is almost never the intent, and hand-clicking the other N-1
  // identical holes is how a five-hole flange took five region picks.
  // Ordering matches LoopEntityIds so the results are valid `regions` rows.
  std::vector<std::vector<uint32_t>>
  SiblingRegions(const std::vector<uint32_t>& ids) const;
  // Removes the entity with `id` and drops every constraint that references
  // it; coincidences chained through it are repaired (A~B~C keeps A~C once B
  // is gone). Nothing else is transferred to a surviving entity. Returns the
  // number of *non-trivial* constraints dropped (i.e. not
  // Coincident/Horizontal/Vertical), for a user-facing warning; the entity is
  // still removed. Returns 0 and does nothing if `id` is unknown.
  size_t DeleteEntity(uint32_t id);

  // Adds an axis-aligned rectangle as four real lines with shared corner
  // points: 4 lines + 4 corner coincidents + 2 horizontal + 2 vertical
  // constraints. Corners derive from the opposite corners c1, c2. Returns the
  // four line ids (bottom, right, top, left). Each side is individually
  // editable and trimmable.
  std::array<uint32_t, 4> AddRectangle(glm::dvec2 c1, glm::dvec2 c2);

  // Centre + corner rectangle: AddRectangle's four sides PLUS a centre Point
  // entity anchored midway between the diagonal corners (Symmetric-about-a-
  // point). Returns {bottom, right, top, left, centre point}.
  std::array<uint32_t, 5> AddRectangleCentered(glm::dvec2 center,
                                               glm::dvec2 corner);

  // Splits the line `id` at point P (assumed on the segment). Piece 1 keeps
  // `id` (its far endpoint becomes P); piece 2 is a new line P -> original-end.
  // Constraints transfer per the FreeCAD lines-only policy: the far-endpoint
  // refs move to piece 2, whole-edge distances re-anchor to span both pieces,
  // H/V/parallel/perpendicular are copied onto piece 2 (so it stays aligned),
  // and a coincident joins the two pieces at the cut. Returns piece 2's id, or
  // 0 if `id` isn't a line or P is at an endpoint (degenerate).
  uint32_t SplitEntity(uint32_t id, glm::dvec2 P);

  // Trims the line `id` at `clickUV`: removes the sub-segment the click falls
  // in, bounded by the line's intersections with other lines (via SplitEntity +
  // DeleteEntity). With no bracketing intersection the whole line is deleted.
  // Returns true if anything changed. Lines only — circles and arcs are
  // trimmed by TrimCurveAt.
  bool TrimLineAt(uint32_t id, glm::dvec2 clickUV);

  // Trims the circle or arc `id` at `clickUV`: removes the angular span the
  // click falls in, bounded by the curve's intersections with other lines (and
  // the arc's own endpoints). A trimmed circle becomes one arc (the complement
  // of the removed span, keeping the id); a trimmed arc becomes up to two arcs
  // (the first keeps the id, the rest are new entities). With no bracketing
  // crossing the whole curve is deleted. Returns true if anything changed.
  bool TrimCurveAt(uint32_t id, glm::dvec2 clickUV);

  // The span (two UV endpoints) that TrimLineAt would remove for a click on
  // line `id` at `clickUV` — the sub-segment bounded by neighbouring crossings.
  // For a hover preview so the user sees exactly what trim will delete. Nullopt
  // if `id` isn't a line.
  std::optional<std::pair<glm::dvec2, glm::dvec2>>
  TrimSpanAt(uint32_t id, glm::dvec2 clickUV) const;

  // Mirrors `sourceIds` across the line entity `axisLineId`, appending the
  // reflected copies (fresh ids) and returning their ids. When `link` is true
  // the copies come from a Mirror derived op, replayed on each regeneration,
  // so they track edits to their sources and to the axis; no constraints are
  // added. `link=false` bakes an unlinked copy. No-op (empty) if the axis
  // isn't a line.
  std::vector<uint32_t> MirrorEntities(const std::vector<uint32_t>& sourceIds,
                                       uint32_t axisLineId, bool link);

  // Rounds the corner where line `lineId1` and `lineId2` meet with a tangent
  // arc of the given radius: trims both lines to the tangent points, inserts
  // the arc, and adds two Tangent + two Coincident constraints (removing the
  // old corner coincident). Returns the new arc's id, or 0 if the lines don't
  // share a corner, are parallel, or the radius is too large for either
  // segment — `whyNot` (when given) receives the specific reason.
  uint32_t FilletCorner(uint32_t lineId1, uint32_t lineId2, double radius,
                        std::string* whyNot = nullptr);

  // Patterns of `sourceIds`. Linear: `count` instances (incl. the seed) each
  // stepped by `step` in UV. Circular: `count` instances stepped by `stepAngle`
  // (radians) about `center`. `link=false` bakes unlinked copies and returns
  // their ids; `link=true` records a DerivedOp (regenerates on source edits, id
  // stable across count changes) and returns the generated ids.
  // step2/count2 = the optional second grid direction (v50); count2 = 1
  // keeps it a 1D row.
  std::vector<uint32_t> AddLinearPattern(const std::vector<uint32_t>& sourceIds,
                                         glm::dvec2 step, int count, bool link,
                                         glm::dvec2 step2 = {0.0, 0.0},
                                         int count2 = 1);
  std::vector<uint32_t>
  // centerEntity/centerWhich (v51): optional entity-point reference the
  // centre tracks across regenerations; 0 = the baked `center` coordinate.
  AddCircularPattern(const std::vector<uint32_t>& sourceIds, glm::dvec2 center,
                     double stepAngle, int count, bool link,
                     uint32_t centerEntity = 0, uint8_t centerWhich = 0);

  // Offsets the closed loop formed by `chainIds` by `distance` (signed; corner
  // treatment via `joinArc` — arcs at convex corners, else extend/trim),
  // appending the offset boundary as new entities and returning their ids.
  // Empty if the chain isn't a single closed loop or OCC fails. Always bakes
  // (unlinked); offset is not yet a regenerating DerivedOp.
  std::vector<uint32_t> OffsetChain(const std::vector<uint32_t>& chainIds,
                                    double distance, bool joinArc);

  // Replays every linked DerivedOp: recomputes each generated entity from its
  // (current) source, keeping generated ids stable — reusing the entity for a
  // surviving (source, k), creating new ones when count grows, deleting the
  // surplus when it shrinks. Call after the constraint solve on any edit.
  void RegenerateDerived();

  // Removes a linked DerivedOp and the entities it generated (its instances).
  // No-op if opIndex is out of range. Call RegenerateDerived after for the
  // rest.
  void RemoveDerivedOp(size_t opIndex);

  // Adds a live external-geometry reference (a face or edge of a body earlier
  // in the history), projected onto this sketch's plane as locked construction
  // geometry. The projection materialises on the next Execute; returns the
  // index into `external`. The body/element must resolve during regeneration.
  size_t AddExternalGeometry(const ElementRef& ref, sketch::ExternalKind kind);

  // Removes external ref `extIndex` and the construction entities it generated.
  // No-op if out of range.
  void RemoveExternalGeometry(size_t extIndex);

  // Re-projects every external reference against the current bodies, refreshing
  // its generated construction entities in place (stable ids). Called by
  // Execute. Returns false (and SetError) when a ref no longer resolves.
  bool RegenerateExternal(const RegenContext& ctx);

  // Breaks a LinkedCopy link: removes the op record but KEEPS its output
  // entities (ids intact, so downstream refs survive) as ordinary free
  // geometry. No-op for other kinds / out-of-range.
  void BreakLink(size_t opIndex);

  // Replays one cross-sketch LinkedCopy op from `source`'s entities. False
  // when any source entity id no longer exists there (the caller errors the
  // sketch with a break-link hint).
  bool ReplayLinkedCopyFrom(sketch::DerivedOp& op, const SketchFeature& source);

  SketchPlane plane;
  // When non-zero: the id of a DatumPlaneFeature earlier in the history;
  // Execute refreshes `plane` from its output each regeneration, so the
  // sketch tracks datum edits. 0 = `plane` is a baked value (legacy
  // behavior, and still how base-plane sketches work).
  uint64_t planeRef = 0;
  // When set: a body face this sketch is attached to (topological naming, T4).
  // Execute re-derives `plane` from the resolved face each regeneration, so the
  // sketch (and everything built on it) follows edits to the base body. Takes
  // precedence over planeRef; mutually exclusive with it in practice.
  ElementRef faceRef;
  std::vector<sketch::Entity> entities;
  // Reference underlays (v49). Cosmetic: excluded from the profile, the solve,
  // and the regen geometry hash (toggling one must not rebuild downstream).
  std::vector<sketch::ReferenceImage> images;
  uint32_t nextImageId = 1; // next reference-image stable id (1-based)
  std::vector<sketch::Constraint> constraints;
  std::vector<sketch::DerivedOp> derived; // linked generative ops (replayed)
  std::vector<sketch::ExternalGeom> external; // live projected geometry
  // Expression-driven entity coordinates (Decision 2A, v38): entity stable
  // id -> {field -> expression}, fields per sketch::EntityExprFields. The
  // expressions are evaluated into the entity geometry by VisitExprs during
  // Document::Regenerate's pre-hash refresh (so they are hash-visible to the
  // regen cache), and the driven parameters are pinned solver-FIXED during
  // the sketch solve (the linked-copy-output mechanism). A parallel map —
  // not fields on the entity variant — because entities are copied and
  // transformed wholesale by mirrors/patterns/derived-op replay, which must
  // not silently duplicate driving expressions, and the aggregate entity
  // structs (and every brace-init of them) stay untouched.
  std::map<uint32_t, std::map<std::string, std::string>> entityExprs;

  // An id no other entity in this sketch has, and that a sketch edited
  // SEPARATELY is not going to mint either (F9).
  //
  // This was `nextEntityId++`, a per-sketch counter — which meant two people
  // editing one sketch offline both got the same next id. That matters more
  // than it looks: a face's stable name is literally "<feature>/g<entityId>"
  // (SketchProfile), so two entities sharing an id make a face NAME
  // ambiguous, and every reference keyed on it — a fillet, a mate, a drawing
  // dimension — can bind to the wrong face while the document rebuilds
  // perfectly and says nothing.
  //
  // Random rather than sequential because, unlike a FEATURE id, an entity id
  // is never a filename: it lives inside the sketch's own JSON, so the
  // readability that kept feature ids sequential is not on the table here.
  // Retried against this sketch's entities, so uniqueness within a sketch is
  // exact rather than probable.
  [[nodiscard]] uint32_t MintEntityId() const;

private:
  // Shared by TrimLineAt/TrimSpanAt: for line `id` at `clickUV`, outputs the
  // line origin `a`, direction `ab`, and the [tLo,tHi] span to remove. Returns
  // false if `id` isn't a line.
  bool TrimBracket(uint32_t id, glm::dvec2 clickUV, glm::dvec2& a,
                   glm::dvec2& ab, double& tLo, double& tHi) const;
};

// Resolves a face ElementRef to a work plane (planar faces only): shared by
// sketch-on-face (v16) and the up-to-face extrude end condition (M3.3).
std::optional<SketchPlane> ResolveFacePlane(const RegenContext& ctx,
                                            const ElementRef& ref);

// True when the sketch holds anything the USER made: any entity that is not
// generated external geometry (a face-attached sketch arrives holding the
// host face's projected outline), or any derived op / reference image.
// The Hole tool uses it to decide whether cancelling should discard the
// sketch it auto-created, or keep work the user actually did.
bool HasUserGeometry(const SketchFeature& sketch);
