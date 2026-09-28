#include "core/cam/CamToolpath.h"

#include "core/cam/CamDropCutter.h"
#include "core/cam/CamPicks.h"
#include "core/cam/CamRotary.h"
#include "core/cam/CamSurface.h"
#include "core/cam/CamToolpathInternal.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <numbers>
#include <string>
#include <vector>

// The finishing generators -- a raster or waterline over the bed, and the
// rotary patterns round the 4th axis -- and the cache-or-worker step every
// surfacing operation goes through (docs/cam/operations.md#3-axis-surfacing,
// docs/cam/rotary.md#continuous-surface_rotary).
//
// Split from CamStrategies.cpp (docs/cam/README.md#where-the-code-is): these
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
// The entry is a rapid to safe Z over the first point, a rapid down to just
// above what is known to be left there (`floorAt`), then a plunge to it.
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
// of them at the steep throat end (docs/cam/operations.md#3-axis-surfacing).
//
// The link is a CUT move straight across. Over one stepover that is safe on
// either curvature: concave, the chord runs above the surface; convex, it
// runs below by the sagitta, which for a 0.9 mm step on a 30 mm crown is
// 3 microns. Anything further apart than a couple of stepovers is a real gap
// -- a hole in the region, or the far side of it -- and gets the lift.
void EmitSurfacePasses(
    Toolpath& tp, const Setup& setup, const Tool& tool,
    const std::vector<SurfacePath>& passes, double linkWithin,
    const std::function<double(const glm::dvec3&)>& floorAt) {
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
      // Down to just above what is left, then the plunge feed.
      if (const double z = floorAt(path.front());
          z < setup.safeZ - 1e-3 && z > path.front().z + 1e-3)
        Emit(tp, Move::Kind::Rapid, {path.front().x, path.front().y, z}, 0.0);
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

// --- rest machining (v125) ---------------------------------------------

// What a cutter LEAVES when its tip rides `tip` at every cell: at each cell,
// the lowest point of the tool's underside over it, across every placement
// within a radius. Two tools' tip heights differ on any slope -- a big ball's
// tip rides higher over a tilted face than a small one's while leaving
// exactly the same surface -- so rest machining compares ENVELOPES, never
// tips. `spanY` is the cell pitch across (mm), given per cell so the rotary
// map can pass arc length, which grows with the radius; `wrapY` closes the
// rotary map on itself. Cells `reached` says are off the part are skipped.
template <class TipAt, class Reached, class SpanY>
std::vector<float> ToolEnvelope(int nx, int ny, double resX, SpanY spanY,
                                bool wrapY, TipAt tipAt, Reached reached,
                                const Tool& tool) {
  const double R = 0.5 * tool.diameter;
  const bool ball = tool.type == ToolType::Ball;
  std::vector<float> out(static_cast<size_t>(nx) * ny, 0.0f);
  const int kx = static_cast<int>(std::ceil(R / resX));
  for (int j = 0; j < ny; j++)
    for (int i = 0; i < nx; i++) {
      double best = 1e18;
      const double sy = std::max(spanY(i, j), 1e-6);
      const int ky = std::min(static_cast<int>(std::ceil(R / sy)), ny / 2);
      for (int dj = -ky; dj <= ky; dj++) {
        int jj = j + dj;
        if (wrapY)
          jj = ((jj % ny) + ny) % ny;
        else if (jj < 0 || jj >= ny)
          continue;
        for (int di = -kx; di <= kx; di++) {
          const int ii = i + di;
          if (ii < 0 || ii >= nx || !reached(ii, jj))
            continue;
          const double d2 = (di * resX) * (di * resX) + (dj * sy) * (dj * sy);
          if (d2 > R * R)
            continue;
          // The underside of the tool standing at (ii, jj), over (i, j).
          const double under =
              tipAt(ii, jj) + (ball ? R - std::sqrt(R * R - d2) : 0.0);
          best = std::min(best, under);
        }
      }
      out[static_cast<size_t>(j) * nx + i] =
          static_cast<float>(best < 1e17 ? best : 0.0);
    }
  return out;
}

// The cells a rest pass cuts: where the larger cutter left measurably more
// than this one would, grown by this cutter's radius so the tool standing
// beside the leftover -- which is where it cuts it from -- is inside too.
//
// Only where the PART is: `part` is the part's own height (or radius) per
// cell, NaN where there is none. Off the part both envelopes are just tool
// undersides over air, and their difference -- a big ball rolling off an
// outside edge -- is not material anyone left.
std::vector<bool> RestCells(const std::vector<float>& leftBig,
                            const std::vector<float>& leftSmall,
                            const std::vector<float>& part, int nx, int ny,
                            double resX, double spanY, bool wrapY,
                            double tolerance, double growBy) {
  std::vector<bool> left(leftBig.size(), false);
  for (size_t k = 0; k < left.size(); k++)
    left[k] = !std::isnan(part[k]) &&
              leftBig[k] - std::max(leftSmall[k], part[k]) > tolerance;
  std::vector<bool> out(left.size(), false);
  const int gx = static_cast<int>(std::ceil(growBy / resX));
  const int gy = std::min(static_cast<int>(std::ceil(growBy / spanY)), ny / 2);
  for (int j = 0; j < ny; j++)
    for (int i = 0; i < nx; i++) {
      if (!left[static_cast<size_t>(j) * nx + i])
        continue;
      for (int dj = -gy; dj <= gy; dj++) {
        int jj = j + dj;
        if (wrapY)
          jj = ((jj % ny) + ny) % ny;
        else if (jj < 0 || jj >= ny)
          continue;
        for (int di = -gx; di <= gx; di++) {
          const int ii = i + di;
          if (ii >= 0 && ii < nx)
            out[static_cast<size_t>(jj) * nx + ii] = true;
        }
      }
    }
  return out;
}

// The rest mask on a fine grid, computed on a COARSE one. The fine grid is
// the small cutter's finishing grid (a tenth of a millimetre on a 2 mm
// ball), and a big ball's envelope kernel over it is thousands of cells per
// cell: on a 30 mm shaft that was a minute. The mask does not need that
// grain -- it is grown by the small cutter's radius anyway -- so the
// envelopes are taken over blocks about a third of that radius across, each
// block CONSERVATIVE in the direction that can only add cutting: the big
// tool's tip the highest in it, the small tool's and the part's the lowest.
// Leftover can be overstated by a block, never missed.
//
// `bigTip` / `smallTip` / `part` read the fine grid (`part` NaN where there is
// no part), `reachedBig` / `reachedSmall` say whether a fine cell is on it;
// `spanY` is a fine cell's pitch across (arc length on the rotary). `ny` must
// be kept whole when `wrapY`.
template <class BigTip, class BigReached, class SmallTip, class SmallReached,
          class Part, class SpanY>
std::vector<bool>
RestMask(int nx, int ny, double resX, SpanY spanY, bool wrapY, BigTip bigTip,
         BigReached reachedBig, SmallTip smallTip, SmallReached reachedSmall,
         Part partAt, const Tool& big, const Tool& small, double tolerance,
         std::vector<float>* leftTop = nullptr) {
  const double rSmall = 0.5 * small.diameter;
  const int fx = std::max(1, static_cast<int>(std::floor(rSmall / 3.0 / resX)));
  int fy =
      std::max(1, static_cast<int>(std::floor(
                      rSmall / 3.0 / std::max(1e-6, spanY(nx / 2, ny / 2)))));
  if (wrapY)
    while (fy > 1 && ny % fy != 0) // whole blocks, so the ring still closes
      fy--;
  const int cx = (nx + fx - 1) / fx;
  const int cy = (ny + fy - 1) / fy;
  const size_t n = static_cast<size_t>(cx) * cy;
  std::vector<float> bTip(n, -1e18f), sTip(n, 1e18f), part(n, std::nanf(""));
  std::vector<bool> bOn(n, false), sOn(n, false);
  for (int j = 0; j < ny; j++)
    for (int i = 0; i < nx; i++) {
      const size_t c = static_cast<size_t>(j / fy) * cx + i / fx;
      if (reachedBig(i, j)) {
        bOn[c] = true;
        bTip[c] = std::max(bTip[c], static_cast<float>(bigTip(i, j)));
      }
      if (reachedSmall(i, j)) {
        sOn[c] = true;
        sTip[c] = std::min(sTip[c], static_cast<float>(smallTip(i, j)));
      }
      const double z = partAt(i, j);
      if (!std::isnan(z))
        part[c] = std::isnan(part[c])
                      ? static_cast<float>(z)
                      : std::min(part[c], static_cast<float>(z));
    }
  const double cRes = resX * fx;
  const auto cSpan = [&](int i, int j) {
    return spanY(std::min(nx - 1, i * fx + fx / 2),
                 std::min(ny - 1, j * fy + fy / 2)) *
           fy;
  };
  const std::vector<float> leftBig = ToolEnvelope(
      cx, cy, cRes, cSpan, wrapY,
      [&](int i, int j) {
        return static_cast<double>(bTip[static_cast<size_t>(j) * cx + i]);
      },
      [&](int i, int j) {
        return static_cast<bool>(bOn[static_cast<size_t>(j) * cx + i]);
      },
      big);
  const std::vector<float> leftSmall = ToolEnvelope(
      cx, cy, cRes, cSpan, wrapY,
      [&](int i, int j) {
        return static_cast<double>(sTip[static_cast<size_t>(j) * cx + i]);
      },
      [&](int i, int j) {
        return static_cast<bool>(sOn[static_cast<size_t>(j) * cx + i]);
      },
      small);
  // Grown by the small radius and half a block's diagonal, since a block's
  // leftover may sit anywhere in it.
  const double grow = rSmall + 0.5 * std::hypot(cRes, cSpan(cx / 2, cy / 2));
  const std::vector<bool> coarse =
      RestCells(leftBig, leftSmall, part, cx, cy, cRes, cSpan(cx / 2, cy / 2),
                wrapY, tolerance, grow);
  std::vector<bool> out(static_cast<size_t>(nx) * ny, false);
  for (int j = 0; j < ny; j++)
    for (int i = 0; i < nx; i++)
      out[static_cast<size_t>(j) * nx + i] =
          coarse[static_cast<size_t>(j / fy) * cx + i / fx];
  // How high the big cutter's leftover stands, per fine cell, NaN where it
  // left nothing measurable: a block's envelope is taken from its HIGHEST
  // tip, so this can only overstate -- the side a rapid's floor must err on
  // (docs/cam/operations.md#surface-entries).
  if (leftTop != nullptr) {
    leftTop->assign(static_cast<size_t>(nx) * ny, std::nanf(""));
    for (int j = 0; j < ny; j++)
      for (int i = 0; i < nx; i++) {
        const size_t c = static_cast<size_t>(j / fy) * cx + i / fx;
        if (!std::isnan(part[c]) && leftBig[c] - part[c] > tolerance)
          (*leftTop)[static_cast<size_t>(j) * nx + i] = leftBig[c];
      }
  }
  return out;
}

// --- entries ---------------------------------------------------------------

// A surface pass rapids down to just above what is KNOWN to be left and feeds
// only the last of it (docs/cam/operations.md#surface-entries). This far
// above: a millimetre, or the tool's radius when that is less.
double EntryClearance(const Tool& tool) {
  return std::min(1.0, 0.5 * tool.diameter);
}

// The ridge a pass leaves beside the next one `step` over: a ball's scallop,
// nothing for a flat end.
double Scallop(const Tool& tool, double step) {
  if (tool.type != ToolType::Ball)
    return 0.0;
  const double R = 0.5 * tool.diameter;
  const double h = std::min(0.5 * step, R);
  return R - std::sqrt(R * R - h * h);
}

// The highest of `top` (NaN = nothing there) within `R` of cell (ci, cj):
// `spanY` is a cell's pitch across, `wrapY` closes a rotary map.
double FootprintMax(const std::vector<float>& top, int nx, int ny, double resX,
                    double spanY, bool wrapY, int ci, int cj, double R) {
  double best = -1e18;
  const int kx = static_cast<int>(std::ceil(R / resX));
  const int ky =
      std::min(static_cast<int>(std::ceil(R / std::max(spanY, 1e-6))), ny / 2);
  for (int dj = -ky; dj <= ky; dj++) {
    int jj = cj + dj;
    if (wrapY)
      jj = ((jj % ny) + ny) % ny;
    else if (jj < 0 || jj >= ny)
      continue;
    for (int di = -kx; di <= kx; di++) {
      const int ii = ci + di;
      if (ii < 0 || ii >= nx)
        continue;
      const float v = top[static_cast<size_t>(jj) * nx + ii];
      if (!std::isnan(v))
        best = std::max(best, static_cast<double>(v));
    }
  }
  return best;
}

// 4-axis finishing (docs/cam/rotary.md#continuous-surface_rotary), by the op's
// pattern: parallel passes, each at one A with the part turning between them,
// or one spiral per layer with A turning WHILE the tool cuts -- continuous
// 4-axis (docs/cam/rotary.md#patterns-rotary_pattern).
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

  // A tool radius past each end: a ball centred beyond the part rolls over
  // its end's edge and down a domed end's steep foot, which one centred no
  // further than the end never reaches. What lies at the chuck or the
  // tailstock is clipped by the holding window (GenerateOp), so running past
  // the ends takes the cutter nowhere the holding does not already bound
  // (docs/cam/rotary.md#layers-rest-and-the-rough-verb).
  const double past = 0.5 * tool.diameter;
  const RadiusMap grid = MakeRotaryStock(x0 - past, x1 + past, rad, res);
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

  RadiusMap cl = RotaryCL(model, tool);

  // Part of the part (docs/cam/rotary.md#continuous-surface_rotary): a picked
  // face bounds the pass ALONG the axis to its own extent -- round the axis it
  // cannot, the part turns. Cells outside it are unreached, which every
  // pattern already treats as "break the pass, or fly over".
  if (!op.targets.empty()) {
    const SurfaceRegion face = PickedSurfaceRegion(
        bodies, op.targets.front().ref, 0.02, op.targets.front().jobPartId);
    if (!face.ok) {
      tp.error = face.why;
      return;
    }
    double fx0 = 1e18, fx1 = -1e18;
    for (const Loop& l : face.loops)
      for (const glm::dvec2& q : l.pts) {
        fx0 = std::min(fx0, q.x);
        fx1 = std::max(fx1, q.x);
      }
    for (int j = 0; j < cl.nTheta; j++)
      for (int i = 0; i < cl.nx; i++)
        if (const double x = cl.CellX(i); x < fx0 - 1e-6 || x > fx1 + 1e-6)
          cl.Set(i, j, 0.0f);
  }

  // Cells this cutter cannot reach at all (off a picked face's span, or
  // beyond the part) still stand at the blank: taken BEFORE the rest mask,
  // whose empty cells the finish did cut.
  std::vector<float> blankTop(cl.r.size(), std::nanf(""));
  {
    const double blank = RotaryBlankRadius(setup, rad);
    for (size_t k = 0; k < cl.r.size(); k++)
      if (cl.r[k] <= 0.0f)
        blankTop[k] = static_cast<float>(blank);
  }

  // What a rest pass knows is left after its finish (NaN where nothing), and
  // how far above that the leftover may stand: the finish's own skin and cusp.
  std::vector<float> restTop;
  double restAbove = 0.0;
  bool restKnown = false;

  // Rest machining: cut only where the larger cutter that finished before
  // could not reach -- where its envelope stands proud of this one's.
  if (op.restFromTool != 0) {
    const Tool* bigger = setup.FindTool(op.restFromTool);
    if (bigger == nullptr) {
      tp.error = "the cutter it cleans up after, T" +
                 std::to_string(op.restFromTool) + ", is not in this setup";
      return;
    }
    // The larger cutter's surface on ITS OWN grid, read here by bilinear
    // interpolation: on this cutter's finer one it cost ~4x this pass's own
    // surface (the kernel grows with the cutter, the grid with this one), and
    // was most of the pawn blank's 157 s. It is the envelope of a ball larger
    // than this one, smooth to well inside the rest threshold at its own
    // resolution.
    const double bigRes =
        std::max(res, DerivedDropRes(*bigger, 0.0, DerivedCusp(*bigger, 0.0)));
    RadiusMap bigModel = SampleModelRotary(
        bodies, MakeRotaryStock(x0 - past, x1 + past, rad, bigRes));
    if (op.stockToLeave > 0.0)
      for (float& v : bigModel.r)
        if (v > 0.0f)
          v = static_cast<float>(v + op.stockToLeave);
    const RadiusMap bigOwn = RotaryCL(bigModel, *bigger);
    const RadiusMap bigCl = ResampleRotary(bigOwn, cl);
    const double dTheta = 2.0 * 3.14159265358979323846 / cl.nTheta;
    // Radius for height: the tool comes from outside with its tip innermost,
    // and across the axis the pitch is ARC length, which grows with the
    // radius. The part's radius is the sampled model's (it carries the stock
    // to leave, as both cutters' surfaces do).
    const std::vector<bool> cut = RestMask(
        cl.nx, cl.nTheta, cl.res,
        [&](int i, int j) {
          return std::max(1e-3, static_cast<double>(model.At(i, j))) * dTheta;
        },
        true, [&](int i, int j) { return static_cast<double>(bigCl.At(i, j)); },
        [&](int i, int j) { return bigCl.At(i, j) > 0.0f; },
        [&](int i, int j) { return static_cast<double>(cl.At(i, j)); },
        [&](int i, int j) { return cl.At(i, j) > 0.0f; },
        [&](int i, int j) {
          const float r = model.At(i, j);
          return r > 0.0f ? static_cast<double>(r) : std::nan("");
        },
        *bigger, tool, std::max(0.02, 2.0 * cusp), &restTop);
    size_t kept = 0;
    for (int j = 0; j < cl.nTheta; j++)
      for (int i = 0; i < cl.nx; i++) {
        if (!cut[static_cast<size_t>(j) * cl.nx + i])
          cl.Set(i, j, 0.0f);
        else if (cl.At(i, j) > 0.0f)
          kept++;
      }
    if (kept == 0) {
      tp.error = "nothing is left for it: T" + std::to_string(op.restFromTool) +
                 " reached everywhere this cutter can";
      return;
    }
    if (const Operation* finish = FinishedBefore(setup, op)) {
      restKnown = true;
      restAbove = finish->stockToLeave + DerivedCusp(*bigger, 0.0);
    }
  }

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

  // Entries (docs/cam/operations.md#surface-entries): rapid radially down to
  // just above what is known to be left, then the plunge feed -- not the
  // whole way from the safe radius at the plunge feed, which was 26 of the
  // rotary sampler's rest pass's 58 minutes. Known: the blank for a first
  // layer, the floor the layer before left (ridges and all) for a later
  // one, the leftover under the tool for a rest pass after its finish. A
  // cell no pass of this op reaches still stands at the blank, so an entry
  // beside one comes down no lower than that.
  const double clear = EntryClearance(tool);
  const double ridge = Scallop(tool, pitch);
  const auto entryR = [&](size_t li, double x, double r, double thetaDeg) {
    const int ci =
        std::clamp(static_cast<int>((x - cl.x0) / cl.res), 0, cl.nx - 1);
    const double turn =
        std::fmod(thetaDeg, 360.0) + (thetaDeg < 0 ? 360.0 : 0.0);
    const int cj =
        std::clamp(static_cast<int>(std::floor(turn / 360.0 * cl.nTheta)), 0,
                   cl.nTheta - 1);
    const double span = cl.ArcAt(std::max(r, 1.0));
    const double R = 0.5 * tool.diameter;
    double top = 0.0;
    if (li > 0)
      // Not the floor alone: a cell beside a wall is never under a tool
      // centre -- its CL stands at the wall -- and keeps what the bodies
      // round it left, up to that CL. The highest of the two under the
      // footprint is what the layer before can have left (the rotary
      // sampler's rough came down into a shoulder's foot without it).
      top =
          std::max(floors[li - 1], FootprintMax(cl.r, cl.nx, cl.nTheta, cl.res,
                                                span, true, ci, cj, R)) +
          ridge;
    else if (restKnown)
      top = FootprintMax(restTop, cl.nx, cl.nTheta, cl.res, span, true, ci, cj,
                         R) +
            restAbove;
    else
      return std::min(safeR, blankR + clear);
    top = std::max(top, FootprintMax(blankTop, cl.nx, cl.nTheta, cl.res, span,
                                     true, ci, cj, R));
    return std::min(safeR, std::max(r, top) + clear);
  };
  const auto enter = [&](size_t li, double x, double r, double thetaDeg) {
    const double a = 90.0 - thetaDeg;
    go(Move::Kind::Rapid, x, safeR, a, 0.0);
    if (const double e = entryR(li, x, r, thetaDeg);
        e < safeR - 1e-3 && e > r + 1e-3)
      go(Move::Kind::Rapid, x, e, a, 0.0);
    go(Move::Kind::Plunge, x, r, a, tool.feedPlunge);
  };

  // Where the last layer's spiral left the axis, in unwound part degrees.
  // The next one starts on the same turn rather than back at zero: a spiral
  // winds the axis hundreds of turns, and restarting at theta = 0 would ask
  // for all of them back on the rapid between two layers.
  double thetaCarry = 0.0;

  // The helical pattern's lead: stated, or the part's own twist. Derived ONCE
  // from the surface itself, not per layer, so every layer follows the same
  // flutes. A part with nothing to follow is cut as parallel passes -- and
  // says so, rather than quietly doing something other than it was asked.
  double lead = 0.0;
  if (op.rotaryPattern == RotaryPattern::Helical) {
    lead = op.leadMm;
    if (lead == 0.0) {
      const double twist = DeriveRotaryTwist(cl);
      if (std::abs(twist) > 1e-3)
        lead = 360.0 / twist;
    }
    if (lead == 0.0)
      tp.strategy = "parallel passes -- a helical pass follows a twist, and "
                    "this part has none to follow (set lead_mm to cut one "
                    "anyway)";
  }

  for (size_t li = 0; li < floors.size(); li++) {
    const double floorR = floors[li];
    // What the layer before left: the blank for the first. A cell this layer
    // would take no lower is EMPTY for it -- the grip, a span a layer above
    // already cut to the surface -- so the pass breaks there instead of
    // riding along it at the cutting feed: half the rotary sampler's rough
    // was that.
    const double before = li == 0 ? blankR : floors[li - 1];
    RadiusMap layer = cl;
    for (float& v : layer.r) {
      if (v <= 0.0f)
        continue;
      const double target =
          floorR > 0.0 ? std::max(static_cast<double>(v), floorR) : v;
      if (std::max(static_cast<double>(v), before) <= target + 1e-3)
        v = 0.0f;
      else
        v = static_cast<float>(target);
    }

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
      enter(li, p0.x, p0.r, p0.thetaDeg);
      for (size_t k = 1; k < path.size(); k++)
        go(Move::Kind::Cut, path[k].x, path[k].r, 90.0 - path[k].thetaDeg,
           tool.feedCut);
      go(Move::Kind::Rapid, path.back().x, safeR, 90.0 - path.back().thetaDeg,
         0.0);
      continue;
    }

    if (lead != 0.0) {
      RotaryHelicalRequest req;
      req.lead_mm = lead;
      req.stepover_mm = pitch;
      req.sampleStep_mm = op.sampleStep;
      req.tolerance_mm = cusp * 0.25;
      std::vector<std::vector<RotaryPathPoint>> passes =
          RotaryHelicalPasses(layer, req);
      if (passes.empty())
        continue;
      // Each layer starts on the turn the last one ended on: a pass winds A
      // by the twist's turns and zigzag unwinds it, but an odd number of
      // passes leaves the axis a turn or two round.
      const double shift =
          360.0 *
          std::round((thetaCarry - passes.front().front().thetaDeg) / 360.0);
      for (std::vector<RotaryPathPoint>& pass : passes) {
        for (RotaryPathPoint& q : pass)
          q.thetaDeg += shift;
        const RotaryPathPoint& p0 = pass.front();
        enter(li, p0.x, p0.r, p0.thetaDeg);
        for (size_t k = 1; k < pass.size(); k++)
          go(Move::Kind::Cut, pass[k].x, pass[k].r, 90.0 - pass[k].thetaDeg,
             tool.feedCut);
        go(Move::Kind::Rapid, pass.back().x, safeR, 90.0 - pass.back().thetaDeg,
           0.0);
        thetaCarry = pass.back().thetaDeg;
      }
      continue;
    }

    RotaryParallelRequest req;
    req.stepover_mm = pitch;
    req.sampleStep_mm = op.sampleStep;
    // Neighbouring passes LINKED at depth: a zigzag pass ends at the end the
    // next one starts, a stepover round, and turning A there with the tool
    // down cuts only the strip that pass cuts anyway. Retracting to the safe
    // radius and plunging again for every pass was 55 m of rapids on the
    // rotary sampler. Only within a layer, only between ends that meet, and
    // only where the arc between them stays clear of the part's surface.
    bool down = false;
    double lastX = 0.0, lastR = 0.0, lastTheta = 0.0;
    const auto clearArc = [&](double x, double r0, double t0, double r1,
                              double t1) {
      const int i = std::clamp(static_cast<int>((x - layer.x0) / layer.res), 0,
                               layer.nx - 1);
      constexpr int kSteps = 16;
      for (int k = 0; k <= kSteps; k++) {
        const double f = static_cast<double>(k) / kSteps;
        const double th = glm::radians(t0 + (t1 - t0) * f);
        const double step = 2.0 * std::numbers::pi / layer.nTheta;
        int j = static_cast<int>(std::floor(th / step));
        j = ((j % layer.nTheta) + layer.nTheta) % layer.nTheta;
        if (cl.At(i, j) > r0 + (r1 - r0) * f + 1e-3)
          return false;
      }
      return true;
    };
    for (const RotaryPass& pass : RotaryParallelPasses(layer, req)) {
      if (pass.pts.size() < 2)
        continue;
      const double a = 90.0 - pass.thetaDeg;
      const glm::dvec2 start = pass.pts.front();
      const bool link =
          down && std::abs(start.x - lastX) <= tool.diameter * 0.5 &&
          std::abs(glm::radians(pass.thetaDeg - lastTheta)) * blankR <=
              2.0 * pitch + 1e-6 &&
          clearArc(start.x, lastR, lastTheta, start.y, pass.thetaDeg);
      if (link) {
        go(Move::Kind::Cut, start.x, start.y, a, tool.feedCut);
      } else {
        if (down)
          go(Move::Kind::Rapid, lastX, safeR, 90.0 - lastTheta, 0.0);
        enter(li, start.x, start.y, pass.thetaDeg);
      }
      for (size_t k = 1; k < pass.pts.size(); k++)
        go(Move::Kind::Cut, pass.pts[k].x, pass.pts[k].y, a, tool.feedCut);
      down = true;
      lastX = pass.pts.back().x;
      lastR = pass.pts.back().y;
      lastTheta = pass.thetaDeg;
    }
    if (down)
      go(Move::Kind::Rapid, lastX, safeR, 90.0 - lastTheta, 0.0);
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

  // A ROUGH (v125, docs/cam/operations.md#rough-a-surface-and-rest-machining):
  // the raster in layers from the stock top. Over the whole part its region is
  // the part's outline plus a ring one cutter wide, cut down to the op's
  // bottom, so the finishing ball can reach the part's edges -- and no further,
  // so the rest of the block stays to hold the part.
  const bool rough = op.type == OpType::SurfaceParallel && op.stepdown > 0.0;
  if (rough && op.targets.empty()) {
    std::vector<Loop> outer;
    for (Loop l : region.loops)
      if (!l.hole) {
        MakeCounterClockwise(l); // Offset grows a CCW loop, shrinks a CW one
        outer.push_back(std::move(l));
      }
    const Clipper2Lib::Paths64 ring = Offset(ToClipper(outer), tool.diameter);
    region.loops.clear();
    for (const Clipper2Lib::Path64& path : ring) {
      Loop l;
      l.pts = FromClipper(path);
      region.loops.push_back(std::move(l));
    }
    if (region.loops.empty()) {
      tp.error = "the part's outline gave no region to rough";
      return;
    }
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
  // A finish lifts over where the tool reaches nothing: a floor below the
  // bottom marks those cells. A rough CUTS there -- the ring round the part
  // comes down to the op's bottom -- so its floor IS the bottom, and the
  // drop cutter returns the true surface of "the part on a floor": the
  // higher of the two at every cell, with the ball rolling off the part's
  // edge onto it continuously. Substituting the bottom into cells marked
  // unreached instead put a step in the surface beside the edge, and the
  // interpolation across it dipped the ball 1.1 mm into the part.
  drop.floorZ = rough ? op.bottomZ : op.bottomZ - 1.0;
  CLSurface cl = DropCut(bodies, tool, drop);
  if (!cl.ok) {
    tp.error = cl.why;
    return;
  }
  const auto unreached = [&](double z) { return z <= drop.floorZ + 1e-6; };

  // Where a pass may rapid down to: the stock top when nothing more is
  // known, which is `RapidFloor`'s rule for the 2.5D ops -- above the stock
  // there is no material (docs/cam/operations.md#surface-entries).
  const double clear = EntryClearance(tool);
  double stockTop = setup.safeZ;
  {
    glm::dvec3 slo(0.0), shi(0.0);
    if (StockBoundsFramed(setup, bodies, slo, shi))
      stockTop = std::min(setup.safeZ, shi.z + clear);
  }
  std::function<double(const glm::dvec3&)> floorAt = [&](const glm::dvec3&) {
    return stockTop;
  };
  std::vector<float> restTop; // what the bigger cutter left, NaN where nothing

  // Rest machining: cut only where the larger cutter that finished before
  // could not reach, by comparing what each LEAVES (ToolEnvelope). The
  // raster's: a clean-up after a waterline is a raster too (DeriveRest),
  // because the region, not the pattern, is the point.
  if (op.restFromTool != 0 && op.type == OpType::SurfaceParallel) {
    const Tool* bigger = setup.FindTool(op.restFromTool);
    if (bigger == nullptr) {
      tp.error = "the cutter it cleans up after, T" +
                 std::to_string(op.restFromTool) + ", is not in this setup";
      return;
    }
    DropRequest same = drop;
    same.res = cl.z.res; // one grid for both, or the comparison is noise
    const CLSurface bigCl = DropCut(bodies, *bigger, same);
    if (!bigCl.ok) {
      tp.error = bigCl.why;
      return;
    }
    const HeightMap& a = cl.z;
    const HeightMap& b = bigCl.z;
    const auto bigAt = [&](int i, int j) {
      const int bi =
          std::clamp(static_cast<int>(std::floor((a.CellX(i) - b.x0) / b.res)),
                     0, b.nx - 1);
      const int bj =
          std::clamp(static_cast<int>(std::floor((a.CellY(j) - b.y0) / b.res)),
                     0, b.ny - 1);
      return static_cast<double>(b.At(bi, bj));
    };
    // The part's own surface, sampled with a point: a flat probe one cell
    // across on the same grid.
    Tool probe;
    probe.type = ToolType::Flat;
    probe.diameter = a.res;
    const CLSurface surface = DropCut(bodies, probe, same);
    const auto partAt = [&](int i, int j) {
      if (!surface.ok)
        return std::nan("");
      const HeightMap& m = surface.z;
      const int si =
          std::clamp(static_cast<int>(std::floor((a.CellX(i) - m.x0) / m.res)),
                     0, m.nx - 1);
      const int sj =
          std::clamp(static_cast<int>(std::floor((a.CellY(j) - m.y0) / m.res)),
                     0, m.ny - 1);
      const double z = m.At(si, sj);
      return unreached(z) ? std::nan("") : z;
    };
    const std::vector<bool> cut = RestMask(
        a.nx, a.ny, a.res, [&](int, int) { return a.res; }, false, bigAt,
        [&](int i, int j) { return !unreached(bigAt(i, j)); },
        [&](int i, int j) { return static_cast<double>(a.At(i, j)); },
        [&](int i, int j) { return !unreached(a.At(i, j)); }, partAt, *bigger,
        tool, std::max(0.02, 2.0 * DerivedCusp(tool, 0.0)), &restTop);
    size_t kept = 0;
    for (int j = 0; j < a.ny; j++)
      for (int i = 0; i < a.nx; i++) {
        if (!cut[static_cast<size_t>(j) * a.nx + i])
          cl.z.Set(i, j, static_cast<float>(drop.floorZ));
        else if (!unreached(cl.z.At(i, j)))
          kept++;
      }
    if (kept == 0) {
      tp.error = "nothing is left for it: T" + std::to_string(op.restFromTool) +
                 " reached everywhere this cutter can";
      return;
    }
    // After its finish, a rest pass enters just above the leftover under the
    // tool -- not from the stock top, through the air the finish cut. Only
    // when that finish is cut FIRST: in any other order the stock is whole.
    if (const Operation* finish = FinishedBefore(setup, op)) {
      const double above =
          finish->stockToLeave + DerivedCusp(*bigger, 0.0) + clear;
      const double R = 0.5 * tool.diameter;
      floorAt = [&restTop, &cl, stockTop, clear, above,
                 R](const glm::dvec3& p) {
        const HeightMap& m = cl.z;
        const int ci = static_cast<int>(std::floor((p.x - m.x0) / m.res));
        const int cj = static_cast<int>(std::floor((p.y - m.y0) / m.res));
        const double top =
            FootprintMax(restTop, m.nx, m.ny, m.res, m.res, false, ci, cj, R);
        return std::min(stockTop, std::max(p.z + clear, top + above));
      };
    }
  }

  // Leaving stock on a FINISHING pass raises the tool, it does not lower the
  // bottom: the cut is a surface, not a depth, so the allowance is a distance
  // above it. (On a 2.5D op the same field shifts `bottomZ`, which is the
  // right answer there and the wrong one here.)
  const double lift = op.stockToLeave;

  if (rough) {
    // What each cell comes down to: the surface plus the skin, or the floor
    // where the ring reaches it.
    HeightMap target = cl.z;
    double lowest = 1e18;
    for (int j = 0; j < target.ny; j++)
      for (int i = 0; i < target.nx; i++) {
        const double z = cl.z.At(i, j);
        const double t = z <= op.bottomZ + 1e-6 ? op.bottomZ : z + lift;
        target.Set(i, j, static_cast<float>(t));
        lowest = std::min(lowest, t);
      }
    // A layer's cut-already cells are marked BELOW the floor, the one value
    // no real cell of this surface holds.
    const double masked = op.bottomZ - 1.0;
    std::vector<double> floors;
    for (double f = op.topZ - op.stepdown; f > lowest + 1e-6; f -= op.stepdown)
      floors.push_back(f);
    floors.push_back(-1e18); // the last layer: the target itself
    ParallelRequest req;
    req.stepover_mm = std::clamp(op.stepover, 0.02, 0.95) * tool.diameter;
    req.sampleStep_mm = op.sampleStep;
    req.cutAngleDeg = op.cutAngleDeg;
    req.floorZ = masked;
    double above = op.topZ; // where the previous layer left each cell, at most
    size_t emitted = 0;
    const double ridge = Scallop(tool, req.stepover_mm);
    for (const double f : floors) {
      // The first layer enters from the stock top; a later one from just
      // over the floor the one before left, ridges and all.
      // Under the whole footprint, the higher of that floor and the target:
      // a cell beside a wall is never under a tool centre and keeps what the
      // bodies round it left, up to its own target.
      const bool first = above >= op.topZ;
      const double floorBefore = above;
      const double R = 0.5 * tool.diameter;
      const std::function<double(const glm::dvec3&)> layerAt =
          [&, first, floorBefore, R](const glm::dvec3& p) {
            if (first)
              return stockTop;
            const int ci =
                static_cast<int>(std::floor((p.x - target.x0) / target.res));
            const int cj =
                static_cast<int>(std::floor((p.y - target.y0) / target.res));
            const double top = std::max(
                floorBefore,
                FootprintMax(target.z, target.nx, target.ny, target.res,
                             target.res, false, ci, cj, R));
            return std::min(stockTop, std::max(p.z, top + ridge) + clear);
          };
      // A cell whose target already stands above the previous floor has
      // nothing left over it: unreached for this layer, so the pass breaks
      // there instead of dragging the tool through air it cut already.
      CLSurface layer = cl;
      for (int j = 0; j < target.ny; j++)
        for (int i = 0; i < target.nx; i++) {
          const double t = target.At(i, j);
          layer.z.Set(
              i, j,
              static_cast<float>(t >= above - 1e-6 ? masked : std::max(t, f)));
        }
      SurfacePaths r = ParallelPasses(layer, region.loops, req);
      if (!r.passes.empty()) {
        emitted += r.passes.size();
        EmitSurfacePasses(tp, setup, tool, r.passes, req.stepover_mm * 2.0,
                          layerAt);
      }
      above = f;
    }
    if (emitted == 0)
      tp.error = "the stock stands no higher than the part here: nothing to "
                 "rough";
    return;
  }

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
    // How far apart two laps may be across the surface: the spacing that
    // leaves the finish's own cusp -- 2 sqrt(2rc - c^2) for a ball -- or half
    // the cutter for a flat one, which has no cusp to solve for.
    {
      const double r = tool.diameter * 0.5;
      const double c = std::min(DerivedCusp(tool, 0.0), r);
      req.maxGap_mm = tool.type == ToolType::Ball
                          ? 2.0 * std::sqrt(std::max(0.0, 2.0 * r * c - c * c))
                          : r;
    }
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
  EmitSurfacePasses(tp, setup, tool, passes, step * 2.0, floorAt);
}

} // namespace

namespace detail {

// A surfacing operation, from the cache, from a worker later, or right here
// -- the same three-way the adaptive pass has, one level up
// (docs/ui/long-operations.md#cam-toolpath-passes). The unit is the whole
// OPERATION because a finishing pass is one computation from the
// cutter-location surface outward, with no interior seam to stop at.
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

const Operation* FinishedBefore(const Setup& setup, const Operation& op) {
  if (op.restFromTool == 0)
    return nullptr;
  const bool rotary = op.type == OpType::SurfaceRotary;
  for (const size_t i : CuttingOrder(setup)) {
    const Operation& o = setup.operations[i];
    if (o.id == op.id)
      return nullptr;
    const bool finishes = rotary ? o.type == OpType::SurfaceRotary
                                 : (o.type == OpType::SurfaceParallel ||
                                    o.type == OpType::SurfaceWaterline);
    if (o.enabled && finishes && o.toolNumber == op.restFromTool)
      return &o;
  }
  return nullptr;
}

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
