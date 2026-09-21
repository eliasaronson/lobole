#include "core/cam/CamRotary.h"

#include "core/Parallel.h"

#include "core/cam/CamToolpath.h" // BodiesInSetupFrame

#include <algorithm>
#include <cmath>
#include <numbers>

namespace cam {

namespace {

constexpr double kTwoPi = 2.0 * std::numbers::pi;

// The same tolerance the height map's rapid check uses, for the same reason:
// a cell standing a micron proud of the tool is quantisation, not a crash.
constexpr double kRotaryGrazeTolerance = 1e-3;

// Leftover shallower than this is the grid's own quantisation, not material.
// A tenth of a millimetre is below anything a finishing pass is asked for and
// above what a half-millimetre grid can resolve.
constexpr double kLeftoverFloor = 0.1;

// Is the part-frame point at (x, theta, rad) inside the tool, with the tool's
// tip at `tip` and the part turned to `aRad`?
//
// The tool is the same solid the planar carve assumes: a body of revolution
// about setup-space Z, opening upward without limit, whose profile is
// `TipHeight`. "Without limit" is what makes a plunge remove the column above
// it, and it is as true here as there.
// `cphi`/`sphi` are cos and sin of theta + A, taken by the CALLER once per
// ray. The bisection below asks this forty times along one ray whose
// direction never changes, and computing the direction here cost eighty trig
// calls a cell -- the bulk of a rotary verify once the stock scan was gone.
bool InsideTool(double x, double cphi, double sphi, double rad,
                const glm::dvec3& tip, const Tool* tool, double toolR) {
  const double y = rad * cphi;
  const double z = rad * sphi;
  const double dx = x - tip.x, dy = y - tip.y;
  const double d = std::hypot(dx, dy);
  if (d > toolR)
    return false;
  const double surface = tip.z + (tool != nullptr ? TipHeight(*tool, d) : 0.0);
  return z > surface;
}

// The largest radius at or below `r0` that is NOT inside the tool.
//
// The ray from the axis outward enters the tool at most once for the shapes
// this app models -- they are convex upward about a single axis -- so the
// answer is a single boundary and bisection finds it. Two ends are answered
// without searching: an axis point inside the tool means the whole ray is
// gone (the cutter has passed the centreline, which is a real thing a rotary
// job does and the reason this is not just `min`), and a tip that does not
// reach `r0` leaves it alone.
double CarveRay(double x, double cphi, double sphi, double r0,
                const glm::dvec3& tip, const Tool* tool, double toolR) {
  if (!InsideTool(x, cphi, sphi, r0, tip, tool, toolR))
    return r0;
  if (InsideTool(x, cphi, sphi, 0.0, tip, tool, toolR))
    return 0.0;
  double lo = 0.0, hi = r0; // lo outside, hi inside
  for (int k = 0; k < 40; k++) {
    const double mid = 0.5 * (lo + hi);
    if (InsideTool(x, cphi, sphi, mid, tip, tool, toolR))
      hi = mid;
    else
      lo = mid;
  }
  return lo;
}

} // namespace

RadiusMap MakeRotaryStock(double x0, double x1, double radius, double res) {
  RadiusMap map;
  map.res = std::max(0.01, res);
  map.x0 = std::min(x0, x1);
  map.nx =
      std::max(1, static_cast<int>(std::ceil(std::abs(x1 - x0) / map.res)));
  // Derived, not typed: enough steps that a cell's arc AT THE BLANK'S SURFACE
  // is about one `res`. A literal step would be right at one diameter and
  // wrong at every other, which is the defaults rule this project enforces on
  // every other unit-carrying number.
  const double circumference = kTwoPi * std::max(0.01, radius);
  map.nTheta = std::clamp(static_cast<int>(std::ceil(circumference / map.res)),
                          8, 20000);
  map.r.assign(static_cast<size_t>(map.nx) * static_cast<size_t>(map.nTheta),
               static_cast<float>(std::max(0.0, radius)));
  map.rMax = std::max(0.0, radius);
  return map;
}

RadiusMap SampleModelRotary(const std::vector<Body>& bodies,
                            const RadiusMap& like) {
  RadiusMap out = like;
  out.r.assign(out.r.size(), 0.0f);
  if (out.nx <= 0 || out.nTheta <= 0)
    return out;

  const double deflection = like.res > 0.0 ? like.res * 0.25 : 0.0;
  const auto splat = [&](const glm::dvec3& p) {
    const double rad = std::hypot(p.y, p.z);
    const int i = static_cast<int>((p.x - out.x0) / out.res);
    if (i < 0 || i >= out.nx)
      return;
    double theta = std::atan2(p.z, p.y);
    if (theta < 0.0)
      theta += kTwoPi;
    int j = static_cast<int>(theta / kTwoPi * out.nTheta);
    j = std::clamp(j, 0, out.nTheta - 1);
    if (rad > out.At(i, j))
      out.Set(i, j, static_cast<float>(rad));
  };

  for (const Body& body : bodies) {
    if (!body.shape.IsValid())
      continue;
    // Detached, for the same reason the planar sampler is: the triangulation
    // rides on the shared TShape, so meshing the body itself at the CHECK's
    // tolerance would make the renderer's mesh of the same part fine too.
    const CADShape::Mesh mesh = body.shape.GenerateMeshDetached(deflection);
    for (size_t tri = 0; tri + 2 < mesh.indices.size(); tri += 3) {
      const glm::dvec3 p0(mesh.vertices[mesh.indices[tri]].position);
      const glm::dvec3 p1(mesh.vertices[mesh.indices[tri + 1]].position);
      const glm::dvec3 p2(mesh.vertices[mesh.indices[tri + 2]].position);
      // How finely to walk this triangle: half a cell, measured in the
      // coarser of the two directions it spans. A triangle near the axis
      // needs fewer angular samples than one at the rim, which is exactly
      // what measuring the ARC rather than the angle gives.
      const double rad =
          std::max({std::hypot(p0.y, p0.z), std::hypot(p1.y, p1.z),
                    std::hypot(p2.y, p2.z)});
      const double step = std::max(0.25 * out.res, 0.5 * out.ArcAt(rad));
      // One count per EDGE, not one for the triangle. A cylinder's side
      // meshes as long thin triangles -- 60 mm by a fraction of one -- and a
      // single count from the longest edge either samples the short direction
      // absurdly finely or, once clamped, leaves whole cells between samples
      // along the long one. That is how a plain bar first sampled as a
      // radius of ZERO in places.
      const int nu = std::clamp(
          static_cast<int>(std::ceil(glm::length(p1 - p0) / step)), 1, 4096);
      const int nv = std::clamp(
          static_cast<int>(std::ceil(glm::length(p2 - p0) / step)), 1, 4096);
      for (int a = 0; a <= nu; a++)
        for (int b = 0; b <= nv; b++) {
          const double u = static_cast<double>(a) / nu;
          const double v = static_cast<double>(b) / nv;
          if (u + v > 1.0)
            continue; // outside the triangle
          splat(p0 + (p1 - p0) * u + (p2 - p0) * v);
        }
    }
  }
  return out;
}

namespace {

// The one walk, used to CUT and to ASK. A rapid must be tested against the
// stock without changing it -- the planar check keeps a separate probe map
// for exactly this -- and doing it with a second copy of the stepping and
// the ray solve is how the two would come to disagree about what the tool
// removes, which is the failure this whole module is arranged to prevent.
struct SweepHit {
  double depth = 0.0; // mm of material the tool is buried in
  glm::dvec2 at{0.0}; // (x mm, theta degrees)
};

SweepHit WalkSweepRotary(const glm::dvec3& from, const glm::dvec3& to,
                         double aFrom, double aTo, const Tool* tool,
                         RadiusMap& stock, bool apply) {
  SweepHit hit;
  if (stock.nx <= 0 || stock.nTheta <= 0)
    return hit;
  const double toolR = tool != nullptr ? tool->diameter * 0.5 : 1.0;
  // The blank's radius, not a scan for the current widest: see `rMax`.
  const double rMax = stock.rMax;
  if (rMax <= 0.0)
    return hit;

  // A rotary move is a HELIX at the surface, not a straight line, so it is
  // walked in steps small enough that neither the tip nor the material under
  // it moves more than half a cell. The angular term uses the arc at the
  // stock's widest point, because that is where a given rotation moves the
  // most metal past the cutter -- stepping by the ANGLE instead would sample
  // a 200 mm blank as finely as a 20 mm one.
  const double travel = glm::length(to - from);
  const double turn = std::abs(aTo - aFrom) / 360.0 * kTwoPi * rMax;
  const int steps = std::clamp(
      static_cast<int>(std::ceil(std::max(travel, turn) / (0.5 * stock.res))),
      1, 100000);

  for (int s = 0; s <= steps; s++) {
    const double t = static_cast<double>(s) / steps;
    const glm::dvec3 tip = from + (to - from) * t;
    const double aRad = glm::radians(aFrom + (aTo - aFrom) * t);

    // Only the cells the tool can touch. Along X that is its own radius; in
    // theta it is every cell whose ray can rise into the cutter, which is the
    // half turn facing the spindle -- the other half is below the axis and
    // out of reach of a tool that only comes down.
    const int i0 =
        std::max(0, static_cast<int>((tip.x - toolR - stock.x0) / stock.res));
    const int i1 =
        std::min(stock.nx - 1,
                 static_cast<int>((tip.x + toolR - stock.x0) / stock.res) + 1);

    // ...and in theta, only the BAND the cutter can reach. It walked every
    // row, and a 6 mm cutter over a 25 mm radius touches about fourteen of
    // three hundred and forty -- which, with the stock scan gone, was most of
    // a rotary verify: 263 us a move, three minutes on the rolling pin, and
    // on the frame once its passes landed.
    //
    // The cutter occupies |y - tip.y| <= R above the tip, and seen from the
    // axis that region spans the angles between its two LOWER corners. A ball
    // or a cone rises from there, which only pulls its rays towards the top,
    // so the band from the tip is a superset of the band the shape needs --
    // conservative by construction, never short. Below the axis the region
    // wraps most of the way round, so the whole half-turn is walked as
    // before.
    const int n = stock.nTheta;
    const double dTheta = kTwoPi / n;
    int jFirst = 0, jCount = n;
    if (tip.z > 1e-6) {
      const double phiLo = std::atan2(tip.z, tip.y + toolR);
      const double phiHi = std::atan2(tip.z, tip.y - toolR);
      // Two rows of margin either side for the cell-centre discretisation.
      jFirst = static_cast<int>(std::floor((phiLo - aRad) / dTheta)) - 2;
      const int jLast =
          static_cast<int>(std::ceil((phiHi - aRad) / dTheta)) + 2;
      jCount = std::min(jLast - jFirst + 1, n);
    }
    for (int i = i0; i <= i1; i++) {
      const double x = stock.CellX(i);
      if (std::abs(x - tip.x) > toolR)
        continue;
      for (int k = 0; k < jCount; k++) {
        const int j = ((jFirst + k) % n + n) % n;
        const double theta = stock.CellTheta(j);
        const double sphi = std::sin(theta + aRad);
        // Cheap rejection before the bisection: a ray pointing away from the
        // spindle cannot meet a tool that only descends.
        if (sphi <= 0.0)
          continue;
        const double r0 = stock.At(i, j);
        if (r0 <= 0.0)
          continue;
        const double cphi = std::cos(theta + aRad);
        const double cut = CarveRay(x, cphi, sphi, r0, tip, tool, toolR);
        if (cut >= r0)
          continue;
        if (apply) {
          stock.Set(i, j, static_cast<float>(cut));
        } else if (const double into = r0 - cut; into > hit.depth) {
          hit.depth = into;
          hit.at = {x, glm::degrees(theta)};
        }
      }
    }
  }
  return hit;
}

} // namespace

void CarveSweepRotary(const glm::dvec3& from, const glm::dvec3& to,
                      double aFrom, double aTo, const Tool* tool,
                      RadiusMap& stock) {
  WalkSweepRotary(from, to, aFrom, aTo, tool, stock, /*apply=*/true);
}

RadiusMap RotaryCL(const RadiusMap& model, const Tool& tool) {
  RadiusMap out = model;
  if (model.nx <= 0 || model.nTheta <= 0 || tool.diameter <= 0.0)
    return out;
  const double R = tool.diameter * 0.5;
  const double R2 = R * R;
  const int di = static_cast<int>(std::ceil(R / model.res)) + 1;
  const int n = model.nTheta;

  // Over THETA ROWS, each writing only its own: nothing is shared, so the
  // rows are independent exactly as the planar drop cutter's are, and it
  // uses the same `parallel::For`. This function ran on one core and `perf`
  // put it -- with the `hypot` inside it -- at ~100% of generating the
  // rolling pin's finishing pass: 27.7 s on that grid, 2,014 x 1,055 cells
  // under a ~1,450-evaluation cutter kernel each.
  parallel::For(static_cast<size_t>(n), [&](size_t jj) {
    const int j = static_cast<int>(jj);
    for (int i = 0; i < model.nx; i++) {
      // The arc a theta step spans depends on the radius, so it is measured
      // at the cell's OWN. A kernel in cells would be a different physical
      // size at every radius, which is the conflation the whole `ArcAt`
      // derivation exists to avoid.
      const double here = model.At(i, j);
      const double arc = model.ArcAt(std::max(here, 0.01));
      // Never more than half way round: near the axis a cell's arc is tiny,
      // the kernel would otherwise wrap onto itself, and the wrap below
      // assumes one lap at most.
      const int dj = std::min(
          static_cast<int>(std::ceil(R / std::max(arc, 1e-6))) + 1, n / 2);
      double best = 0.0;
      bool touched = false;
      for (int a = -di; a <= di; a++) {
        const int ni = i + a;
        if (ni < 0 || ni >= model.nx)
          continue;
        const double ax = a * model.res;
        const double ax2 = ax * ax;
        if (ax2 > R2)
          continue;
        for (int b = -dj; b <= dj; b++) {
          // Squared, and the root taken only for a cell that is inside the
          // cutter. `std::hypot` was a third of the profile on its own: it is
          // careful about overflow, which a distance of a few millimetres
          // never needs, and it was paid for every cell of the square kernel
          // including the fifth of them outside the circle.
          const double by = b * arc;
          const double d2 = ax2 + by * by;
          if (d2 > R2)
            continue;
          int nj = j + b;
          if (nj < 0)
            nj += n;
          else if (nj >= n)
            nj -= n;
          const double s = model.At(ni, nj);
          if (s <= 0.0)
            continue; // off the part: nothing there to rest on
          const double tip = s - TipHeight(tool, std::sqrt(d2));
          if (!touched || tip > best) {
            best = tip;
            touched = true;
          }
        }
      }
      out.Set(i, j, static_cast<float>(touched ? best : 0.0));
    }
  });
  return out;
}

Verdict CompareRotary(const RadiusMap& machined, const RadiusMap& designed) {
  Verdict v;
  v.rotary = true;
  if (machined.nx != designed.nx || machined.nTheta != designed.nTheta ||
      machined.nx <= 0)
    return v;
  // A cell that STRADDLES a step is measured against its neighbours, not
  // against itself.
  //
  // The sampler takes the furthest point of the part in each cell, exactly as
  // the height map takes the highest. At a shoulder -- a 5 mm step on a
  // half-millimetre grid -- the boundary cell therefore reads as the BIG
  // diameter while the cutter, which is 6 mm across and does not stop at a
  // cell edge, legitimately turns it down to the small one. Compared cell to
  // cell that is 5 mm of "gouge" on a correct program, and a verifier that
  // cries wolf at every shoulder is one nobody reads.
  //
  // So gouge is measured against the SMALLEST designed radius among a cell
  // and its immediate neighbours, and leftover against the largest. It costs
  // one cell of resolution at a vertical face, which is precisely what the
  // grid cannot resolve there anyway: nothing in the map says which side of
  // the step the cell centre fell on.
  const auto neighbourhood = [&](int i, int j, double& lo, double& hi) {
    lo = 1e18;
    hi = -1e18;
    for (int di = -1; di <= 1; di++)
      for (int dj = -1; dj <= 1; dj++) {
        const int ni = i + di;
        if (ni < 0 || ni >= designed.nx)
          continue;
        const int nj = (j + dj + designed.nTheta) % designed.nTheta;
        const double r = designed.At(ni, nj);
        if (r <= 0.0)
          continue;
        lo = std::min(lo, r);
        hi = std::max(hi, r);
      }
  };
  size_t compared = 0, missed = 0;
  for (int j = 0; j < machined.nTheta; j++)
    for (int i = 0; i < machined.nx; i++) {
      const double want = designed.At(i, j);
      if (want <= 0.0) {
        missed++;
        continue; // the ray misses the part: blank, not leftover
      }
      compared++;
      const double got = machined.At(i, j);
      const double x = machined.CellX(i);
      const double theta = glm::degrees(machined.CellTheta(j));
      double lo = want, hi = want;
      neighbourhood(i, j, lo, hi);
      if (const double gouge = lo - got; gouge > v.maxGouge) {
        v.maxGouge = gouge;
        v.gougeAt = {x, theta};
      }
      if (const double left = got - hi; left > v.maxLeftover) {
        v.maxLeftover = left;
        v.leftoverAt = {x, theta};
      }
      // How MUCH, as well as how deep. A cell's area here is its axial size
      // by its ARC at the radius it sits at, which is why the sum is taken
      // cell by cell rather than as a count times a constant.
      if (got - hi > kLeftoverFloor)
        v.leftoverArea += machined.res * machined.ArcAt(got);
    }
  v.cells = compared;
  const size_t total = compared + missed;
  v.untouchedFraction =
      total > 0 ? static_cast<double>(missed) / static_cast<double>(total)
                : 0.0;
  return v;
}

RotaryInspection InspectRotary(const std::string& gcode, const Setup& setup,
                               const std::vector<Body>& rawBodies, double res) {
  RotaryInspection out;
  // Framed, exactly as the planar check frames: the file is in setup space,
  // so the part must be too. For a continuous job `rotationA` is 0 -- the
  // angle lives in the FILE, per move, not in the setup -- and the frame is
  // whatever fixture put the part on the trunnion.
  const std::vector<Body> bodies = BodiesInSetupFrame(setup, rawBodies);

  // The blank, derived from the part it is cut from. The axis is setup-space
  // +X through the origin, so "furthest from the axis" is hypot(y, z).
  double x0 = 1e18, x1 = -1e18, rad = 0.0;
  for (const Body& b : bodies) {
    if (!b.shape.IsValid())
      continue;
    const CADShape::Mesh mesh = b.shape.GenerateMeshDetached(0.0);
    for (const auto& vert : mesh.vertices) {
      x0 = std::min(x0, static_cast<double>(vert.position.x));
      x1 = std::max(x1, static_cast<double>(vert.position.x));
      rad = std::max(rad, std::hypot(static_cast<double>(vert.position.y),
                                     static_cast<double>(vert.position.z)));
    }
  }
  if (x1 <= x0 || rad <= 0.0) {
    out.why = "this job has no part on the rotary axis to cut from";
    return out;
  }
  // A box's margins are the only ones a `Stock` can state and a cylinder has
  // one radius, so the radial margin is the larger of the two across the
  // axis. Named here rather than silently chosen: a round stock MODE is the
  // honest fix (docs/cam/rotary-plan.md, D3).
  const double radialMargin =
      std::max(setup.stock.margin.y, setup.stock.margin.z);
  out.machined =
      MakeRotaryStock(x0 - setup.stock.margin.x, x1 + setup.stock.margin.x,
                      rad + radialMargin, res);
  out.designed = SampleModelRotary(bodies, out.machined);

  const GProgram program = ProgramInSetupSpace(gcode, setup);
  bool have = false;
  glm::dvec3 from(0.0);
  double aFrom = 0.0;
  int rapids = 0, firstBadLine = 0, worstLine = 0;
  double worstDepth = 0.0;
  glm::dvec2 worstAt(0.0);
  for (const GMove& m : program.moves) {
    const double aTo = m.hasA ? m.a : aFrom;
    if (!have) {
      from = m.to;
      aFrom = aTo;
      have = true;
      continue;
    }
    // A rapid is checked against the stock and removes nothing from it; a
    // cut removes and is not checked. Same rule as the height map, and the
    // same reason: a rapid is only safe over air, and it is the one failure
    // a shape comparison cannot make -- the part can come out perfect and
    // the tool still be broken. The waveguide round is why this is not
    // optional: the check found 121 rapids through material on a job whose
    // gouge was zero, and the fix took them to 16.
    if (m.rapid) {
      const SweepHit hit =
          WalkSweepRotary(from, m.to, aFrom, aTo, setup.FindTool(m.tool),
                          out.machined, /*apply=*/false);
      if (hit.depth > kRotaryGrazeTolerance) {
        rapids++;
        if (firstBadLine == 0)
          firstBadLine = m.line;
        if (hit.depth > worstDepth) {
          worstDepth = hit.depth;
          worstAt = hit.at;
          worstLine = m.line;
        }
      }
    } else {
      CarveSweepRotary(from, m.to, aFrom, aTo, setup.FindTool(m.tool),
                       out.machined);
    }
    from = m.to;
    aFrom = aTo;
  }

  out.verdict = CompareRotary(out.machined, out.designed);
  out.verdict.rapidsThroughStock = rapids;
  out.verdict.firstBadLine = firstBadLine;
  out.verdict.worstRapidDepth = worstDepth;
  out.verdict.worstRapidAt = worstAt;
  out.verdict.worstRapidLine = worstLine;
  out.verdict.fileErrors = static_cast<int>(program.errors.size());
  if (!program.errors.empty())
    out.verdict.firstFileError = program.errors.front();
  out.verdict.fileWarnings = static_cast<int>(program.warnings.size());
  if (!program.warnings.empty())
    out.verdict.firstFileWarning = program.warnings.front();
  out.ok = true;
  return out;
}

} // namespace cam
