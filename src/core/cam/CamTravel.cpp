#include "core/cam/CamNames.h"
#include "core/cam/CamToolpath.h"

#include "core/cam/CamToolpathInternal.h"

#include "core/Measure.h"

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

// The machine's limits: where parts sit in the sheet, what order they are
// cut in, the reach window, and clipping a path to the travel.

// One of six files split from CamToolpath.cpp
// (docs/architecture.md#source-layout).

namespace cam {

using namespace detail;

namespace {

// The XY footprint of one placed part, in setup space.
struct Foot {
  bool ok = false;
  glm::dvec3 lo{0.0}, hi{0.0};
};

Foot FootOf(const Setup& setup, const std::vector<Body>& framed,
            uint64_t jobPartId) {
  std::vector<Body> mine;
  for (const Body& b : framed)
    if (b.jobPartId == jobPartId)
      mine.push_back(b);
  (void)setup;
  Foot f;
  f.ok = ModelBoundsFramed(mine, f.lo, f.hi);
  return f;
}

bool OverlapsXY(const Foot& a, const Foot& b) {
  const double eps = 1e-6;
  return a.lo.x < b.hi.x - eps && b.lo.x < a.hi.x - eps &&
         a.lo.y < b.hi.y - eps && b.lo.y < a.hi.y - eps;
}

// The placed, enabled parts' box in setup space; false when there are none.
bool PlacedPartsBounds(const Setup& setup, const std::vector<Body>& bodies,
                       glm::dvec3& lo, glm::dvec3& hi) {
  lo = glm::dvec3(std::numeric_limits<double>::max());
  hi = glm::dvec3(std::numeric_limits<double>::lowest());
  bool any = false;
  for (const Body& b : BodiesInSetupFrame(setup, bodies)) {
    glm::vec3 bmin, bmax;
    // TIGHT, like the stock. OCC bounds a B-spline by its CONTROL POINTS,
    // which lie outside the surface -- so this asked the same part for its
    // size and got a different answer from `StockBoundsFramed`, which was
    // switched to the tight bound when a 200 mm waveguide asked for a 216 mm
    // blank. The rotary handle is what made the gap impossible to miss: a
    // part 38 mm across reported as needing 310 mm of Z travel, and this is
    // the check that tells an operator their job does not fit the machine.
    // A warning that wrong is a warning nobody reads.
    b.shape.GetBoundingBox(bmin, bmax, /*tight=*/true);
    lo = glm::min(lo, glm::dvec3(bmin));
    hi = glm::max(hi, glm::dvec3(bmax));
    any = true;
  }
  return any;
}

} // namespace

std::vector<PlacementFit> CheckPlacements(const Setup& setup,
                                          const std::vector<Body>& bodies) {
  std::vector<PlacementFit> out;
  if (setup.placements.empty())
    return out;
  const std::vector<Body> framed = BodiesInSetupFrame(setup, bodies);
  glm::dvec3 stockLo(0.0), stockHi(0.0);
  const bool haveStock = StockBoundsFramed(setup, framed, stockLo, stockHi);

  std::vector<Foot> feet;
  feet.reserve(setup.placements.size());
  for (const Placement& p : setup.placements)
    feet.push_back(FootOf(setup, framed, p.jobPartId));

  for (size_t i = 0; i < setup.placements.size(); i++) {
    const Placement& p = setup.placements[i];
    PlacementFit fit;
    fit.placementId = p.id;
    fit.jobPartId = p.jobPartId;
    // A disabled placement is not cut, so it is not in anyone's way and
    // nothing is asked of it.
    if (!p.enabled || !feet[i].ok) {
      out.push_back(std::move(fit));
      continue;
    }
    if (haveStock && setup.stock.mode == StockMode::Fixed) {
      const bool anyIn = feet[i].lo.x < stockHi.x && stockLo.x < feet[i].hi.x &&
                         feet[i].lo.y < stockHi.y && stockLo.y < feet[i].hi.y;
      const bool allIn = feet[i].lo.x >= stockLo.x - kStockFit &&
                         feet[i].hi.x <= stockHi.x + kStockFit &&
                         feet[i].lo.y >= stockLo.y - kStockFit &&
                         feet[i].hi.y <= stockHi.y + kStockFit;
      fit.fit = allIn   ? Fit::Inside
                : anyIn ? Fit::PartlyOutside
                        : Fit::FullyOutside;
    }
    for (size_t k = 0; k < setup.placements.size(); k++) {
      if (k == i || !setup.placements[k].enabled || !feet[k].ok)
        continue;
      if (OverlapsXY(feet[i], feet[k]))
        fit.overlaps.push_back(setup.placements[k].id);
    }
    fit.lo = feet[i].lo;
    fit.hi = feet[i].hi;
    out.push_back(std::move(fit));
  }
  return out;
}

std::vector<std::string> LayoutWarnings(const Setup& setup,
                                        const std::vector<Body>& bodies) {
  std::vector<std::string> out;
  double widest = 0.0;
  std::string widestName;
  for (const Tool& t : setup.tools)
    if (t.diameter > widest) {
      widest = t.diameter;
      widestName = t.name;
    }
  if (widest <= 0.0)
    return out;
  const std::vector<PlacementFit> fits = CheckPlacements(setup, bodies);
  for (size_t i = 0; i < fits.size(); i++) {
    for (size_t k = i + 1; k < fits.size(); k++) {
      // Only the pairs that are beside each other: two parts that overlap in
      // NEITHER axis are diagonal neighbours, and nothing cuts between them.
      const bool sameRow =
          fits[i].lo.y < fits[k].hi.y && fits[k].lo.y < fits[i].hi.y;
      const bool sameColumn =
          fits[i].lo.x < fits[k].hi.x && fits[k].lo.x < fits[i].hi.x;
      double gap = 0.0;
      if (sameRow && !sameColumn)
        gap =
            std::max(fits[i].lo.x - fits[k].hi.x, fits[k].lo.x - fits[i].hi.x);
      else if (sameColumn && !sameRow)
        gap =
            std::max(fits[i].lo.y - fits[k].hi.y, fits[k].lo.y - fits[i].hi.y);
      else
        continue; // overlapping (already a refusal) or diagonal
      if (gap >= widest - 1e-9)
        continue;
      const std::string which =
          widestName.empty() ? std::string() : " (" + widestName + ")";
      char buf[280];
      std::snprintf(buf, sizeof(buf),
                    "two parts are %.4g mm apart and the widest cutter here "
                    "is %.4g mm%s: cutting one out will cut into the other. "
                    "Move them apart, or cut them in separate setups",
                    gap, widest, which.c_str());
      out.push_back(buf);
    }
  }
  return out;
}

namespace {
// Tools in the order they cut: as they first appear in `appearance`, then
// every tool a REST pass uses moved after the tool it rests from. A rest pass
// cleans up after a finish, and run before it -- a 3 mm ball numbered T2
// ahead of the 6 mm finish it follows as T3 -- it cut 14 m of the finish's
// work.
std::vector<int> ToolRank(const Setup& setup, std::vector<int> appearance) {
  for (const Operation& op : setup.operations) {
    if (!op.enabled || op.restFromTool == 0)
      continue;
    const auto rest =
        std::find(appearance.begin(), appearance.end(), op.toolNumber);
    const auto from =
        std::find(appearance.begin(), appearance.end(), op.restFromTool);
    if (rest == appearance.end() || from == appearance.end() || rest > from)
      continue;
    const int t = *rest;
    appearance.erase(rest);
    appearance.insert(
        std::find(appearance.begin(), appearance.end(), op.restFromTool) + 1,
        t);
  }
  return appearance;
}
} // namespace

std::vector<size_t> CuttingOrder(const Setup& setup) {
  std::vector<size_t> idx;
  idx.reserve(setup.operations.size());
  for (size_t i = 0; i < setup.operations.size(); i++)
    idx.push_back(i);
  // A file per tool IS tool order: the flag overrides the setting rather
  // than contradicting it (R2).
  const OrderOutputBy order =
      setup.splitOutput ? OrderOutputBy::Tool : setup.order;
  if (order == OrderOutputBy::Authored)
    return idx;

  // The key each operation groups by. An operation with no target belongs to
  // no part -- facing the stock is about the workpiece, not about any one
  // thing on it -- and 0 sorts it first, which is where a facing pass goes
  // anyway.
  const auto key = [&](size_t i) -> uint64_t {
    const Operation& op = setup.operations[i];
    if (order == OrderOutputBy::Tool)
      return static_cast<uint64_t>(op.toolNumber);
    return op.targets.empty() ? 0 : op.targets.front().jobPartId;
  };
  // Group in FIRST-APPEARANCE order rather than by sorting the key: a tool
  // table is not ordered by how early each cutter is wanted, and reordering
  // to 1,2,3 would move a roughing pass after a finishing one that happens
  // to use a lower number.
  std::vector<uint64_t> seen;
  std::vector<size_t> out;
  out.reserve(idx.size());
  for (size_t i : idx) {
    const uint64_t k = key(i);
    if (std::find(seen.begin(), seen.end(), k) == seen.end())
      seen.push_back(k);
  }
  if (order == OrderOutputBy::Tool) {
    std::vector<int> tools(seen.begin(), seen.end());
    tools = ToolRank(setup, tools);
    seen.assign(tools.begin(), tools.end());
  }
  for (uint64_t k : seen)
    for (size_t i : idx)
      if (key(i) == k)
        out.push_back(i);
  return out;
}

DerivedReach DeriveReachOffset(const Setup& setup,
                               const std::vector<Body>& bodies) {
  DerivedReach out;
  const Machine& m = setup.machine;
  if (!m.HasWorkArea()) {
    out.why = "this setup's machine states no work area, so there is no "
              "window to place";
    return out;
  }
  const std::vector<Body> framed = BodiesInSetupFrame(setup, bodies);
  glm::dvec3 slo(0.0), shi(0.0);
  if (!StockBoundsFramed(setup, framed, slo, shi)) {
    out.why = "there is no stock to measure the sheet against";
    return out;
  }
#ifndef HAVE_CAM
  // Where the window goes is decided by what the job actually CUTS, and this
  // build has no toolpath generator to ask. Refusing says so; guessing from
  // the stock would place the window over a sheet that is mostly overhang.
  out.why = "this build cannot generate toolpaths, so there is nothing to "
            "measure the reach window against";
  return out;
#else
  // What is actually CUT, not what the sheet is: the window has to cover the
  // toolpaths, and on an overhanging sheet those are a fraction of it.
  Setup probe = setup;
  probe.machine = Machine{}; // no work area: generate unclipped
  glm::dvec2 lo(std::numeric_limits<double>::max());
  glm::dvec2 hi(std::numeric_limits<double>::lowest());
  bool any = false;
  for (const Toolpath& tp : GenerateAll(probe, bodies)) {
    for (const Move& mv : tp.moves) {
      lo = glm::min(lo, glm::dvec2(mv.to));
      hi = glm::max(hi, glm::dvec2(mv.to));
      any = true;
    }
  }
  if (!any) {
    out.why = "this setup cuts nothing yet, so there is nowhere to put the "
              "window";
    return out;
  }
  const glm::dvec2 travel(m.travelMax.x - m.travelMin.x,
                          m.travelMax.y - m.travelMin.y);
  const char* axes[2] = {"X", "Y"};
  for (int a = 0; a < 2; a++) {
    const double need = hi[a] - lo[a];
    if (need > travel[a] + 1e-6) {
      char buf[220];
      std::snprintf(buf, sizeof(buf),
                    "the cutting itself spans %.0f mm in %s and %s travels "
                    "%.0f: no position of the sheet brings it all within "
                    "reach, so this is two setups",
                    need, axes[a],
                    m.name.empty() ? "this machine" : m.name.c_str(),
                    travel[a]);
      out.why = buf;
      return out;
    }
    // A stock that fits the travel is centred in it already
    // (MachineBoundsFramed), with room round every cut: nothing to move.
    if (shi[a] - slo[a] <= travel[a] + 1e-6) {
      out.offset[a] = 0.0;
      continue;
    }
    // A longer sheet: centre the window on the cutting, then keep it on the
    // sheet's near side -- a window starting before the stock is a machine
    // position no sheet reaches.
    const double centred = (lo[a] + hi[a]) * 0.5 - travel[a] * 0.5;
    out.offset[a] = std::max(0.0, centred - slo[a]);
  }
  out.ok = true;
  return out;
#endif // HAVE_CAM
}

std::string MachineWarning(const Setup& setup,
                           const std::vector<Body>& bodies) {
  const Machine& m = setup.machine;
  if (!m.HasWorkArea())
    return "";
  glm::dvec3 lo(0.0), hi(0.0);
  if (!PlacedPartsBounds(setup, bodies, lo, hi))
    return "";
  const char* axes[3] = {"X", "Y", "Z"};
  for (int a = 0; a < 3; a++) {
    const double need = hi[a] - lo[a];
    const double have = m.travelMax[a] - m.travelMin[a];
    if (need > have + 1e-6) {
      char buf[260];
      std::snprintf(buf, sizeof(buf),
                    "the parts need %.0f mm in %s and %s travels %.0f: the "
                    "last %.0f mm are beyond the machine. Say where the bed "
                    "is on the sheet (reach_offset) if the cutting itself "
                    "fits; otherwise it is a second setup with the sheet "
                    "moved along",
                    need, axes[a],
                    m.name.empty() ? "this machine" : m.name.c_str(), have,
                    need - have);
      return buf;
    }
  }
  return "";
}

bool MachineBoundsFramed(const Setup& setup, const std::vector<Body>& framed,
                         glm::dvec3& lo, glm::dvec3& hi) {
  const Machine& m = setup.machine;
  if (!m.HasWorkArea())
    return false;
  glm::dvec3 slo(0.0), shi(0.0);
  if (!StockBoundsFramed(setup, framed, slo, shi))
    return false;
  // Where the bed sits ON the sheet (docs/cam/setup.md#the-machines-work-area).
  // Per axis: a stock that FITS the travel is centred in it, so every cutter
  // has room on both sides of it -- anchored at the stock's near corner, an
  // outline cutter running a radius outside the stock was outside the window
  // on the near sides, and half the sampler part's outline was clipped out
  // of the file. A stock LONGER than the travel is anchored at its near
  // corner, and `reach_offset` says where along it the bed is: the case it
  // exists for. On either, `reach_offset` moves the window from there.
  const glm::dvec3 travel = m.travelMax - m.travelMin;
  lo = slo;
  for (int a = 0; a < 2; a++)
    if (shi[a] - slo[a] <= travel[a] + 1e-6)
      lo[a] = (slo[a] + shi[a]) * 0.5 - travel[a] * 0.5;
  lo += glm::dvec3(setup.reachOffset, 0.0);
  hi = lo + travel;
  return true;
}

bool MachineBounds(const Setup& setup, const std::vector<Body>& bodies,
                   glm::dvec3& lo, glm::dvec3& hi) {
  return MachineBoundsFramed(setup, BodiesInSetupFrame(setup, bodies), lo, hi);
}

void ClipToMachine(Toolpath& tp, const glm::dvec3& lo, const glm::dvec3& hi,
                   double safeZ, double plungeFeed) {
  if (tp.moves.empty())
    return;
  constexpr double kEps = 1e-6;
  // Liang-Barsky in XY: the parameter range of a->b that lies in the box.
  const auto clip = [&](const glm::dvec3& a, const glm::dvec3& b, double& t0,
                        double& t1) {
    t0 = 0.0;
    t1 = 1.0;
    const double d[2] = {b.x - a.x, b.y - a.y};
    const double p0[2] = {a.x, a.y};
    const double bl[2] = {lo.x - kEps, lo.y - kEps};
    const double bh[2] = {hi.x + kEps, hi.y + kEps};
    for (int ax = 0; ax < 2; ax++) {
      if (std::abs(d[ax]) < 1e-12) {
        if (p0[ax] < bl[ax] || p0[ax] > bh[ax])
          return false;
        continue;
      }
      double tl = (bl[ax] - p0[ax]) / d[ax], th = (bh[ax] - p0[ax]) / d[ax];
      if (tl > th)
        std::swap(tl, th);
      t0 = std::max(t0, tl);
      t1 = std::min(t1, th);
      if (t0 > t1)
        return false;
    }
    return true;
  };
  const auto lerp = [](const glm::dvec3& a, const glm::dvec3& b, double t) {
    return a + (b - a) * t;
  };
  const auto cutting = [](const Move& m) {
    return m.kind == Move::Kind::Cut || m.kind == Move::Kind::Plunge;
  };

  std::vector<Move> out;
  out.reserve(tp.moves.size() + 16);
  double clipped = 0.0;
  bool outside = false; // the tool is off the bed, parked at safeZ
  // An inserted move INHERITS the rotary index of the move it came from. It
  // used to be built from scratch, so a retract or a re-entry arrived with
  // `hasA = false` in the middle of an indexed path where every other move
  // carried one. Harmless in the FILE -- A is modal and the machine holds the
  // last one -- and not harmless in the toolpath: `LastCollinear` breaks a
  // run wherever `hasA` differs, so a clipped indexed path decimated worse
  // than the same path unclipped, and anything reading A off a move saw it
  // blink. Inherit rather than assert: a 3-axis path must stay free of A.
  const auto like = [](Move& m, const Move& from) {
    m.hasA = from.hasA;
    m.a = from.a;
  };
  const auto retract = [&](const glm::dvec3& at, const Move& from) {
    if (at.z < safeZ - kEps) {
      Move up;
      up.kind = Move::Kind::Rapid;
      up.to = {at.x, at.y, safeZ};
      like(up, from);
      out.push_back(up);
    }
    outside = true;
  };
  const auto reenter = [&](const glm::dvec3& at, const Move& from) {
    Move over;
    over.kind = Move::Kind::Rapid;
    over.to = {at.x, at.y, safeZ};
    like(over, from);
    out.push_back(over);
    if (at.z < safeZ - kEps) {
      Move down;
      down.kind = from.kind == Move::Kind::Rapid ? Move::Kind::Rapid
                                                 : Move::Kind::Plunge;
      down.to = at;
      down.feed = from.kind == Move::Kind::Rapid ? 0.0 : plungeFeed;
      like(down, from);
      out.push_back(down);
    }
    outside = false;
  };

  // The first move has no "from": it is where the tool goes first.
  {
    const Move& first = tp.moves[0];
    double t0, t1;
    if (clip(first.to, first.to, t0, t1))
      out.push_back(first);
    else
      outside = true; // parked off the bed until something comes back in
  }
  glm::dvec3 prev = tp.moves[0].to;
  for (size_t i = 1; i < tp.moves.size(); i++) {
    const Move& m = tp.moves[i];
    const double length = glm::length(m.to - prev);
    double t0, t1;
    if (!clip(prev, m.to, t0, t1)) {
      if (!outside)
        retract(prev, m);
      if (cutting(m))
        clipped += length;
      prev = m.to;
      continue;
    }
    if (t0 > kEps) {
      // Comes in part-way: what came before it is off the bed.
      if (cutting(m))
        clipped += length * t0;
      if (!outside)
        retract(prev, m);
      reenter(lerp(prev, m.to, t0), m);
    } else if (outside) {
      // Starts on the boundary or inside while parked: come back in here.
      reenter(prev, m);
    }
    if (t1 < 1.0 - kEps) {
      Move part = m;
      part.to = lerp(prev, m.to, t1);
      out.push_back(part);
      if (cutting(m))
        clipped += length * (1.0 - t1);
      retract(part.to, m);
    } else {
      out.push_back(m);
    }
    prev = m.to;
  }
  tp.moves = std::move(out);
  tp.clippedMm += clipped;
}

std::string MachineOverhang(const Setup& setup,
                            const std::vector<Body>& bodies) {
  const Machine& m = setup.machine;
  if (!m.HasWorkArea() || !MachineWarning(setup, bodies).empty())
    return "";
  glm::dvec3 lo(0.0), hi(0.0);
  if (!StockBounds(setup, bodies, lo, hi))
    return "";
  const char* axes[3] = {"X", "Y", "Z"};
  for (int a = 0; a < 2; a++) { // a sheet hangs off in X or Y, not Z
    const double need = hi[a] - lo[a];
    const double have = m.travelMax[a] - m.travelMin[a];
    if (need > have + 1e-6) {
      char buf[200];
      std::snprintf(buf, sizeof(buf),
                    "the sheet is %.0f mm in %s and %s travels %.0f: it hangs "
                    "%.0f mm off the bed. The cuts fit; slide the sheet "
                    "through and hold it down where it overhangs",
                    need, axes[a],
                    m.name.empty() ? "this machine" : m.name.c_str(), have,
                    need - have);
      return buf;
    }
  }
  return "";
}

std::string PlacementRefusal(const Setup& setup,
                             const std::vector<Body>& bodies) {
  for (size_t i = 0; i < setup.placements.size(); i++)
    for (size_t k = i + 1; k < setup.placements.size(); k++)
      if (setup.placements[i].jobPartId == setup.placements[k].jobPartId &&
          setup.placements[i].enabled && setup.placements[k].enabled)
        return "this setup places one part twice, and repeats are not cut "
               "yet -- the second copy would be skipped and the file would "
               "look right. Disable one placement, or give the copies their "
               "own job parts";
  for (const PlacementFit& f : CheckPlacements(setup, bodies)) {
    if (f.fit == Fit::FullyOutside)
      return "a part is not on the stock at all -- place it, or take it out "
             "of this setup";
    if (f.fit == Fit::PartlyOutside)
      return "a part hangs over the edge of the stock: the cut would run off "
             "the workpiece. Move it, or use a bigger sheet";
    if (!f.overlaps.empty())
      return "two parts are placed on top of each other -- whichever is cut "
             "second is cut out of the first one's offcut. Move one";
  }
  return "";
}

// Generating a toolpath is what ENABLE_CAM is optional ON: these three call
// into CamStrategies, which offsets polygons with Clipper2. Everything above
// -- placements, cutting order, the machine's travel -- is arithmetic on the
// job document and is built either way.
#ifdef HAVE_CAM

std::vector<Toolpath> GenerateAll(const Setup& setup,
                                  const std::vector<Body>& bodies,
                                  GenerateContext* ctx) {
  // An arrangement we cannot honestly cut stops here, in every operation's
  // own error, rather than at the machine.
  if (const std::string why = PlacementRefusal(setup, bodies); !why.empty()) {
    std::vector<Toolpath> refused;
    for (const Operation& op : setup.operations)
      if (op.enabled) {
        Toolpath t;
        t.operationId = op.id;
        t.toolNumber = op.toolNumber;
        t.error = why;
        refused.push_back(std::move(t));
      }
    return refused;
  }
  // Framed ONCE for the whole setup: the transform is an OCC operation per
  // body, and doing it per operation was both slower and a place for two
  // operations to disagree.
  const std::vector<Body> framed = BodiesInSetupFrame(setup, bodies);
  // Cutting order, not authoring order -- and applied HERE, so the backplot,
  // the simulation, the verifier and the posted file all walk the same list.
  // Two orders would be a picture that can disagree with the file, which the
  // CAM plan refuses on principle.
  std::vector<Toolpath> out;
  for (size_t i : CuttingOrder(setup)) {
    const Operation& op = setup.operations[i];
    if (!op.enabled)
      continue;
    // Each path arrives clipped to the machine (GenerateFramed), the full
    // pass and the panel's single-operation refresh alike.
    for (Toolpath& tp : GenerateOpFramed(setup, op, framed, ctx))
      out.push_back(std::move(tp));
  }
  // Tool order is an order of TOOLPATHS (R6): an operation that roughs with
  // T1 and finishes with T2 is two of them, and grouping by operation would
  // put T2's finish between two T1 roughs -- which a file per tool refuses.
  const OrderOutputBy order =
      setup.splitOutput ? OrderOutputBy::Tool : setup.order;
  // Grouped by the tool's RANK, not its number: first appearance among the
  // toolpaths (a finishing tool appears where its operation does), rest
  // after what it rests from. Sorted by number, this undid CuttingOrder --
  // the reordering its own comment warns against.
  if (order == OrderOutputBy::Tool) {
    std::vector<int> appearance;
    for (const Toolpath& tp : out)
      if (std::find(appearance.begin(), appearance.end(), tp.toolNumber) ==
          appearance.end())
        appearance.push_back(tp.toolNumber);
    const std::vector<int> rank = ToolRank(setup, appearance);
    const auto rankOf = [&](int t) {
      return std::find(rank.begin(), rank.end(), t) - rank.begin();
    };
    std::stable_sort(out.begin(), out.end(),
                     [&](const Toolpath& a, const Toolpath& b) {
                       return rankOf(a.toolNumber) < rankOf(b.toolNumber);
                     });
  }
  return out;
}

std::vector<Toolpath> GenerateOpFramed(const Setup& setup, const Operation& op,
                                       const std::vector<Body>& framed,
                                       GenerateContext* ctx) {
  std::vector<Toolpath> out;
  // The table decides, not the field. A surfacing op's row claims no Finish --
  // the RPC refuses `finish_tool` on one by name, and the panel never offers
  // it -- but this function only ever asked whether the number was non-zero,
  // so an op carrying one from any other route (a loaded file, a test, the
  // in-process API) quietly grew a second full-depth pass of a strategy that
  // has no wall to finish. Found by the capability gate on its first run
  // (docs/cam/operations.md#the-capability-table-and-the-field-table), which is
  // the whole argument for having one.
  if (op.finishTool == 0 || !OpTypeHas(op.type, OpField::Finish)) {
    out.push_back(GenerateFramed(setup, op, framed, ctx));
    return out;
  }
  // Rough then finish (R6): the same operation twice. The rough leaves
  // `finishStock` on the wall; the finish takes it with the finishing tool
  // in one full-depth pass along the final boundary.
  Operation rough = op;
  rough.stockToLeave += op.finishStock;
  // On the WALL only: the profile reads stock-to-leave in Z as well, and a
  // rough that stopped short of the floor left a 0.6 mm step in the stock
  // beyond the finishing tool's reach -- reported as leftover on a correct
  // job. The finish takes the wall band at full depth; the floor is the
  // rough's, cut to depth.
  rough.bottomZ -= op.finishStock;
  rough.cornerRelief = CornerRelief::None; // the finish clears the corners
  rough.leadArcs = false;
  out.push_back(GenerateFramed(setup, rough, framed, ctx));

  Operation finish = op;
  finish.toolNumber = op.finishTool;
  finish.stepdown = 0.0; // one pass, the full depth
  finish.peck = 0.0;
  Toolpath tp = GenerateFramed(setup, finish, framed, ctx);
  tp.finish = true;
  if (setup.FindTool(op.finishTool) == nullptr)
    tp.error = "finishing tool " + std::to_string(op.finishTool) +
               " is not in this setup's tool table";
  out.push_back(std::move(tp));
  return out;
}

std::vector<Toolpath> GenerateOp(const Setup& setup, const Operation& op,
                                 const std::vector<Body>& bodies,
                                 GenerateContext* ctx) {
  return GenerateOpFramed(setup, op, BodiesInSetupFrame(setup, bodies), ctx);
}

#endif // HAVE_CAM

} // namespace cam
