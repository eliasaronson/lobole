#include "core/cam/CamToolpath.h"

#include "core/cam/CamToolpathInternal.h"

#include "core/Measure.h"
#include "core/cam/CamAdaptive.h"

#include <clipper2/clipper.h>

#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepBndLib.hxx>
#include <BRepLProp_SLProps.hxx>
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

// Toolpath geometry: the Clipper2 bridge, loop orientation, the move
// emitters (ramp, helix, one closed loop) and the sections and depth passes
// every strategy is built from.

// One of six files split from CamToolpath.cpp
// (docs/architecture.md#source-layout).

namespace cam {

using namespace detail;

namespace {

// The largest circle that fits inside `region`, in mm.
//
// A region vanishes under an inward offset exactly when the offset passes its
// inscribed radius — the concentric pocket already relies on that to size its
// helix — so bisecting the offset measures the narrowest place without a
// medial axis. Only ever called on the failure path, so the twenty offsets
// cost nothing that matters.
double InscribedRadius(const Clipper2Lib::Paths64& region) {
  if (region.empty())
    return 0.0;
  // An upper bound: half the diagonal of the bounding box. Nothing round can
  // be wider than that.
  int64_t lox = INT64_MAX, loy = INT64_MAX, hix = INT64_MIN, hiy = INT64_MIN;
  for (const Clipper2Lib::Path64& path : region)
    for (const Clipper2Lib::Point64& p : path) {
      lox = std::min(lox, p.x);
      loy = std::min(loy, p.y);
      hix = std::max(hix, p.x);
      hiy = std::max(hiy, p.y);
    }
  if (hix <= lox || hiy <= loy)
    return 0.0;
  double lo = 0.0;
  double hi = 0.5 * std::sqrt(std::pow((hix - lox) / kScale, 2.0) +
                              std::pow((hiy - loy) / kScale, 2.0));
  for (int i = 0; i < 20; i++) {
    const double mid = 0.5 * (lo + hi);
    if (Offset(region, -mid).empty())
      hi = mid;
    else
      lo = mid;
  }
  return lo;
}

// A helix down to depth, for a loop too short to hold the ramp
// (docs/cam/operations.md#depth-passes-and-entries). Descending on the toolpath
// itself is right when the path is long; on a small pocket that lap is short
// and the descent gets Down to where a pass starts cutting, from safe height.
// ONLY the part above the stock is a rapid; below it the tool FEEDS.
//
// The old form rapided the whole way, on the reasoning (written at the ramp
// entry) that the height it enters at "is either the stock top or the depth
// the previous pass already cut here, so it is air". That holds for a closed
// contour, which returns to its own entry at full depth. It does not hold for
// a POCKET: the entry sits mid-spiral, and the pass that was meant to clear
// it was still ramping when it went past — so the rapid went through whatever
// that ramp had not yet removed. On the real baffle job that was 3 mm of G0
// into MDF, which is a broken cutter
// (docs/cam/README.md#rapids-profile-outlines-and-through-cuts).
//
// Nothing above the stock can be material, so rapiding to the stock top is
// always safe and costs nothing on the first pass, where the two are equal.
// Where a pass should BEGIN its descent, given where it was asked to. Never
// below the stock top: above that there can be no material, so the ramp then
// cuts its way in instead of arriving already buried.
//
// The entry used to start at the height it was asked for, on the reasoning
// (written at the ramp entry) that that height "is either the stock top or
// the depth the previous pass already cut here, so it is air". True of a
// closed contour, which returns to its own entry at full depth. False of a
// POCKET, whose entry sits mid-spiral where the previous pass was still
// ramping, and false of any pass told to start below the stock top — a
// finishing skim after a roughing pass. On the real baffle job that was 3 mm
// of G0 into MDF (docs/cam/README.md#rapids-profile-outlines-and-through-cuts).
//
// Raising the start lengthens the descent, which the ramp already bounds by
// the loop's own perimeter — steeper than asked, still never a plunge, and a
// profile must not plunge at all: a non-centre-cutting end mill rubs.
// How far a pass may RAPID down before it starts cutting. Never below the
// stock top: above that there can be no material. The descent from there to
// the entry height is left to the ramp, whose moves are feeds.
double RapidFloor(double entry, double stockTop, double safeZ) {
  return std::clamp(std::max(entry, stockTop), entry, safeZ);
}

} // namespace

namespace detail {

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

// Offsetting is signed against the winding: a positive delta grows a
// positively wound path and shrinks a negatively wound one. Callers say what
// they mean with the sign, so every contour handed to `Offset` arrives in one
// convention — Clipper hands the holes of a union back wound the other way,
// and an opening IS a hole.
void MakeCounterClockwise(Loop& l) {
  double a2 = 0.0;
  for (size_t i = 0; i < l.pts.size(); i++) {
    const glm::dvec2& a = l.pts[i];
    const glm::dvec2& b = l.pts[(i + 1) % l.pts.size()];
    a2 += a.x * b.y - b.x * a.y;
  }
  if (a2 < 0.0)
    std::reverse(l.pts.begin(), l.pts.end());
}

double Perimeter(const std::vector<glm::dvec2>& pts) {
  double total = 0.0;
  for (size_t i = 0; i + 1 < pts.size(); i++)
    total += glm::length(pts[i + 1] - pts[i]);
  if (pts.size() > 2)
    total += glm::length(pts.front() - pts.back());
  return total;
}

// A millimetre figure for a message. std::to_string on a double gives
// "3.000000", which reads as machine noise in a sentence meant for a person.
std::string Round2(double mm) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%g", std::round(mm * 100.0) / 100.0);
  return buf;
}

// Why the cutter could not get into `region`, in the words a person can act
// on: how wide the place actually is, how wide this tool is, and what would
// fit. Empty when the tool DOES fit, so the caller keeps looking for the real
// reason rather than blaming the tool for something else.
//
// This exists because the generic "produced no toolpath — check its depths"
// blamed the depths for a job whose depths were right and whose cutter was
// too fat, which is the one failure a beginner hits first and the one they
// have no way to diagnose.
std::string TooTightForTool(const Clipper2Lib::Paths64& region,
                            const Tool& tool, double stockToLeave,
                            const char* what) {
  const double across = InscribedRadius(region) * 2.0;
  if (across <= 1e-9)
    return {};
  const double needs = tool.diameter + 2.0 * stockToLeave;
  if (across >= needs - 1e-6)
    return {}; // it fits; something else is wrong
  const std::string head = std::string("this ") + what + " is " +
                           Round2(across) + " mm across at its narrowest and " +
                           "tool " + std::to_string(tool.number) + " is " +
                           Round2(tool.diameter) + " mm";
  // Two different problems with two different fixes: a cutter that cannot fit
  // at all, and one that fits but not with the stock being left on.
  if (across < tool.diameter - 1e-6)
    return head + " — it cannot reach in. Use a cutter of " + Round2(across) +
           " mm or less.";
  return head + ", but leaving " + Round2(stockToLeave) +
         " mm of stock needs " + Round2(needs) +
         " mm of room. Leave less stock, or use a smaller cutter.";
}

void Emit(Toolpath& tp, Move::Kind kind, const glm::dvec3& to, double feed) {
  Move m;
  m.kind = kind;
  m.to = to;
  m.feed = feed;
  tp.moves.push_back(m);
}

// Rotate a closed loop so it begins at the point nearest `from`. A ring cut
// straight after its neighbour should start where the tool already is, not at
// whatever point the offsetter happened to emit first — otherwise "link at
// depth" drags the cutter most of the way round the ring to get started.
std::vector<glm::dvec2> StartNearest(const std::vector<glm::dvec2>& pts,
                                     const glm::dvec2& from) {
  if (pts.size() < 2)
    return pts;
  size_t best = 0;
  double bestD = std::numeric_limits<double>::max();
  for (size_t i = 0; i < pts.size(); i++) {
    const double d = glm::length(pts[i] - from);
    if (d < bestD) {
      bestD = d;
      best = i;
    }
  }
  std::vector<glm::dvec2> out;
  out.reserve(pts.size());
  for (size_t i = 0; i < pts.size(); i++)
    out.push_back(pts[(best + i) % pts.size()]);
  return out;
}

// steep, so instead the tool spirals down inside the region and the rings
// are then cut from the middle outwards.
//
// The circle it walks is the TOOL CENTRE's, so `radius` must already have the
// cutter's own radius taken out of it — a helix sized from the wall instead
// of the centre is a cut through the wall.
void EmitHelix(Toolpath& tp, const Tool& tool, const glm::dvec2& centre,
               double radius, double fromZ, double toZ, double safeZ,
               double stockTop, double deflection) {
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
  Emit(tp, Move::Kind::Rapid,
       {start.x, start.y, RapidFloor(fromZ, stockTop, safeZ)}, 0.0);
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

// Cuts one closed loop at depth `z`, entering by ramp (or straight in, at
// depth) and returns WHERE IT FINISHED as an arc length along the loop.
//
// `startS` is where to begin, and the return value is what to pass as the
// next depth pass's `startS`. That chaining is the whole point: the lap
// deliberately overshoots the ramp so the ramped-down stretch still gets cut
// at full depth, so every pass ends `rampLen` past where it began — and a
// pass that then restarts at pts[0] has to lift, fly back ACROSS the opening
// at safe height, and drop again, once per pass. Starting where the last one
// stopped puts the lift and the drop at the same point instead, which is no
// travel at all.
// `leadOut` > 0: leave the wall on a tangent arc of that radius (R5) before
// retracting, curving AWAY from the material -- `materialInside` says which
// side of the loop that is. A tool that stops against the wall and lifts
// leaves a mark where it dwelt; one that arcs off does not.
double EmitLoop(Toolpath& tp, const Setup& setup, const Tool& tool,
                const std::vector<glm::dvec2>& pts, double z, double safeZ,
                double stockTop, int tabCount, double tabWidth, double tabZ,
                double entryZ, Entry entry, double startS, bool retract,
                double leadOut, bool materialInside,
                std::vector<Toolpath::Tab>* tabsOut) {
  if (pts.size() < 2)
    return startS;
  const double perimeter = Perimeter(pts);
  if (perimeter < 1e-9)
    return startS;

  if (entry == Entry::AtDepth) {
    // Straight to the start of this loop at depth. The step from the
    // previous ring is at most one step-over through material that ring
    // already cleared.
    Emit(tp, Move::Kind::Cut, {pts[0].x, pts[0].y, z}, tool.feedCut);
    std::vector<glm::dvec2> lap(pts.begin(), pts.end());
    lap.push_back(pts[0]);
    for (size_t i = 1; i < lap.size(); i++)
      Emit(tp, Move::Kind::Cut, {lap[i].x, lap[i].y, z}, tool.feedCut);
    if (retract)
      Emit(tp, Move::Kind::Rapid, {pts[0].x, pts[0].y, safeZ}, 0.0);
    return startS;
  }

  // Where along the loop this pass begins. Everything below measures arc
  // length from pts[0] so tabs stay put; `s` is the distance travelled by
  // THIS pass, `startS + s` is where that lands on the contour.
  const auto wrapS = [&](double v) {
    return std::fmod(std::fmod(v, perimeter) + perimeter, perimeter);
  };
  startS = wrapS(startS);
  const auto at = [&](const std::vector<glm::dvec2>& poly, double arc) {
    double m = std::fmod(std::fmod(arc, perimeter) + perimeter, perimeter);
    for (size_t i = 0; i < poly.size(); i++) {
      const glm::dvec2& a = poly[i];
      const glm::dvec2& b = poly[(i + 1) % poly.size()];
      const double seg = glm::length(b - a);
      if (m <= seg || i + 1 == poly.size())
        return seg < 1e-12 ? a : a + (b - a) * std::clamp(m / seg, 0.0, 1.0);
      m -= seg;
    }
    return poly[0];
  };
  const glm::dvec2 begin = at(pts, startS);

  // Rapid over, then down to the height we enter from: that is either the
  // stock top or the depth the previous pass already cut here, so it is air.
  // When the previous pass handed us its finishing point this is the SAME
  // point it retracted at, so the two rapids are a vertical hop rather than a
  // flight back across the opening.
  // STRAIGHT UP first if the tool is still down. Laps no longer retract at
  // their end (so a ring can link to the next at depth), which means the
  // lateral rapid below can start from the bottom of a cut — and a rapid that
  // climbs and travels at once travels through whatever is in the way.
  if (!tp.moves.empty() && tp.moves.back().to.z < safeZ - 1e-9)
    Emit(tp, Move::Kind::Rapid,
         {tp.moves.back().to.x, tp.moves.back().to.y, safeZ}, 0.0);
  Emit(tp, Move::Kind::Rapid, {begin.x, begin.y, safeZ}, 0.0);
  const double from = std::max(entryZ, z);
  Emit(tp, Move::Kind::Rapid,
       {begin.x, begin.y, RapidFloor(from, stockTop, safeZ)}, 0.0);

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
  if (tabsOut != nullptr)
    for (const auto& [lo, hi] : tabs)
      tabsOut->push_back({at(pts, (lo + hi) * 0.5),
                          tabWidth * 0.5 + tool.diameter * 0.5, tabZ});
  const auto tabTop = [&](double s) {
    const double m = wrap(startS + s);
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
  const auto pointAt = [&](double s) { return at(pts, startS + s); };

  // A full lap AFTER the ramp finishes, so every millimetre of the contour is
  // cut at depth even though the descent used most of one.
  const double total = rampLen + perimeter;
  std::vector<double> stations;
  stations.reserve(pts.size() * 2 + tabs.size() * 4 + 4);
  {
    // Every place worth stopping at is an ABSOLUTE arc length from pts[0] —
    // the contour's own vertices, and the tab edges. This pass starts at
    // `startS`, so each one is reached after `absolute - startS` mm of
    // travel, and again a perimeter later because the lap overshoots the
    // ramp. Measuring them from pts[0] while walking from startS is what
    // rounds every corner off and moves the tabs (they came out 6 mm tall
    // instead of 2 mm, which is the test that caught it).
    const auto station = [&](double absolute) {
      for (double a = wrapS(absolute - startS); a <= total + 1e-9;
           a += perimeter)
        if (a > 1e-9)
          stations.push_back(a);
    };
    double acc = 0.0;
    for (size_t i = 0; i < pts.size(); i++) {
      acc += glm::length(pts[(i + 1) % pts.size()] - pts[i]);
      station(acc);
    }
    for (const auto& [lo, hi] : tabs)
      for (const double edge : {lo, hi})
        station(edge);
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
    Emit(tp, Move::Kind::Plunge, {begin.x, begin.y, std::max(z, tabTop(0.0))},
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
      // The vertical move at a tab edge. Coming DOWN off a tab is a plunge:
      // the cutter is buried and feeding straight into the material, which
      // is the one thing the tool's plunge feed exists to govern -- it went
      // in at the CUTTING rate, so a 3 mm tab exit ran 1200 mm/min straight
      // down. Going up is a retract, where the cutting rate is right.
      const bool descending = zStart < currentZ;
      Emit(tp, descending ? Move::Kind::Plunge : Move::Kind::Cut,
           {a.x, a.y, zStart}, descending ? tool.feedPlunge : tool.feedCut);
      currentZ = zStart;
    }
    Emit(tp, Move::Kind::Cut, {b.x, b.y, zEnd},
         s <= rampLen + 1e-9 ? tool.feedPlunge : tool.feedCut);
    currentZ = zEnd;
    prev = s;
  }
  glm::dvec2 last = pointAt(total);
  if (leadOut > 1e-9) {
    // The direction the lap ends in, and the normal away from the material:
    // the loop's outward normal when the part is inside it, inward when it
    // is not. A quarter arc, tangent at the wall, ending a radius off it.
    const glm::dvec2 before = pointAt(std::max(0.0, total - 1e-3));
    const glm::dvec2 d = glm::length(last - before) > 1e-12
                             ? glm::normalize(last - before)
                             : glm::dvec2(1.0, 0.0);
    double area2 = 0.0;
    for (size_t i = 0; i < pts.size(); i++) {
      const glm::dvec2& a = pts[i];
      const glm::dvec2& b = pts[(i + 1) % pts.size()];
      area2 += a.x * b.y - b.x * a.y;
    }
    const glm::dvec2 outward =
        area2 > 0.0 ? glm::dvec2(d.y, -d.x) : glm::dvec2(-d.y, d.x);
    const glm::dvec2 away = materialInside ? outward : -outward;
    const glm::dvec2 centre = last + away * leadOut;
    constexpr int kArcSegments = 8;
    for (int i = 1; i <= kArcSegments; i++) {
      const double t = glm::half_pi<double>() * i / kArcSegments;
      const glm::dvec2 p =
          centre - away * (leadOut * std::cos(t)) + d * (leadOut * std::sin(t));
      Emit(tp, Move::Kind::Cut, {p.x, p.y, currentZ}, tool.feedCut);
    }
    last = glm::dvec2(tp.moves.back().to.x, tp.moves.back().to.y);
  }
  // Left DOWN when the caller says another loop follows it: a concentric
  // clearing links ring to ring at depth, and it cannot if every lap ends by
  // retracting. The caller retracts once when the run is done.
  if (retract)
    Emit(tp, Move::Kind::Rapid, {last.x, last.y, safeZ}, 0.0);
  return wrapS(startS + total);
}

} // namespace detail

double Toolpath::CutLength() const {
  double total = 0.0;
  for (size_t i = 1; i < moves.size(); i++)
    // Plunges too. A plunge IS cutting — `CuttingMinutes` has always counted
    // one, reading the feed off the move — so leaving it out here made a job
    // of drilled holes report 0 mm of cutting and a real number of minutes.
    if (moves[i].kind == Move::Kind::Cut || moves[i].kind == Move::Kind::Plunge)
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

std::vector<std::vector<Loop>>
ProjectedOutlines(const std::vector<Body>& bodies, double topZ,
                  const std::vector<double>& passes, double deflection) {
  // The material at one height as a REGION rather than a bag of wires: an
  // even-odd union over every loop of the section, because what counts as
  // inside is a question of nesting (inside the part is filled, inside an
  // opening is not, inside a boss standing in that opening is again).
  const auto materialAt = [&](double z) {
    Clipper2Lib::Paths64 section;
    for (const Loop& l : SectionLoops(bodies, z, deflection))
      if (l.pts.size() >= 3)
        section.push_back(ToClipper(l.pts));
    return Clipper2Lib::Union(section, Clipper2Lib::FillRule::EvenOdd);
  };
  // A section flush with a face is degenerate or empty, so every sample is
  // taken a hair inside the interval it stands for.
  constexpr double kHair = 1e-3;
  // The operation's own top belongs to every column beneath it: the first
  // pass sweeps the material between topZ and itself, so that material
  // constrains where it may go.
  Clipper2Lib::Paths64 acc = materialAt(topZ - kHair);
  std::vector<std::vector<Loop>> out;
  out.reserve(passes.size());
  for (const double z : passes) {
    Clipper2Lib::Paths64 both = acc;
    for (const Clipper2Lib::Path64& p : materialAt(z + kHair))
      both.push_back(p);
    // Below the model this adds nothing and `acc` stands, which is what a
    // through cut needs: `bottomZ` is deliberately into the spoilboard, and a
    // pass that found nothing must keep cutting the outline that was there
    // rather than leave the part welded to the sheet.
    acc = Clipper2Lib::Union(both, Clipper2Lib::FillRule::NonZero);
    std::vector<Loop> loops;
    loops.reserve(acc.size());
    for (const Clipper2Lib::Path64& p : acc) {
      Loop l;
      l.pts = FromClipper(p);
      if (l.pts.size() < 3)
        continue;
      l.hole = !Clipper2Lib::IsPositive(p); // Clipper winds outers positive
      loops.push_back(std::move(l));
    }
    out.push_back(std::move(loops));
  }
  return out;
}

uint64_t OutlineKey(uint64_t bodiesKey, double topZ,
                    const std::vector<double>& passes, double deflection) {
  // Every argument ProjectedOutlines takes, and nothing else: the function
  // is pure, so this is complete by construction and needs no gate.
  uint64_t h = 1469598103934665603ULL;
  const auto bytes = [&h](const void* p, size_t n) {
    const unsigned char* c = static_cast<const unsigned char*>(p);
    for (size_t i = 0; i < n; i++) {
      h ^= c[i];
      h *= 1099511628211ULL;
    }
  };
  bytes(&bodiesKey, sizeof bodiesKey);
  bytes(&topZ, sizeof topZ);
  bytes(&deflection, sizeof deflection);
  const uint64_t count = passes.size();
  bytes(&count, sizeof count);
  for (const double z : passes)
    bytes(&z, sizeof z);
  return h;
}

OutlineResult OutlinePass(const OutlineRequest& request) {
  // The worker's form. Thread-safe for the reason the surfacing pass is:
  // the sections go through Measure::SectionCaps, whose booleans are built
  // non-destructive (BuildQuery), so nothing is written onto a shape the
  // frame may be reading.
  OutlineResult out;
  out.outlines = ProjectedOutlines(request.bodies, request.topZ, request.passes,
                                   request.deflection);
  return out;
}

const OutlineResult* OutlineCache::Find(uint64_t key) {
  const auto it = m_entries.find(key);
  if (it == m_entries.end()) {
    misses++;
    return nullptr;
  }
  hits++;
  return &it->second;
}

bool OutlineCache::Contains(uint64_t key) const {
  return m_entries.find(key) != m_entries.end();
}

void OutlineCache::Put(uint64_t key, OutlineResult result) {
  if (const auto it = m_entries.find(key); it != m_entries.end()) {
    it->second = std::move(result);
    return;
  }
  m_entries.emplace(key, std::move(result));
  m_order.push_back(key);
  while (m_entries.size() > kMaxEntries) {
    m_entries.erase(m_order.front());
    m_order.pop_front();
  }
}

void OutlineCache::Clear() {
  m_entries.clear();
  m_order.clear();
  hits = 0;
  misses = 0;
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

} // namespace cam
