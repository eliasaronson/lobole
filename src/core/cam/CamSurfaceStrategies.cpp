#include "core/cam/CamToolpath.h"

#include "core/cam/CamDropCutter.h"
#include "core/cam/CamPicks.h"
#include "core/cam/CamRotary.h"
#include "core/cam/CamSurface.h"
#include "core/cam/CamToolpathInternal.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

// The finishing generators -- a raster or waterline over the bed, and the
// rotary patterns round the 4th axis -- and the cache-or-worker step every
// surfacing operation goes through (docs/cam/surfacing.md,
// docs/cam/rotary-plan.md, docs/cam/rotary-continuous-plan.md).
//
// Split from CamStrategies.cpp (docs/cam/cleanup-after-spiral-plan.md): these
// share nothing with the Clipper-based 2.5D strategies there. Each reads a
// cutter-location surface and lays a pattern over it.

namespace cam {

using namespace detail;

namespace {

// --- surfacing (v118) -----------------------------------------------------

// A pass whose Z varies continuously. `EmitLoop` takes a scalar z by
// signature and carries a contour's notions -- arc-length chaining, tabs,
// lead-out -- none of which apply here, so this is a sibling rather than a
// retrofit.
//
// The entry is a rapid to safe Z over the first point, then a plunge to it.
// There is no ramp: a finishing pass starts on a surface the roughing already
// cleared, so the descent is through air.
// Emit a run of passes, LINKING the ones that are neighbours.
//
// A zigzag raster does not lift between adjacent passes -- it turns at the
// end of one and comes back along the next. Lifting to safe Z after every
// pass costs a retract and a plunge each time, and it is also what put the
// ball's rim in the strip beside it: climbing vertically out of a groove on
// a steep surface, the uncut neighbour stands above the tool's own profile.
// On the waveguide that was 121 rapids through material, worst 0.787 mm, all
// of them at the steep throat end (docs/cam/hardening-round.md).
//
// The link is a CUT move straight across. Over one stepover that is safe on
// either curvature: concave, the chord runs above the surface; convex, it
// runs below by the sagitta, which for a 0.9 mm step on a 30 mm crown is
// 3 microns. Anything further apart than a couple of stepovers is a real gap
// -- a hole in the region, or the far side of it -- and gets the lift.
void EmitSurfacePasses(Toolpath& tp, const Setup& setup, const Tool& tool,
                       const std::vector<SurfacePath>& passes,
                       double linkWithin) {
  // ORDER them first, nearest end to nearest end, reversing a pass when its
  // far end is the closer one.
  //
  // Raster order is not tour order the moment the region has a hole in it.
  // The waveguide's flare is an annulus, so every row crosses it TWICE and
  // the passes arrive left, right, left, right -- consecutive entries lying
  // across the hole from each other rather than side by side. Linking them
  // in that order links almost nothing: it took 121 lifts to 120.
  std::vector<SurfacePath> tour;
  tour.reserve(passes.size());
  std::vector<bool> used(passes.size(), false);
  glm::dvec3 cursor(0.0);
  bool have = false;
  for (size_t n = 0; n < passes.size(); n++) {
    size_t best = passes.size();
    bool bestFlip = false;
    double bestD = 0.0;
    for (size_t k = 0; k < passes.size(); k++) {
      if (used[k] || passes[k].size() < 2)
        continue;
      const double dHead = have ? glm::length(passes[k].front() - cursor) : 0.0;
      const double dTail = have ? glm::length(passes[k].back() - cursor) : 1.0;
      const bool flip = dTail < dHead;
      const double d = std::min(dHead, dTail);
      if (best == passes.size() || d < bestD) {
        best = k;
        bestFlip = flip;
        bestD = d;
      }
    }
    if (best == passes.size())
      break;
    used[best] = true;
    SurfacePath path = passes[best];
    if (bestFlip)
      std::reverse(path.begin(), path.end());
    cursor = path.back();
    have = true;
    tour.push_back(std::move(path));
  }

  bool down = false;
  glm::dvec3 at(0.0);
  for (const SurfacePath& path : tour) {
    if (path.size() < 2)
      continue;
    const bool linkable = down && glm::length(path.front() - at) <= linkWithin;
    if (linkable) {
      Emit(tp, Move::Kind::Cut, path.front(), tool.feedCut);
    } else {
      if (down)
        Emit(tp, Move::Kind::Rapid, {at.x, at.y, setup.safeZ}, 0.0);
      Emit(tp, Move::Kind::Rapid, {path.front().x, path.front().y, setup.safeZ},
           0.0);
      Emit(tp, Move::Kind::Plunge, path.front(), tool.feedPlunge);
    }
    for (size_t i = 1; i < path.size(); i++)
      Emit(tp, Move::Kind::Cut, path[i], tool.feedCut);
    at = path.back();
    down = true;
  }
  if (down)
    Emit(tp, Move::Kind::Rapid, {at.x, at.y, setup.safeZ}, 0.0);
}

// 4-axis finishing (docs/cam/rotary-plan.md), by the op's pattern: parallel
// passes, each at one A with the part turning between them, or one spiral
// per layer with A turning WHILE the tool cuts -- continuous 4-axis
// (docs/cam/rotary-continuous-plan.md).
//
// The same shape as `GenerateSurface` and deliberately so: a contact surface,
// a pattern over it, and a link between the passes. What changes is the
// SPACE -- r(x, theta) about the rotary axis instead of z(x, y) over the bed
// -- and it changes in exactly one place, which is the argument the plan made
// for keeping the generators free of world assumptions.
void GenerateRotarySurface(const Setup& setup, const Operation& op,
                           const Tool& tool, const std::vector<Body>& bodies,
                           Toolpath& tp) {
  if (!setup.machine.hasRotaryAxis) {
    tp.error = "this setup's machine has no 4th axis, so a rotary pass "
               "cannot be posted for it";
    return;
  }
  // The grid: fine enough for the cusp this cutter is allowed to leave, the
  // same derivation the planar drop cutter uses.
  const double cusp = DerivedCusp(tool, 0.0);
  const double res = DerivedDropRes(tool, 0.0, cusp);

  double x0 = 1e18, x1 = -1e18, rad = 0.0;
  for (const Body& b : bodies) {
    if (!b.shape.IsValid())
      continue;
    const CADShape::Mesh mesh = b.shape.GenerateMeshDetached(res * 0.25);
    for (const auto& v : mesh.vertices) {
      x0 = std::min(x0, static_cast<double>(v.position.x));
      x1 = std::max(x1, static_cast<double>(v.position.x));
      rad = std::max(rad, std::hypot(static_cast<double>(v.position.y),
                                     static_cast<double>(v.position.z)));
    }
  }
  if (x1 <= x0 || rad <= 0.0) {
    tp.error = "there is no part on the rotary axis to finish";
    return;
  }

  const RadiusMap grid = MakeRotaryStock(x0, x1, rad, res);
  RadiusMap model = SampleModelRotary(bodies, grid);
  // Stock to leave on a rotary pass holds the tool OUT, radially -- the same
  // field that shifts bottomZ on a 2.5D op and raises the tool on a planar
  // finish. Same name, three mechanics, because one cuts to a depth, one to
  // a surface and one to a radius.
  if (op.stockToLeave > 0.0)
    for (int j = 0; j < model.nTheta; j++)
      for (int i = 0; i < model.nx; i++)
        if (const double r = model.At(i, j); r > 0.0)
          model.Set(i, j, static_cast<float>(r + op.stockToLeave));

  const RadiusMap cl = RotaryCL(model, tool);

  // Layers, when the op has a stepdown: floors from the blank inward, each
  // the cutter-location surface held out at that radius, the last the
  // surface itself. FreeCAD's rotary surface and Kiri:Moto's lathe both rough
  // this way -- the same pattern over a clamped surface -- and it is the only
  // rotary roughing there is. A cell the tool reaches nothing in stays empty
  // at every floor, so a layer still breaks (or flies) where the finish does.
  double clMin = 1e18;
  for (float v : cl.r)
    if (v > 0.0f)
      clMin = std::min(clMin, static_cast<double>(v));
  const double blankR = RotaryBlankRadius(setup, rad);
  std::vector<double> floors;
  if (op.stepdown > 0.0)
    for (double f = blankR - op.stepdown; f > clMin + 1e-6; f -= op.stepdown)
      floors.push_back(f);
  floors.push_back(0.0); // the finish: the surface, unclamped

  // Setup space: the tool stands on +Z over the axis, at y = 0, and the PART
  // turns. A point at part angle theta is cut with A = 90 - theta, which is
  // the angle that brings theta to the top (a part angle appears in setup
  // space at theta + A).
  const auto go = [&](Move::Kind kind, double x, double r, double aDeg,
                      double feed) {
    Move m;
    m.kind = kind;
    m.to = {x, 0.0, r};
    m.feed = feed;
    m.hasA = true;
    m.a = aDeg;
    tp.moves.push_back(m);
  };
  // Clear of the work before the trunnion turns, every time. The post
  // enforces safe Z before an index too, but a toolpath that arrives
  // already retracted is one the post never has to correct -- and here
  // the retract is RADIAL, so "safe" is a radius outside the blank.
  const double safeR = std::max(blankR, rad) + std::max(2.0, tool.diameter);
  const double pitch = std::clamp(op.stepover, 0.02, 0.95) * tool.diameter;

  // Where the last layer's spiral left the axis, in unwound part degrees.
  // The next one starts on the same turn rather than back at zero: a spiral
  // winds the axis hundreds of turns, and restarting at theta = 0 would ask
  // for all of them back on the rapid between two layers.
  double thetaCarry = 0.0;
  for (const double floorR : floors) {
    RadiusMap layer = cl;
    if (floorR > 0.0)
      for (float& v : layer.r)
        if (v > 0.0f)
          v = std::max(v, static_cast<float>(floorR));

    if (op.rotaryPattern == RotaryPattern::Spiral) {
      RotarySpiralRequest req;
      req.pitch_mm = pitch;
      req.sampleStep_mm = op.sampleStep;
      req.tolerance_mm = cusp * 0.25;
      // Where the tool reaches nothing, the helix crosses outside the blank.
      req.airRadius = blankR + 1.0;
      std::vector<RotaryPathPoint> path = RotarySpiralPath(layer, req);
      if (path.size() < 2)
        continue;
      const double shift = std::ceil(thetaCarry / 360.0) * 360.0;
      for (RotaryPathPoint& q : path)
        q.thetaDeg += shift;
      thetaCarry = path.back().thetaDeg;
      const RotaryPathPoint& p0 = path.front();
      go(Move::Kind::Rapid, p0.x, safeR, 90.0 - p0.thetaDeg, 0.0);
      go(Move::Kind::Plunge, p0.x, p0.r, 90.0 - p0.thetaDeg, tool.feedPlunge);
      for (size_t k = 1; k < path.size(); k++)
        go(Move::Kind::Cut, path[k].x, path[k].r, 90.0 - path[k].thetaDeg,
           tool.feedCut);
      go(Move::Kind::Rapid, path.back().x, safeR, 90.0 - path.back().thetaDeg,
         0.0);
      continue;
    }

    RotaryParallelRequest req;
    req.stepover_mm = pitch;
    req.sampleStep_mm = op.sampleStep;
    for (const RotaryPass& pass : RotaryParallelPasses(layer, req)) {
      if (pass.pts.size() < 2)
        continue;
      const double a = 90.0 - pass.thetaDeg;
      go(Move::Kind::Rapid, pass.pts.front().x, safeR, a, 0.0);
      go(Move::Kind::Plunge, pass.pts.front().x, pass.pts.front().y, a,
         tool.feedPlunge);
      for (size_t k = 1; k < pass.pts.size(); k++)
        go(Move::Kind::Cut, pass.pts[k].x, pass.pts[k].y, a, tool.feedCut);
      go(Move::Kind::Rapid, pass.pts.back().x, safeR, a, 0.0);
    }
  }
  if (tp.moves.empty())
    tp.error = "this part gives no rotary passes — check the cutter against "
               "the part's size on the axis";
}

void GenerateSurface(const Setup& setup, const Operation& op, const Tool& tool,
                     const std::vector<Body>& bodies, Toolpath& tp) {
  // The region to stay inside: the picked face's SILHOUETTE, or the whole
  // part when nothing was picked. The face gives its outline and never its
  // depths -- those come from the cutter-location surface of the whole model,
  // because a pass taking Z from the picked face alone would gouge whatever
  // stands beside it.
  SurfaceRegion region;
  if (op.targets.empty()) {
    region = WholePartSilhouette(bodies, 0.02);
  } else {
    region = PickedSurfaceRegion(bodies, op.targets.front().ref, 0.02,
                                 op.targets.front().jobPartId);
  }
  if (!region.ok) {
    tp.error = region.why;
    return;
  }

  glm::dvec2 lo(0.0), hi(0.0);
  bool any = false;
  for (const Loop& l : region.loops)
    for (const glm::dvec2& p : l.pts) {
      lo = any ? glm::min(lo, p) : p;
      hi = any ? glm::max(hi, p) : p;
      any = true;
    }
  if (!any) {
    tp.error = "that region has no area to finish";
    return;
  }

  DropRequest drop;
  drop.min = lo;
  drop.max = hi;
  drop.res = op.sampleStep; // 0 derives, which is the usual case
  drop.floorZ = op.bottomZ - 1.0;
  const CLSurface cl = DropCut(bodies, tool, drop);
  if (!cl.ok) {
    tp.error = cl.why;
    return;
  }

  // Leaving stock on a FINISHING pass raises the tool, it does not lower the
  // bottom: the cut is a surface, not a depth, so the allowance is a distance
  // above it. (On a 2.5D op the same field shifts `bottomZ`, which is the
  // right answer there and the wrong one here.)
  const double lift = op.stockToLeave;

  std::vector<SurfacePath> passes;
  std::string why;
  if (op.type == OpType::SurfaceParallel) {
    ParallelRequest req;
    req.stepover_mm = std::clamp(op.stepover, 0.02, 0.95) * tool.diameter;
    req.sampleStep_mm = op.sampleStep;
    req.cutAngleDeg = op.cutAngleDeg;
    req.floorZ = drop.floorZ;
    SurfacePaths r = ParallelPasses(cl, region.loops, req);
    why = r.why;
    passes = std::move(r.passes);
  } else {
    WaterlineRequest req;
    req.zStep_mm = op.zStep > 0.0 ? op.zStep : DerivedCusp(tool, 0.0) * 10.0;
    req.topZ = op.topZ;
    req.bottomZ = op.bottomZ;
    req.sampleStep_mm = op.sampleStep;
    req.floorZ = drop.floorZ;
    SurfacePaths r = WaterlinePasses(cl, region.loops, req);
    why = r.why;
    passes = std::move(r.passes);
  }
  if (passes.empty()) {
    tp.error = why.empty() ? "that region produced no finishing pass" : why;
    return;
  }

  for (SurfacePath& path : passes) {
    for (glm::dvec3& p : path) {
      p.z += lift;
      // A finishing pass never goes deeper than it was told to, whatever the
      // surface says -- the same rule DepthPasses follows for a level.
      p.z = std::max(p.z, op.bottomZ);
    }
  }
  // Two stepovers: a neighbouring pass is one away, and a zigzag's turn puts
  // its start at the same end of it. Anything beyond that is a gap the tool
  // has to lift over.
  const double step = op.type == OpType::SurfaceParallel
                          ? std::clamp(op.stepover, 0.02, 0.95) * tool.diameter
                          : (op.zStep > 0.0 ? op.zStep : tool.diameter * 0.1);
  EmitSurfacePasses(tp, setup, tool, passes, step * 2.0);
}

} // namespace

namespace detail {

// A surfacing operation, from the cache, from a worker later, or right here
// -- the same three-way the adaptive pass has, one level up
// (docs/ui/long-operations.md, phase 5). The unit is the whole OPERATION
// because a finishing pass is one computation from the cutter-location
// surface outward, with no interior seam to stop at.
void SurfaceThroughCache(const Setup& setup, const Operation& op,
                         const Tool& tool, const std::vector<Body>& bodies,
                         Toolpath& tp, GenerateContext* ctx) {
  const bool cached =
      ctx != nullptr && ctx->surfaceCache != nullptr && ctx->bodiesKey != 0;
  const uint64_t key = cached ? SurfaceKey(setup, op, tool, ctx->bodiesKey) : 0;
  if (cached) {
    if (const SurfaceResult* hit = ctx->surfaceCache->Find(key)) {
      tp.moves = hit->moves;
      tp.error = hit->error;
      tp.strategy = hit->strategy;
      return;
    }
    if (ctx->defer) {
      SurfaceRequest req;
      req.key = key;
      req.setup = setup;
      req.op = op;
      req.tool = tool;
      req.bodies = bodies;
      ctx->pendingSurface.push_back(std::move(req));
      tp.pending = true;
      return;
    }
  }
  if (op.type == OpType::SurfaceRotary)
    GenerateRotarySurface(setup, op, tool, bodies, tp);
  else
    GenerateSurface(setup, op, tool, bodies, tp);
  if (cached) {
    SurfaceResult r;
    r.moves = tp.moves;
    r.error = tp.error;
    r.strategy = tp.strategy;
    ctx->surfaceCache->Put(key, std::move(r));
  }
}

} // namespace detail

SurfaceResult SurfacePass(const SurfaceRequest& request) {
  // The form a worker thread calls. It takes the strategy's own path with no
  // context at all, so nothing here touches a cache another thread owns.
  Toolpath tp;
  const Tool& tool = request.tool;
  if (request.op.type == OpType::SurfaceRotary)
    GenerateRotarySurface(request.setup, request.op, tool, request.bodies, tp);
  else
    GenerateSurface(request.setup, request.op, tool, request.bodies, tp);
  SurfaceResult out;
  out.moves = std::move(tp.moves);
  out.error = std::move(tp.error);
  out.strategy = std::move(tp.strategy);
  return out;
}

} // namespace cam
