#include "core/cam/CamToolpath.h"

#include "core/Measure.h"
#include "core/cam/CamAdaptive.h"

#include <clipper2/clipper.h>

#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepBndLib.hxx>
#include <BRepTools.hxx>
#include <BRepTools_WireExplorer.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box.hxx>
#include <GCPnts_QuasiUniformDeflection.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Wire.hxx>
#include <gp_Cylinder.hxx>
#include <gp_Pnt.hxx>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <numbers>
#include <optional>
#include <string>

#include <glm/trigonometric.hpp>

namespace cam {

namespace {

// Clipper2 works in integers. 1e4 puts the quantum at 0.1 µm — four orders
// below anything a mill can hold, and far enough from int64's range that a
// metre-scale part cannot overflow. ONE constant, because two would disagree.
constexpr double kScale = 1.0e4;

// The chord tolerance curved edges are flattened to. One constant, so a
// helix is no coarser than the walls it sits between (it is also
// SectionLoops' default).
constexpr double kDeflection = 0.02;

Clipper2Lib::Path64 ToClipper(const std::vector<glm::dvec2>& pts) {
  Clipper2Lib::Path64 out;
  out.reserve(pts.size());
  for (const glm::dvec2& p : pts)
    out.emplace_back(static_cast<int64_t>(std::llround(p.x * kScale)),
                     static_cast<int64_t>(std::llround(p.y * kScale)));
  return out;
}

std::vector<glm::dvec2> FromClipper(const Clipper2Lib::Path64& path) {
  std::vector<glm::dvec2> out;
  out.reserve(path.size());
  for (const Clipper2Lib::Point64& p : path)
    out.push_back(
        {static_cast<double>(p.x) / kScale, static_cast<double>(p.y) / kScale});
  return out;
}

Clipper2Lib::Paths64 ToClipper(const std::vector<Loop>& loops) {
  Clipper2Lib::Paths64 out;
  out.reserve(loops.size());
  for (const Loop& l : loops)
    if (l.pts.size() >= 3)
      out.push_back(ToClipper(l.pts));
  return out;
}

// Offsets a set of closed paths. Positive grows, negative shrinks; an empty
// result means the region closed up, which is how concentric clearing knows
// it is done.
Clipper2Lib::Paths64 Offset(const Clipper2Lib::Paths64& paths, double deltaMm) {
  if (paths.empty())
    return {};
  Clipper2Lib::ClipperOffset co;
  // Round joins: a mill cannot cut a sharp inside corner anyway, and square
  // joins on an outside profile put the tool through the part corner.
  co.AddPaths(paths, Clipper2Lib::JoinType::Round,
              Clipper2Lib::EndType::Polygon);
  Clipper2Lib::Paths64 out;
  co.Execute(deltaMm * kScale, out);
  return out;
}

// Traversal direction. The convention, stated once because every machinist
// checks it on the first cut: with a right-hand cutter and the tool OUTSIDE
// the material, climb milling runs clockwise seen from +Z; inside a pocket it
// runs counter-clockwise. Clipper hands back outers CCW and holes CW.
void Orient(Clipper2Lib::Path64& path, bool climb, bool insideMaterial) {
  const bool wantCcw = climb == insideMaterial;
  if (Clipper2Lib::IsPositive(path) != wantCcw)
    std::reverse(path.begin(), path.end());
}

double Perimeter(const std::vector<glm::dvec2>& pts) {
  double total = 0.0;
  for (size_t i = 0; i + 1 < pts.size(); i++)
    total += glm::length(pts[i + 1] - pts[i]);
  if (pts.size() > 2)
    total += glm::length(pts.front() - pts.back());
  return total;
}

// Walks one wire into an ordered polyline, honouring edge orientation so the
// points come out in sequence rather than in map order.
std::vector<glm::dvec2> WireToLoop(const TopoDS_Wire& wire, double deflection) {
  std::vector<glm::dvec2> pts;
  BRepTools_WireExplorer ex(wire);
  for (; ex.More(); ex.Next()) {
    const TopoDS_Edge& edge = ex.Current();
    BRepAdaptor_Curve curve(edge);
    GCPnts_QuasiUniformDeflection sampler(curve, deflection);
    if (!sampler.IsDone())
      continue;
    const int n = sampler.NbPoints();
    const bool reversed = edge.Orientation() == TopAbs_REVERSED;
    for (int k = 0; k < n; k++) {
      const gp_Pnt p = sampler.Value(reversed ? n - k : k + 1);
      const glm::dvec2 xy(p.X(), p.Y());
      // The last point of one edge is the first of the next.
      if (!pts.empty() && glm::length(pts.back() - xy) < 1e-9)
        continue;
      pts.push_back(xy);
    }
  }
  if (pts.size() > 2 && glm::length(pts.front() - pts.back()) < 1e-9)
    pts.pop_back();
  return pts;
}

// A millimetre figure for a message. std::to_string on a double gives
// "3.000000", which reads as machine noise in a sentence meant for a person.
std::string Round2(double mm) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%g", std::round(mm * 100.0) / 100.0);
  return buf;
}

void Emit(Toolpath& tp, Move::Kind kind, const glm::dvec3& to, double feed) {
  Move m;
  m.kind = kind;
  m.to = to;
  m.feed = feed;
  tp.moves.push_back(m);
}

// Rapid over, plunge, cut the loop, retract. Tabs raise the tool over evenly
// spaced spans so the part stays attached on the final pass.
//
// The subtlety that bit on the first run: a tab span has to SPLIT the
// segments it crosses. A profiled rectangle is four segments of 40 mm, and
// testing "is the end of this segment inside a tab?" steps straight over
// every tab on the part — silently, with the file looking plausible. So the
// loop is walked by arc length and cut at each tab boundary, with an explicit
// vertical move there, which also makes the tab square-walled rather than a
// diagonal ramp.
// Entry ramp angle. Shallow enough for a non-centre-cutting end mill to
// enter metal, steep enough not to spend a lap descending. A knob later if
// anyone needs one; a fixed sane value beats a vertical plunge today.
constexpr double kRampAngleDeg = 3.0;

// A helix down to depth, for a loop too short to hold the ramp (C6,
// docs/cam/plan.md). Descending on the toolpath itself is right when the
// path is long; on a small pocket that lap is short and the descent gets
// steep, so instead the tool spirals down inside the region and the rings
// are then cut from the middle outwards.
//
// The circle it walks is the TOOL CENTRE's, so `radius` must already have the
// cutter's own radius taken out of it — a helix sized from the wall instead
// of the centre is a cut through the wall.
void EmitHelix(Toolpath& tp, const Tool& tool, const glm::dvec2& centre,
               double radius, double fromZ, double toZ, double safeZ,
               double deflection) {
  const double descend = fromZ - toZ;
  if (radius < 1e-9 || descend <= 1e-9)
    return;

  // Segments per revolution from the same chord tolerance the section used
  // to make these loops, so the helix is no coarser than the walls it sits
  // between. Bounded because a tiny radius would otherwise ask for hundreds.
  const double perSeg =
      radius > deflection ? 2.0 * std::acos(1.0 - deflection / radius) : 1.0;
  const int segments = std::clamp(
      static_cast<int>(std::ceil(2.0 * std::numbers::pi / perSeg)), 8, 256);
  const double circumference = 2.0 * std::numbers::pi * radius;
  const double perRev = circumference * std::tan(glm::radians(kRampAngleDeg));
  const double revolutions = perRev > 1e-9 ? descend / perRev : 1.0;
  const int steps =
      std::max(segments, static_cast<int>(std::ceil(revolutions * segments)));

  const glm::dvec2 start(centre.x + radius, centre.y);
  Emit(tp, Move::Kind::Rapid, {start.x, start.y, safeZ}, 0.0);
  Emit(tp, Move::Kind::Rapid, {start.x, start.y, fromZ}, 0.0);
  for (int i = 1; i <= steps; i++) {
    const double t = static_cast<double>(i) / steps;
    const double a = 2.0 * std::numbers::pi * revolutions * t;
    Emit(tp, Move::Kind::Cut,
         {centre.x + radius * std::cos(a), centre.y + radius * std::sin(a),
          fromZ - descend * t},
         tool.feedPlunge);
  }
  // One flat turn at depth: it clears the disc the spiral only grazed, and
  // it leaves the tool on a known circle for the first ring to join.
  const double endAngle = 2.0 * std::numbers::pi * revolutions;
  for (int i = 1; i <= segments; i++) {
    const double a = endAngle + 2.0 * std::numbers::pi * i / segments;
    Emit(
        tp, Move::Kind::Cut,
        {centre.x + radius * std::cos(a), centre.y + radius * std::sin(a), toZ},
        tool.feedCut);
  }
}

// Cut the loop at depth `z`, entering from `entryZ` by RAMPING along the path
// rather than plunging straight down — a vertical plunge with a
// non-centre-cutting mill rubs rather than cuts. The ramp runs along the
// toolpath itself, so it needs no room the cut does not already have; the lap
// then starts where the ramp finished, which is why the point list is
// rotated rather than restarted.
//
// C6: `entry` says how the tool gets to depth. `Ramp` is the above.
// `AtDepth` means the caller already descended (a helix) and the tool is
// standing in cleared material at `z` — so the loop is joined with a cut
// move instead of a retract-and-ramp, which is the whole point of clearing
// inside-out after a helix.
enum class Entry { Ramp, AtDepth };

void EmitLoop(Toolpath& tp, const Setup& setup, const Tool& tool,
              const std::vector<glm::dvec2>& pts, double z, double safeZ,
              int tabCount, double tabWidth, double tabZ, double entryZ,
              Entry entry = Entry::Ramp) {
  if (pts.size() < 2)
    return;
  const double perimeter = Perimeter(pts);
  if (perimeter < 1e-9)
    return;

  if (entry == Entry::AtDepth) {
    // Straight to the start of this loop at depth. The step from the
    // previous ring is at most one step-over through material that ring
    // already cleared.
    Emit(tp, Move::Kind::Cut, {pts[0].x, pts[0].y, z}, tool.feedCut);
    std::vector<glm::dvec2> lap(pts.begin(), pts.end());
    lap.push_back(pts[0]);
    for (size_t i = 1; i < lap.size(); i++)
      Emit(tp, Move::Kind::Cut, {lap[i].x, lap[i].y, z}, tool.feedCut);
    return;
  }

  // Rapid over, then down to the height we enter from: that is either the
  // stock top or the depth the previous pass already cut here, so it is air.
  Emit(tp, Move::Kind::Rapid, {pts[0].x, pts[0].y, safeZ}, 0.0);
  const double from = std::max(entryZ, z);
  Emit(tp, Move::Kind::Rapid, {pts[0].x, pts[0].y, from}, 0.0);

  const double descend = from - z;
  double rampLen =
      descend > 1e-9 ? descend / std::tan(glm::radians(kRampAngleDeg)) : 0.0;
  // A loop shorter than the ramp would spiral forever: descend over one lap
  // instead, steeper than asked but bounded and still not a plunge.
  rampLen = std::min(rampLen, perimeter);

  // Tabs, as arc-length intervals measured from pts[0]. The DESCENT has to
  // know about them as much as the lap does: it takes most of a lap to get
  // down, so a ramp that ran through a tab took the top off it long before
  // the pass that was meant to leave it standing. Both the ramp and the lap
  // are now one function of arc length, which is the only way the two can
  // agree about where a tab is.
  const bool wantTabs =
      tabCount > 0 && tabWidth > 0.0 && perimeter > tabWidth * tabCount;
  std::vector<std::pair<double, double>> tabs;
  if (wantTabs)
    for (int i = 0; i < tabCount; i++) {
      const double centre = perimeter * (static_cast<double>(i) + 0.5) /
                            static_cast<double>(tabCount);
      tabs.emplace_back(centre - tabWidth * 0.5, centre + tabWidth * 0.5);
    }
  const auto wrap = [&](double s) {
    return std::fmod(std::fmod(s, perimeter) + perimeter, perimeter);
  };
  const auto tabTop = [&](double s) {
    const double m = wrap(s);
    for (const auto& [lo, hi] : tabs)
      if (m > lo && m < hi)
        return tabZ;
    return -1e300;
  };
  // Where the tool belongs `s` mm along the loop, ignoring tabs: descending
  // while it ramps, at depth after that.
  const auto zPath = [&](double s) {
    return rampLen > 1e-9 && s < rampLen ? from - descend * (s / rampLen) : z;
  };
  const auto pointAt = [&](double s) {
    double m = wrap(s);
    for (size_t i = 0; i < pts.size(); i++) {
      const glm::dvec2& a = pts[i];
      const glm::dvec2& b = pts[(i + 1) % pts.size()];
      const double seg = glm::length(b - a);
      if (m <= seg || i + 1 == pts.size())
        return seg < 1e-12 ? a : a + (b - a) * std::clamp(m / seg, 0.0, 1.0);
      m -= seg;
    }
    return pts[0];
  };

  // A full lap AFTER the ramp finishes, so every millimetre of the contour is
  // cut at depth even though the descent used most of one.
  const double total = rampLen + perimeter;
  std::vector<double> stations;
  stations.reserve(pts.size() * 2 + tabs.size() * 4 + 4);
  {
    double acc = 0.0;
    for (size_t i = 0; i < pts.size() && acc <= total; i++) {
      acc += glm::length(pts[(i + 1) % pts.size()] - pts[i]);
      for (double a = acc; a <= total + 1e-9; a += perimeter)
        stations.push_back(a);
    }
    for (const auto& [lo, hi] : tabs)
      for (const double edge : {lo, hi})
        for (double a = edge; a <= total + 1e-9; a += perimeter)
          stations.push_back(a);
    if (rampLen > 1e-9)
      stations.push_back(rampLen);
    stations.push_back(total);
    std::sort(stations.begin(), stations.end());
    stations.erase(
        std::remove_if(stations.begin(), stations.end(),
                       [&](double a) { return a < 1e-9 || a > total + 1e-9; }),
        stations.end());
    stations.erase(
        std::unique(stations.begin(), stations.end(),
                    [](double a, double b) { return std::abs(a - b) < 1e-9; }),
        stations.end());
  }

  if (rampLen <= 1e-9)
    Emit(tp, Move::Kind::Plunge, {pts[0].x, pts[0].y, std::max(z, tabTop(0.0))},
         tool.feedPlunge);

  // Walk it segment by segment. Every tab edge is a station, so a segment is
  // wholly inside a tab or wholly outside one — which is what lets the lift
  // happen AT the edge as a vertical move instead of sloping into it.
  double currentZ = std::max(zPath(0.0), tabTop(0.0));
  double prev = 0.0;
  for (const double s : stations) {
    const double onTab = tabTop((prev + s) * 0.5);
    const double zStart = std::max(zPath(prev), onTab);
    const double zEnd = std::max(zPath(s), onTab);
    const glm::dvec2 a = pointAt(prev), b = pointAt(s);
    if (std::abs(zStart - currentZ) > 1e-9) {
      Emit(tp, Move::Kind::Cut, {a.x, a.y, zStart}, tool.feedCut);
      currentZ = zStart;
    }
    Emit(tp, Move::Kind::Cut, {b.x, b.y, zEnd},
         s <= rampLen + 1e-9 ? tool.feedPlunge : tool.feedCut);
    currentZ = zEnd;
    prev = s;
  }
  const glm::dvec2 last = pointAt(total);
  Emit(tp, Move::Kind::Rapid, {last.x, last.y, safeZ}, 0.0);
}

} // namespace

double Toolpath::CutLength() const {
  double total = 0.0;
  for (size_t i = 1; i < moves.size(); i++)
    if (moves[i].kind == Move::Kind::Cut)
      total += glm::length(moves[i].to - moves[i - 1].to);
  return total;
}

double Toolpath::MinZ() const {
  double lowest = 0.0;
  bool any = false;
  for (const Move& m : moves) {
    if (!any || m.to.z < lowest) {
      lowest = m.to.z;
      any = true;
    }
  }
  return any ? lowest : 0.0;
}

std::vector<Loop> SectionLoops(const std::vector<Body>& bodies, double z,
                               double deflection) {
  std::vector<Loop> loops;
  // The section machinery the Section TOOL already uses (I3): one plane, the
  // filled cut face per body. Reusing it means a toolpath cannot disagree
  // with what the section view shows.
  const std::vector<CADShape> caps =
      Measure::SectionCaps(bodies, glm::vec3(0.0f, 0.0f, static_cast<float>(z)),
                           glm::vec3(0.0f, 0.0f, 1.0f));
  for (const CADShape& cap : caps) {
    if (!cap.IsValid())
      continue;
    for (TopExp_Explorer fx(cap.GetShape(), TopAbs_FACE); fx.More();
         fx.Next()) {
      const TopoDS_Face face = TopoDS::Face(fx.Current());
      const TopoDS_Wire outer = BRepTools::OuterWire(face);
      for (TopExp_Explorer wx(face, TopAbs_WIRE); wx.More(); wx.Next()) {
        const TopoDS_Wire wire = TopoDS::Wire(wx.Current());
        Loop loop;
        loop.pts = WireToLoop(wire, deflection);
        if (loop.pts.size() < 3)
          continue;
        loop.hole = !wire.IsSame(outer);
        loops.push_back(std::move(loop));
      }
    }
  }
  return loops;
}

std::vector<double> DepthPasses(const Operation& op) {
  std::vector<double> zs;
  const double top = op.topZ;
  const double bottom = op.bottomZ + op.stockToLeave;
  if (bottom >= top) {
    zs.push_back(bottom);
    return zs;
  }
  if (op.stepdown <= 0.0) {
    zs.push_back(bottom);
    return zs;
  }
  double z = top - op.stepdown;
  while (z > bottom + 1e-9) {
    zs.push_back(z);
    z -= op.stepdown;
  }
  // The final pass lands exactly on the requested depth: a pass a hair deeper
  // than asked is a scrapped part, and a hair shallower is a witness ridge.
  zs.push_back(bottom);
  return zs;
}

// Assumes `bodies` are ALREADY in setup space. The public StockBounds frames
// first; internal callers that framed once do not want it done twice.
bool StockBoundsFramed(const Setup& setup, const std::vector<Body>& bodies,
                       glm::dvec3& min, glm::dvec3& max) {
  bool any = false;
  glm::vec3 lo(0.0f), hi(0.0f);
  for (const Body& b : bodies) {
    if (!b.shape.IsValid())
      continue;
    glm::vec3 bmin(0.0f), bmax(0.0f);
    b.shape.GetBoundingBox(bmin, bmax);
    lo = any ? glm::min(lo, bmin) : bmin;
    hi = any ? glm::max(hi, bmax) : bmax;
    any = true;
  }
  if (!any)
    return false;
  const Stock& s = setup.stock;
  min = glm::dvec3(lo) - glm::dvec3(s.margin.x, s.margin.y, s.bottomMargin);
  max = glm::dvec3(hi) + glm::dvec3(s.margin.x, s.margin.y, s.margin.z);
  return true;
}

bool StockBounds(const Setup& setup, const std::vector<Body>& bodies,
                 glm::dvec3& min, glm::dvec3& max) {
  return StockBoundsFramed(setup, BodiesInSetupFrame(setup, bodies), min, max);
}

glm::dvec3 FromSetupSpace(const Setup& setup, const glm::dvec3& p) {
  if (std::abs(setup.rotationA) < 1e-9)
    return p;
  const double a = glm::radians(setup.rotationA);
  const double c = std::cos(a), s = std::sin(a);
  return {p.x, p.y * c - p.z * s, p.y * s + p.z * c};
}

FaceRegion PickedFaceRegion(const std::vector<Body>& bodies,
                            const ElementRef& ref, double deflection) {
  FaceRegion out;
  if (!ref.IsSet()) {
    out.why = "nothing picked";
    return out;
  }
  for (const Body& body : bodies) {
    if (body.createdBy != ref.featureId || !body.shape.IsValid())
      continue;
    const std::optional<int> idx = body.elementMap.Resolve(ref.name);
    if (!idx) {
      out.why = "that face is gone from the model";
      return out;
    }
    glm::vec3 centre(0.0f), normal(0.0f), uAxis(0.0f);
    if (!body.shape.FacePlane(*idx, centre, normal, uAxis)) {
      out.why = "pick a flat face — that one is curved";
      return out;
    }
    // A 2.5D job cuts in Z, so a face it can machine to is level. A tilted
    // face has no single depth, and guessing one puts the cut somewhere the
    // user did not point at.
    if (std::abs(normal.z) < 0.99f) {
      out.why = "pick a level face — a tilted one has no single depth";
      return out;
    }
    // Walk to the face in the same explorer order FacePlane uses.
    TopoDS_Face face;
    int i = 0;
    for (TopExp_Explorer ex(body.shape.GetShape(), TopAbs_FACE); ex.More();
         ex.Next(), i++)
      if (i == *idx) {
        face = TopoDS::Face(ex.Current());
        break;
      }
    if (face.IsNull()) {
      out.why = "that face is gone from the model";
      return out;
    }
    const TopoDS_Wire outer = BRepTools::OuterWire(face);
    for (TopExp_Explorer wx(face, TopAbs_WIRE); wx.More(); wx.Next()) {
      const TopoDS_Wire wire = TopoDS::Wire(wx.Current());
      Loop loop;
      loop.pts = WireToLoop(wire, deflection);
      if (loop.pts.size() < 3)
        continue;
      // Inner wires are islands: material standing in the pocket, which the
      // clearing region has to go around rather than through.
      loop.hole = !wire.IsSame(outer);
      if (loop.hole)
        out.loops.push_back(std::move(loop));
      else
        out.loops.insert(out.loops.begin(), std::move(loop));
    }
    if (out.loops.empty()) {
      out.why = "that face has no usable boundary";
      return out;
    }
    out.z = centre.z;
    out.ok = true;
    return out;
  }
  out.why = "that face is not on a body this job can machine";
  return out;
}

namespace {

// The face `ref` names, and the body it belongs to. Both picks below need the
// same walk, and doing it twice was how the two disagreed about which face a
// name meant.
bool FindFace(const std::vector<Body>& bodies, const ElementRef& ref,
              TopoDS_Face& face, glm::vec3& centre, glm::vec3& normal) {
  if (!ref.IsSet())
    return false;
  for (const Body& body : bodies) {
    if (body.createdBy != ref.featureId || !body.shape.IsValid())
      continue;
    const std::optional<int> idx = body.elementMap.Resolve(ref.name);
    if (!idx)
      return false;
    glm::vec3 uAxis(0.0f);
    if (!body.shape.FacePlane(*idx, centre, normal, uAxis))
      return false;
    int i = 0;
    for (TopExp_Explorer ex(body.shape.GetShape(), TopAbs_FACE); ex.More();
         ex.Next(), i++)
      if (i == *idx) {
        face = TopoDS::Face(ex.Current());
        return !face.IsNull();
      }
    return false;
  }
  return false;
}

// The shortest distance from `p` to a loop's boundary. Which loop a wall
// belongs to is decided by this rather than by containment: a wall lies ON
// its own contour, and "on" is unreliable to test with point-in-polygon.
double DistanceToLoop(const std::vector<glm::dvec2>& pts, const glm::dvec2& p) {
  double best = std::numeric_limits<double>::max();
  for (size_t i = 0; i < pts.size(); i++) {
    const glm::dvec2 a = pts[i], b = pts[(i + 1) % pts.size()];
    const glm::dvec2 ab = b - a;
    const double len2 = glm::dot(ab, ab);
    const double t =
        len2 < 1e-18 ? 0.0 : std::clamp(glm::dot(p - a, ab) / len2, 0.0, 1.0);
    best = std::min(best, glm::length(p - (a + ab * t)));
  }
  return best;
}

} // namespace

bool IsWallFace(const std::vector<Body>& bodies, const ElementRef& ref) {
  TopoDS_Face face;
  glm::vec3 centre(0.0f), normal(0.0f);
  if (!FindFace(bodies, ref, face, centre, normal))
    return false;
  return std::abs(normal.z) < 0.01f;
}

WallContour PickedWallContour(const std::vector<Body>& bodies,
                              const ElementRef& ref, double deflection) {
  WallContour out;
  TopoDS_Face face;
  glm::vec3 centre(0.0f), normal(0.0f);
  if (!FindFace(bodies, ref, face, centre, normal)) {
    out.why = "that face is not on a body this job can machine";
    return out;
  }
  if (std::abs(normal.z) >= 0.01f) {
    out.why = "pick a wall — that face is level, so it is a floor to clear "
              "rather than a contour to follow";
    return out;
  }
  Bnd_Box box;
  BRepBndLib::Add(face, box);
  if (box.IsVoid()) {
    out.why = "that face has no extent";
    return out;
  }
  double x0 = 0, y0 = 0, z0 = 0, x1 = 0, y1 = 0, z1 = 0;
  box.Get(x0, y0, z0, x1, y1, z1);
  out.topZ = z1;
  out.bottomZ = z0;
  if (z1 - z0 < 1e-6) {
    out.why = "that wall has no height to cut";
    return out;
  }
  // Section where the wall certainly exists. Its own mid-height, not the
  // model's: a wall that stops half way up belongs to a different contour
  // than the one at the part's waist.
  const std::vector<Loop> loops =
      SectionLoops(bodies, (z0 + z1) * 0.5, deflection);
  const glm::dvec2 at(centre.x, centre.y);
  double best = std::numeric_limits<double>::max();
  const Loop* pick = nullptr;
  for (const Loop& l : loops) {
    const double d = DistanceToLoop(l.pts, at);
    if (d < best) {
      best = d;
      pick = &l;
    }
  }
  // A tolerance in millimetres, not a fraction: the wall centre lies on its
  // own contour, so anything further away is a different loop entirely.
  if (pick == nullptr || best > 0.5) {
    out.why = "that wall is not on this part's cross-section at its own "
              "height — nothing here can follow it";
    return out;
  }
  out.loop = *pick;
  out.opening = pick->hole;
  out.ok = true;
  return out;
}

DerivedOp DeriveOperation(const Setup& setup,
                          const std::vector<Body>& rawBodies, OpType type,
                          const ElementRef& target, int toolNumber) {
  // Depths are read in the frame the machine cuts in, or an indexed setup
  // derives them from the wrong axis (C9).
  const std::vector<Body> bodies = BodiesInSetupFrame(setup, rawBodies);
  DerivedOp out;
  if (setup.tools.empty()) {
    out.why = "add a tool first — every depth here comes from its diameter";
    return out;
  }
  // Every derived number below is a fraction of the cutter, so deriving for
  // one tool and cutting with another is a stepdown meant for a 6 mm end mill
  // handed to a 3 mm one.
  const Tool* named = toolNumber > 0 ? setup.FindTool(toolNumber) : nullptr;
  if (toolNumber > 0 && named == nullptr) {
    out.why =
        "there is no tool " + std::to_string(toolNumber) + " in this setup";
    return out;
  }
  const Tool& tool = named != nullptr ? *named : setup.tools.front();
  glm::dvec3 lo(0.0), hi(0.0);
  if (!StockBoundsFramed(setup, bodies, lo, hi)) {
    out.why = "there is no solid in this part yet, so there is no stock";
    return out;
  }

  Operation op;
  op.type = type;
  op.toolNumber = tool.number;
  // Half the cutter per pass and 45% of it per step-over: the rule of thumb
  // every 2.5D job starts from, expressed in the tool rather than in
  // millimetres so it stays right for a 1 mm cutter and a 12 mm one.
  op.stepdown = tool.diameter * 0.5;
  op.stepover = 0.45;

  // The model's own box, as opposed to the stock's: "does this wall go all
  // the way through" is a question about the part.
  const double modelBottom = lo.z + setup.stock.bottomMargin;

  switch (type) {
  case OpType::Pocket: {
    if (IsWallFace(bodies, target)) {
      out.why = "click the pocket's FLOOR — a wall says where to go round, "
                "not how deep to go. Cut it out follows a wall.";
      return out;
    }
    const FaceRegion region = PickedFaceRegion(bodies, target);
    if (!region.ok) {
      out.why = region.why;
      return out;
    }
    op.name = "Clear pocket";
    op.targets = {target};
    op.bottomZ = region.z;  // the floor is the face you clicked
    op.topZ = hi.z;         // …down from the top of the stock
    op.side = Side::Inside; // a pocket is cut from inside its own boundary
    // Deeper than one cutter diameter is the regime where adaptive clearing
    // pays: a constant angle of engagement lets a small spindle take the
    // whole depth, where concentric rings take the cutter's full width on
    // the first lap of every pass. Still one verb — the strategy is a
    // consequence of the geometry, not a question for the user (C8). The
    // Advanced disclosure can force either.
    if (AdaptiveAvailable() && op.topZ - op.bottomZ > tool.diameter)
      op.type = OpType::Adaptive;
    if (op.bottomZ >= op.topZ - 1e-9) {
      out.why = "that face is already the top of the stock — there is "
                "nothing above it to clear";
      return out;
    }
    break;
  }
  case OpType::Profile: {
    op.name = "Cut out";
    op.side = Side::Outside;
    op.topZ = hi.z;
    // Through the part and a whisker into the spoilboard, which is what
    // "cut it out" means — a profile that stops on the last skin leaves the
    // part welded to the stock.
    op.bottomZ = lo.z;
    if (IsWallFace(bodies, target)) {
      // A wall names the contour to follow AND how far down to follow it.
      const WallContour wall = PickedWallContour(bodies, target);
      if (!wall.ok) {
        out.why = wall.why;
        return out;
      }
      op.targets = {target};
      op.side = wall.opening ? Side::Inside : Side::Outside;
      op.name = wall.opening ? "Cut the opening" : "Cut out";
      // A wall that stops short of the bottom of the part is a step, not a
      // through cut: follow it exactly that far. One that reaches the bottom
      // is through, so it carries on into the spoilboard like any cut-out.
      if (wall.bottomZ > modelBottom + 1e-6)
        op.bottomZ = wall.bottomZ;
    } else if (target.IsSet()) {
      const FaceRegion region = PickedFaceRegion(bodies, target);
      if (!region.ok) {
        out.why = region.why;
        return out;
      }
      op.targets = {target};
    }
    // Tabs, so the part does not break free under the cutter on the last
    // pass. Sized from the tool (a tab narrower than the cutter is cut away
    // as the tool passes) and from the depth, with a floor so a shallow part
    // still gets a tab rather than a scratch. An OPENING needs none: the slug
    // that falls out of it is scrap, and tabbing it only leaves stubs to
    // clean off the part.
    if (op.side != Side::Inside) {
      op.tabCount = 4;
      op.tabWidth = tool.diameter * 2.0;
      op.tabHeight = std::max(0.5, (op.topZ - op.bottomZ) * 0.2);
    }
    break;
  }
  case OpType::Face: {
    op.name = "Face top";
    op.topZ = hi.z;
    op.bottomZ = hi.z - setup.stock.margin.z; // the part's own top
    if (setup.stock.margin.z <= 1e-9) {
      out.why = "there is no stock above the part to face — the top of the "
                "part is already the top of the stock (Stock and safety > "
                "Above the part sets how much sits above it)";
      return out;
    }
    break;
  }
  case OpType::Drill: {
    out.why = "drilling picks cylindrical faces, not a verb";
    return out;
  }
  }

  out.op = op;
  out.ok = true;
  return out;
}

std::vector<Body> BodiesInSetupFrame(const Setup& setup,
                                     const std::vector<Body>& in) {
  if (std::abs(setup.rotationA) < 1e-9)
    return in;
  // Rotate the WORLD by -A about X: the part turns under a spindle that
  // always points down, which is what an indexed trunnion does.
  const double a = glm::radians(-setup.rotationA);
  const double c = std::cos(a), s = std::sin(a);
  glm::mat4 m(1.0f);
  m[1][1] = static_cast<float>(c);
  m[1][2] = static_cast<float>(s);
  m[2][1] = static_cast<float>(-s);
  m[2][2] = static_cast<float>(c);

  std::vector<Body> out;
  out.reserve(in.size());
  for (const Body& b : in) {
    Body copy = b; // keeps createdBy and the element map: names must survive
    if (b.shape.IsValid())
      copy.shape = b.shape.Transformed(m);
    out.push_back(std::move(copy));
  }
  return out;
}

bool SolveIndexA(const glm::dvec3& normal, double& degrees) {
  const double len = glm::length(normal);
  if (len < 1e-9)
    return false;
  const glm::dvec3 n = normal / len;
  // A rotation about X cannot move the X component, so a face with one
  // cannot be brought to face the spindle.
  if (std::abs(n.x) > 1e-3)
    return false;
  // R_x(A) * n = +Z has the solution A = atan2(ny, nz): sin A = ny and
  // cos A = nz, which is consistent because ny^2 + nz^2 = 1 here.
  degrees = glm::degrees(std::atan2(n.y, n.z));
  return true;
}

double DeriveSafeZ(const Setup& setup, const std::vector<Body>& bodies) {
  glm::dvec3 lo(0.0), hi(0.0);
  if (!StockBounds(setup, bodies, lo, hi))
    return setup.safeZ;
  // Clear the stock top by a fraction of the job's own height, with a floor
  // so a thin plate still gets real clearance rather than a scrape.
  return hi.z + std::max(2.0, (hi.z - lo.z) * 0.1);
}

double CuttingMinutes(const Setup& setup, const std::vector<Toolpath>& paths) {
  double minutes = 0.0;
  for (const Toolpath& tp : paths) {
    for (size_t i = 1; i < tp.moves.size(); i++) {
      const double len = glm::length(tp.moves[i].to - tp.moves[i - 1].to);
      if (len < 1e-12)
        continue;
      double feed = tp.moves[i].feed;
      if (tp.moves[i].kind == Move::Kind::Rapid)
        feed = setup.rapidFeed; // 0 = not stated, so not counted
      if (feed > 1e-9)
        minutes += len / feed;
    }
  }
  return minutes;
}

namespace {

// The XY contour(s) a profile follows, and the region a pocket clears, when
// the operation was made by clicking a face: the face's own wires, the same
// at every depth. Empty when the operation has no target, which is the
// caller's signal to fall back to sectioning the model.
Clipper2Lib::Paths64 TargetPaths(const Operation& op,
                                 const std::vector<Body>& bodies, bool holesToo,
                                 std::string& error) {
  if (op.targets.empty())
    return {};
  // A wall names one contour at every depth — the case an opening that goes
  // all the way through has, because it has no floor to name.
  if (IsWallFace(bodies, op.targets.front())) {
    const WallContour wall = PickedWallContour(bodies, op.targets.front());
    if (!wall.ok) {
      error = wall.why;
      return {};
    }
    return Clipper2Lib::Paths64{ToClipper(wall.loop.pts)};
  }
  const FaceRegion region = PickedFaceRegion(bodies, op.targets.front());
  if (!region.ok) {
    error = region.why;
    return {};
  }
  Clipper2Lib::Paths64 outer, islands;
  for (const Loop& l : region.loops)
    (l.hole ? islands : outer).push_back(ToClipper(l.pts));
  if (!holesToo || islands.empty())
    return outer;
  return Clipper2Lib::Difference(outer, islands,
                                 Clipper2Lib::FillRule::NonZero);
}

// Profile: offset the section outward (or inward) by the tool radius and
// follow it, once per depth pass.
void GenerateProfile(const Setup& setup, const Operation& op, const Tool& tool,
                     const std::vector<Body>& bodies, Toolpath& tp) {
  const double radius = tool.diameter * 0.5;
  const double bottom = op.bottomZ + op.stockToLeave;
  const std::vector<double> passes = DepthPasses(op);
  std::string why;
  const Clipper2Lib::Paths64 picked =
      TargetPaths(op, bodies, /*holesToo=*/false, why);
  if (!why.empty()) {
    tp.error = why;
    return;
  }
  // Without a target a profile follows the part's own outline, which is what
  // "cut it out" means and is nonsense for an inside cut: offsetting the
  // perimeter INWARD by a tool radius carves a trench a whisker inside the
  // part, the full depth, all the way round. The first dogfooding round asked
  // for a slot this way and got a 15 mm gouge round the whole outline.
  if (picked.empty() && op.side == Side::Inside) {
    tp.error = "an inside profile needs the contour to cut inside of — pick "
               "the wall of the opening. Without one this would follow the "
               "part's own outline and cut a trench round it.";
    return;
  }
  // The contour of the LAST level that had one, kept so a through cut can
  // carry it down past the bottom of the model. `bottomZ` is deliberately
  // below the part — into the spoilboard — and sectioning finds nothing down
  // there, so stopping when the model runs out leaves the part welded to the
  // sheet: the exact failure tabs exist to control.
  Clipper2Lib::Paths64 lastFound;
  for (size_t pass = 0; pass < passes.size(); pass++) {
    const double z = passes[pass];
    std::vector<Loop> solid;
    if (picked.empty()) {
      // Section slightly ABOVE the pass depth: exactly at the bottom face of
      // a through-cut the section is degenerate or empty.
      const std::vector<Loop> loops = SectionLoops(bodies, z + 1e-3);
      for (const Loop& l : loops)
        if (!l.hole)
          solid.push_back(l);
    }
    const double delta = op.side == Side::Inside ? -(radius + op.stockToLeave)
                         : op.side == Side::On   ? 0.0
                                                 : radius + op.stockToLeave;
    Clipper2Lib::Paths64 contour = picked.empty() ? ToClipper(solid) : picked;
    if (contour.empty())
      contour = lastFound; // below the model: keep cutting what was there
    else
      lastFound = contour;
    if (contour.empty())
      continue; // above the model, where there has never been anything
    Clipper2Lib::Paths64 paths =
        delta == 0.0 ? contour : Offset(contour, delta);
    for (Clipper2Lib::Path64& path : paths) {
      Orient(path, op.climb, op.side == Side::Inside);
      // Tabs on EVERY pass, not only the last one. A tab is a pillar from the
      // bottom of the cut up to `tabHeight`, and a pass that ran through it
      // on the way down left only the part of it below that pass — asking for
      // a 3.8 mm tab and getting 1 mm, which is a part that breaks free under
      // the cutter and is thrown. EmitLoop lifts to max(z, tabZ), so a pass
      // that never reaches the tab top is unaffected.
      EmitLoop(tp, setup, tool, FromClipper(path), z, setup.safeZ, op.tabCount,
               op.tabWidth, bottom + op.tabHeight,
               pass == 0 ? op.topZ : passes[pass - 1]);
    }
  }
}

// Pocket: clear the voids inside the part's outline. Concentric offsets
// inward until the region closes — islands fall out of the boolean, because
// material inside a pocket is simply not part of the region.
void GeneratePocket(const Setup& setup, const Operation& op, const Tool& tool,
                    const std::vector<Body>& bodies, Toolpath& tp) {
  const double radius = tool.diameter * 0.5;
  const double step = std::max(0.05, op.stepover) * tool.diameter;
  // A clicked floor face IS the region to clear, at every depth: its outer
  // wire is the pocket wall and its inner wires are islands (docs/cam/gui.md).
  std::string why;
  const Clipper2Lib::Paths64 picked =
      TargetPaths(op, bodies, /*holesToo=*/true, why);
  if (!why.empty()) {
    tp.error = why;
    return;
  }
  double entryZ = op.topZ;
  for (const double z : DepthPasses(op)) {
    Clipper2Lib::Paths64 region = picked;
    if (region.empty()) {
      const std::vector<Loop> loops = SectionLoops(bodies, z + 1e-3);
      if (loops.empty())
        continue;
      Clipper2Lib::Paths64 outers, holes;
      for (const Loop& l : loops)
        (l.hole ? holes : outers).push_back(ToClipper(l.pts));
      if (holes.empty())
        continue; // nothing enclosed at this level
      // The void = what the outline encloses minus the material.
      region = Clipper2Lib::Difference(holes, Clipper2Lib::Paths64{},
                                       Clipper2Lib::FillRule::NonZero);
    }
    // Collect the concentric rings before emitting any: the LAST non-empty
    // inset brackets the region's inscribed radius (a region shrinks to
    // nothing exactly when the offset passes it), and that is what sizes the
    // helix. First ring sits one radius in from the wall (plus stock).
    std::vector<Clipper2Lib::Paths64> rings;
    double inset = radius + op.stockToLeave;
    double lastInset = 0.0;
    Clipper2Lib::Paths64 ring = Offset(region, -inset);
    while (!ring.empty()) {
      lastInset = inset;
      rings.push_back(std::move(ring));
      inset += step;
      ring = Offset(region, -inset);
    }
    if (rings.empty())
      continue;

    // Does the ramp fit on the loop we would cut first? If not, the descent
    // on that loop would be far steeper than asked (C6).
    double longest = 0.0;
    for (const Clipper2Lib::Path64& path : rings.front())
      longest = std::max(longest, Perimeter(FromClipper(path)));
    const double descend = std::max(0.0, entryZ - z);
    const double rampLen =
        descend > 1e-9 ? descend / std::tan(glm::radians(kRampAngleDeg)) : 0.0;
    // The tool CENTRE's circle: the inscribed radius less the cutter and the
    // stock being left. A helix narrower than a tenth of the cutter is a
    // plunge with extra steps, so that is where the fallback stays.
    const double helixRadius = lastInset - radius - op.stockToLeave;
    const bool helix = rampLen > longest && helixRadius > tool.diameter * 0.1 &&
                       descend > 1e-9;

    if (helix) {
      // The innermost ring's centroid stands in for the incentre.
      const std::vector<glm::dvec2> inner = FromClipper(rings.back().front());
      glm::dvec2 centre(0.0);
      for (const glm::dvec2& p : inner)
        centre += p;
      centre /= static_cast<double>(inner.size());
      EmitHelix(tp, tool, centre, helixRadius, std::max(entryZ, z), z,
                setup.safeZ, kDeflection);
      // Inside out: the helix cleared the middle, so each ring outward steps
      // into fresh material by one step-over. Outside-in after a helix would
      // drag the tool through uncut material to reach the wall.
      for (auto it = rings.rbegin(); it != rings.rend(); ++it)
        for (Clipper2Lib::Path64& path : *it) {
          Orient(path, op.climb, /*insideMaterial=*/true);
          EmitLoop(tp, setup, tool, FromClipper(path), z, setup.safeZ, 0, 0.0,
                   z, z, Entry::AtDepth);
        }
      Emit(tp, Move::Kind::Rapid,
           {tp.moves.back().to.x, tp.moves.back().to.y, setup.safeZ}, 0.0);
    } else {
      for (Clipper2Lib::Paths64& paths : rings)
        for (Clipper2Lib::Path64& path : paths) {
          Orient(path, op.climb, /*insideMaterial=*/true);
          EmitLoop(tp, setup, tool, FromClipper(path), z, setup.safeZ, 0, 0.0,
                   z, entryZ);
        }
    }
    entryZ = z;
  }
}

void GeneratePocket(const Setup& setup, const Operation& op, const Tool& tool,
                    const std::vector<Body>& bodies, Toolpath& tp);

// Adaptive: the same region a pocket clears, handed to libarea's Adaptive2d
// (C8). Everything about WHICH region and at what depths is shared with the
// pocket; only the clearing strategy differs, which is the point of putting
// the split here rather than in a second operation type's worth of geometry.
void GenerateAdaptive(const Setup& setup, const Operation& op, const Tool& tool,
                      const std::vector<Body>& bodies, Toolpath& tp) {
  if (!AdaptiveAvailable()) {
    // Not a refusal: the job still has to cut. The concentric pocket is the
    // strategy this build has, and saying so beats producing nothing.
    GeneratePocket(setup, op, tool, bodies, tp);
    return;
  }
  std::string why;
  const Clipper2Lib::Paths64 picked =
      TargetPaths(op, bodies, /*holesToo=*/true, why);
  if (!why.empty()) {
    tp.error = why;
    return;
  }
  double entryZ = op.topZ;
  for (const double z : DepthPasses(op)) {
    Clipper2Lib::Paths64 region = picked;
    if (region.empty()) {
      const std::vector<Loop> loops = SectionLoops(bodies, z + 1e-3);
      Clipper2Lib::Paths64 holes;
      for (const Loop& l : loops)
        if (l.hole)
          holes.push_back(ToClipper(l.pts));
      if (holes.empty())
        continue;
      region = Clipper2Lib::Difference(holes, Clipper2Lib::Paths64{},
                                       Clipper2Lib::FillRule::NonZero);
    }
    if (region.empty())
      continue;
    std::vector<Loop> asLoops;
    for (const Clipper2Lib::Path64& path : region) {
      Loop l;
      l.pts = FromClipper(path);
      l.hole = false;
      asLoops.push_back(std::move(l));
    }
    const AdaptiveResult cleared =
        AdaptiveClear(setup, op, tool, asLoops, z, entryZ);
    if (cleared.error.empty() && cleared.moves.empty() &&
        op.side == Side::Outside) {
      // Adaptive is the one clearing strategy that reads `side`, and outside
      // a region whose own boundary is the stock there is nothing to clear —
      // it returns silence rather than a complaint. The concentric pocket
      // ignores `side` entirely, so the same operation cut one way and
      // vanished the other, under the generic "produced no toolpath".
      tp.error = "this operation clears OUTSIDE its boundary, and there is "
                 "nothing out there to cut — a pocket clears inside "
                 "(side: inside)";
      return;
    }
    if (!cleared.error.empty() && cleared.moves.empty()) {
      tp.error = cleared.error;
      return;
    }
    if (!cleared.error.empty() && tp.error.empty())
      tp.error = cleared.error; // partial: say so, keep what it did cut
    tp.moves.insert(tp.moves.end(), cleared.moves.begin(), cleared.moves.end());
    entryZ = z;
  }
}

// Face: clear the stock top down to topZ over the whole stock rectangle.
void GenerateFace(const Setup& setup, const Operation& op, const Tool& tool,
                  const std::vector<Body>& bodies, Toolpath& tp) {
  glm::dvec3 lo(0.0), hi(0.0);
  if (!StockBoundsFramed(setup, bodies, lo, hi)) {
    tp.error = "no bodies, so there is no stock to face";
    return;
  }
  const double radius = tool.diameter * 0.5;
  const double step = std::max(0.05, op.stepover) * tool.diameter;
  std::vector<glm::dvec2> rect = {
      {lo.x, lo.y}, {hi.x, lo.y}, {hi.x, hi.y}, {lo.x, hi.y}};
  const Clipper2Lib::Paths64 region{ToClipper(rect)};
  double entryZ = op.topZ;
  for (const double z : DepthPasses(op)) {
    // Start with the cutter centred ON the stock edge so the outside is
    // cleaned up, then spiral inward.
    double inset = 0.0;
    Clipper2Lib::Paths64 ring = Offset(region, -inset);
    while (!ring.empty()) {
      for (Clipper2Lib::Path64& path : ring) {
        Orient(path, op.climb, /*insideMaterial=*/true);
        EmitLoop(tp, setup, tool, FromClipper(path), z, setup.safeZ, 0, 0.0, z,
                 entryZ);
      }
      inset += step;
      if (inset > glm::length(hi - lo))
        break; // guard: a degenerate stock must not spin forever
      ring = Offset(region, -inset);
    }
    entryZ = z;
    (void)radius;
  }
}

// The radius of a cylindrical face, for the one question a drill has to ask:
// is this tool the size of this hole?
bool BoreRadius(const Body& body, int idx, double& radius) {
  int i = 0;
  for (TopExp_Explorer ex(body.shape.GetShape(), TopAbs_FACE); ex.More();
       ex.Next(), i++) {
    if (i != idx)
      continue;
    BRepAdaptor_Surface surf(TopoDS::Face(ex.Current()));
    if (surf.GetType() != GeomAbs_Cylinder)
      return false;
    radius = surf.Cylinder().Radius();
    return radius > 0.0;
  }
  return false;
}

// Drill: one plunge per target hole, at the hole's axis.
void GenerateDrill(const Setup& setup, const Operation& op, const Tool& tool,
                   const std::vector<Body>& bodies, Toolpath& tp) {
  if (op.targets.empty()) {
    tp.error = "drill needs target holes; pick the cylindrical faces to drill";
    return;
  }
  int found = 0;
  for (const ElementRef& ref : op.targets) {
    for (const Body& body : bodies) {
      if (body.createdBy != ref.featureId)
        continue;
      const std::optional<int> idx = body.elementMap.Resolve(ref.name);
      if (!idx)
        continue;
      glm::vec3 origin(0.0f), axis(0.0f), xAxis(0.0f);
      if (!body.shape.FaceAxis(*idx, origin, axis, xAxis))
        continue; // not a cylinder: silently skipping would drill nothing
      // A plunge goes straight down, so a bore lying on its side is not a
      // hole this operation can make — it would be drilled somewhere the
      // part has no hole at all.
      if (std::abs(axis.z) < 0.99f) {
        tp.error = "that hole does not point at the spindle — index the setup "
                   "to bring it up, or drill it in another setup";
        return;
      }
      // The size the cutter makes IS the size of the hole. A 3 mm tool
      // plunged into a 6 mm bore leaves a 6 mm hole half cut and nothing said
      // so: the first dogfooding round posted exactly that, and it showed up
      // only as 42 mm2 of "leftover" among the tabs and the slug.
      double radius = 0.0;
      if (BoreRadius(body, *idx, radius)) {
        const double dia = radius * 2.0;
        if (tool.diameter > dia + 1e-6) {
          tp.error = "tool " + std::to_string(op.toolNumber) + " is " +
                     Round2(tool.diameter) + " mm and that hole is " +
                     Round2(dia) + " mm — it would not fit in";
          return;
        }
        if (tool.diameter < dia - 1e-3) {
          tp.error = "tool " + std::to_string(op.toolNumber) + " is " +
                     Round2(tool.diameter) + " mm and that hole is " +
                     Round2(dia) +
                     " mm — a plunge makes a hole the size of the cutter. Use "
                     "a " +
                     Round2(dia) + " mm tool, or clear the hole as a pocket.";
          return;
        }
      }
      found++;
      const glm::dvec3 centre(origin.x, origin.y, 0.0);
      Emit(tp, Move::Kind::Rapid, {centre.x, centre.y, setup.safeZ}, 0.0);
      // Down to the top of the cut at rapid, and only then at feed. Feeding
      // the whole way from Safe Z spends seconds per hole cutting air — the
      // same reason a profile rapids to the stock top before it ramps.
      if (op.topZ < setup.safeZ - 1e-9)
        Emit(tp, Move::Kind::Rapid, {centre.x, centre.y, op.topZ}, 0.0);
      Emit(tp, Move::Kind::Plunge, {centre.x, centre.y, op.bottomZ},
           tool.feedPlunge);
      Emit(tp, Move::Kind::Rapid, {centre.x, centre.y, setup.safeZ}, 0.0);
      break;
    }
  }
  if (found == 0)
    tp.error = "none of the drill targets resolved to a cylindrical face";
}

} // namespace

Toolpath GenerateFramed(const Setup& setup, const Operation& op,
                        const std::vector<Body>& bodies) {
  Toolpath tp;
  tp.operationId = op.id;
  tp.toolNumber = op.toolNumber;

  const Tool* tool = setup.FindTool(op.toolNumber);
  if (tool == nullptr) {
    tp.error = "no tool " + std::to_string(op.toolNumber) + " in this setup";
    return tp;
  }
  if (tool->diameter <= 0.0) {
    tp.error = "tool diameter must be positive";
    return tp;
  }
  if (op.bottomZ >= op.topZ) {
    tp.error = "the bottom of the cut must be below its top";
    return tp;
  }

  switch (op.type) {
  case OpType::Profile:
    GenerateProfile(setup, op, *tool, bodies, tp);
    break;
  case OpType::Pocket:
    GeneratePocket(setup, op, *tool, bodies, tp);
    break;
  case OpType::Face:
    GenerateFace(setup, op, *tool, bodies, tp);
    break;
  case OpType::Drill:
    GenerateDrill(setup, op, *tool, bodies, tp);
    break;
  case OpType::Adaptive:
    GenerateAdaptive(setup, op, *tool, bodies, tp);
    break;
  }
  if (tp.moves.empty() && tp.error.empty())
    tp.error = "this operation produced no toolpath — check its depths "
               "against the model";
  // The index this setup is cut at rides on every move, so the post emits it
  // and the machine is never asked to guess which way the part is facing.
  if (std::abs(setup.rotationA) > 1e-9)
    for (Move& m : tp.moves) {
      m.hasA = true;
      m.a = setup.rotationA;
    }
  return tp;
}

Toolpath Generate(const Setup& setup, const Operation& op,
                  const std::vector<Body>& bodies) {
  return GenerateFramed(setup, op, BodiesInSetupFrame(setup, bodies));
}

std::vector<Toolpath> GenerateAll(const Setup& setup,
                                  const std::vector<Body>& bodies) {
  // Framed ONCE for the whole setup: the transform is an OCC operation per
  // body, and doing it per operation was both slower and a place for two
  // operations to disagree.
  const std::vector<Body> framed = BodiesInSetupFrame(setup, bodies);
  std::vector<Toolpath> out;
  for (const Operation& op : setup.operations)
    if (op.enabled)
      out.push_back(GenerateFramed(setup, op, framed));
  return out;
}

} // namespace cam
