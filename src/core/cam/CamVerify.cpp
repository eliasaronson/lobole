#include "core/cam/CamVerify.h"

#include "core/cam/CamRotary.h"
#include "core/cam/CamToolpath.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <limits>
#include <numbers>

namespace cam {

namespace {

// Distance from p to segment [a,b] in XY, plus the parameter t of the closest
// point. Exact — this is what replaces FreeCAD's 0.6-pixel path walk.
double DistToSegment(const glm::dvec2& p, const glm::dvec2& a,
                     const glm::dvec2& b, double& t) {
  const glm::dvec2 ab = b - a;
  const double len2 = glm::dot(ab, ab);
  t = len2 > 1e-18 ? std::clamp(glm::dot(p - a, ab) / len2, 0.0, 1.0) : 0.0;
  return glm::length(p - (a + ab * t));
}

// Flags every cell the swept tool covers on one cutting move. The same
// culling Simulate uses, but recording coverage rather than depth: it is what
// tells "the cutter left a corner" apart from "you have not programmed that
// part of the block yet".
void MarkSwept(const glm::dvec3& from, const GMove& m, const Setup& setup,
               const HeightMap& grid, std::vector<uint8_t>& touched) {
  const Tool* tool = setup.FindTool(m.tool);
  const double radius = tool != nullptr ? tool->diameter * 0.5 : 1.0;
  const glm::dvec2 a(from.x, from.y), b(m.to.x, m.to.y);
  const int i0 = std::max(
      0, static_cast<int>((std::min(a.x, b.x) - radius - grid.x0) / grid.res));
  const int i1 = std::min(
      grid.nx - 1,
      static_cast<int>((std::max(a.x, b.x) + radius - grid.x0) / grid.res) + 1);
  const int j0 = std::max(
      0, static_cast<int>((std::min(a.y, b.y) - radius - grid.y0) / grid.res));
  const int j1 = std::min(
      grid.ny - 1,
      static_cast<int>((std::max(a.y, b.y) + radius - grid.y0) / grid.res) + 1);
  for (int j = j0; j <= j1; j++)
    for (int i = i0; i <= i1; i++) {
      double t = 0.0;
      const glm::dvec2 c(grid.CellX(i), grid.CellY(j));
      if (DistToSegment(c, a, b, t) <= radius)
        touched[static_cast<size_t>(j) * static_cast<size_t>(grid.nx) +
                static_cast<size_t>(i)] = 1;
    }
}

} // namespace

HeightMap MakeStock(const glm::dvec3& min, const glm::dvec3& max, double res) {
  HeightMap map;
  map.res = std::max(0.01, res);
  map.x0 = min.x;
  map.y0 = min.y;
  map.nx = std::max(1, static_cast<int>(std::ceil((max.x - min.x) / map.res)));
  map.ny = std::max(1, static_cast<int>(std::ceil((max.y - min.y) / map.res)));
  map.z.assign(static_cast<size_t>(map.nx) * static_cast<size_t>(map.ny),
               static_cast<float>(max.z));
  return map;
}

void CarveSweep(const glm::dvec3& from, const glm::dvec3& to, const Tool* tool,
                HeightMap& stock) {
  const double radius = tool != nullptr ? tool->diameter * 0.5 : 1.0;

  // Cells the swept tool can touch, from the segment's bounding box grown
  // by the radius. Everything outside cannot be reached, so it is not
  // tested at all.
  const glm::dvec2 a(from.x, from.y), b(to.x, to.y);
  const bool xyStill = glm::length(b - a) < 1e-9;
  const double lo_x = std::min(a.x, b.x) - radius;
  const double hi_x = std::max(a.x, b.x) + radius;
  const double lo_y = std::min(a.y, b.y) - radius;
  const double hi_y = std::max(a.y, b.y) + radius;
  const int i0 = std::max(0, static_cast<int>((lo_x - stock.x0) / stock.res));
  const int i1 = std::min(stock.nx - 1,
                          static_cast<int>((hi_x - stock.x0) / stock.res) + 1);
  const int j0 = std::max(0, static_cast<int>((lo_y - stock.y0) / stock.res));
  const int j1 = std::min(stock.ny - 1,
                          static_cast<int>((hi_y - stock.y0) / stock.res) + 1);

  for (int j = j0; j <= j1; j++) {
    for (int i = i0; i <= i1; i++) {
      double t = 0.0;
      const glm::dvec2 c(stock.CellX(i), stock.CellY(j));
      const double d = DistToSegment(c, a, b, t);
      if (d > radius)
        continue;
      // The tool's own shape, cut by cut: flat mills are still the
      // interpolated Z everywhere under them (TipHeight is 0), a ball
      // leaves its bowl, a V-bit or a drill its cone. Verifying a
      // roundover against a flat-mill model reported the arc it MEANT to
      // leave as 100% gouge (R15).
      //
      // `t` is the parameter of the CLOSEST point on the XY segment, which
      // is 0 for a move that does not travel in XY at all — so a straight
      // plunge removed material down to where it STARTED, which is to say
      // none. Every drilled hole came out full of stock, and the retract
      // out of it was then reported as a rapid through material. A move
      // that stands still in XY reaches its lowest point over every cell it
      // covers.
      const double toolZ =
          xyStill ? std::min(from.z, to.z) : from.z + (to.z - from.z) * t;
      const double surface =
          toolZ + (tool != nullptr ? TipHeight(*tool, d) : 0.0);
      if (stock.At(i, j) > surface)
        stock.Set(i, j, static_cast<float>(surface));
    }
  }
}

void Simulate(const GProgram& program, const Setup& setup, HeightMap& stock) {
  glm::dvec3 from(0.0);
  bool have = false;
  for (const GMove& m : program.moves) {
    if (!have) {
      from = m.to;
      have = true;
      continue;
    }
    CarveSweep(from, m.to, setup.FindTool(m.tool), stock);
    from = m.to;
  }
}

HeightMap SampleModel(const std::vector<Body>& bodies, const HeightMap& like,
                      double floorZ) {
  HeightMap out = like;
  out.z.assign(out.z.size(), static_cast<float>(floorZ));

  // Mesh the reference surface finer than the grid that samples it. This is
  // the surface every gouge and leftover figure is measured AGAINST, so its
  // faceting is measurement noise, and noise coarser than a cell is noise the
  // comparison cannot tell from a real defect. A quarter of a cell puts it
  // below what the grid can resolve at all.
  //
  // Derived from the grid rather than typed: the caller picks `res` for the
  // job (`Inspect` takes it), and a literal here would be right at 0.25 mm and
  // wrong at every other resolution. Zero when there is no grid to derive
  // from, which asks the kernel for its own size-scaled default.
  const double deflection = like.res > 0.0 ? like.res * 0.25 : 0.0;

  for (const Body& body : bodies) {
    if (!body.shape.IsValid())
      continue;
    // Detached: this is the CHECK's tolerance, not the document's. Meshing
    // the body itself here would make the renderer's mesh of the same part
    // fine too, because the triangulation rides on the shared TShape.
    const CADShape::Mesh mesh = body.shape.GenerateMeshDetached(deflection);
    for (size_t tri = 0; tri + 2 < mesh.indices.size(); tri += 3) {
      const glm::vec3& p0 = mesh.vertices[mesh.indices[tri]].position;
      const glm::vec3& p1 = mesh.vertices[mesh.indices[tri + 1]].position;
      const glm::vec3& p2 = mesh.vertices[mesh.indices[tri + 2]].position;

      const double lo_x = std::min({p0.x, p1.x, p2.x});
      const double hi_x = std::max({p0.x, p1.x, p2.x});
      const double lo_y = std::min({p0.y, p1.y, p2.y});
      const double hi_y = std::max({p0.y, p1.y, p2.y});
      const int i0 = std::max(0, static_cast<int>((lo_x - out.x0) / out.res));
      const int i1 =
          std::min(out.nx - 1, static_cast<int>((hi_x - out.x0) / out.res) + 1);
      const int j0 = std::max(0, static_cast<int>((lo_y - out.y0) / out.res));
      const int j1 =
          std::min(out.ny - 1, static_cast<int>((hi_y - out.y0) / out.res) + 1);

      // Barycentric point-in-triangle, then the height there: dropping a ray
      // per cell over the whole mesh would be the same work with more code.
      const double d =
          (p1.y - p2.y) * (p0.x - p2.x) + (p2.x - p1.x) * (p0.y - p2.y);
      if (std::abs(d) < 1e-12)
        continue; // edge-on triangle contributes no height
      for (int j = j0; j <= j1; j++) {
        for (int i = i0; i <= i1; i++) {
          const double x = out.CellX(i), y = out.CellY(j);
          const double l0 =
              ((p1.y - p2.y) * (x - p2.x) + (p2.x - p1.x) * (y - p2.y)) / d;
          const double l1 =
              ((p2.y - p0.y) * (x - p2.x) + (p0.x - p2.x) * (y - p2.y)) / d;
          const double l2 = 1.0 - l0 - l1;
          if (l0 < -1e-9 || l1 < -1e-9 || l2 < -1e-9)
            continue;
          const double z = l0 * p0.z + l1 * p1.z + l2 * p2.z;
          if (z > out.At(i, j))
            out.Set(i, j, static_cast<float>(z));
        }
      }
    }
  }
  return out;
}

Verdict Compare(const HeightMap& machined, const HeightMap& designed,
                double floorZ, const std::vector<uint8_t>* touched,
                double leftoverTolerance = 0.0,
                const std::vector<Toolpath::Relief>& reliefs = {},
                const std::vector<Toolpath::Tab>& tabs = {}) {
  Verdict v;
  if (machined.nx != designed.nx || machined.ny != designed.ny)
    return v;
  size_t untouched = 0;
  const size_t total =
      static_cast<size_t>(machined.nx) * static_cast<size_t>(machined.ny);
  const double leftTol =
      leftoverTolerance > 0.0 ? leftoverTolerance : machined.res * 1.5;
  std::vector<uint8_t> over(total, 0);
  for (int j = 0; j < machined.ny; j++) {
    for (int i = 0; i < machined.nx; i++) {
      const size_t k =
          static_cast<size_t>(j) * static_cast<size_t>(machined.nx) +
          static_cast<size_t>(i);
      if (touched != nullptr && touched->size() == total &&
          (*touched)[k] == 0) {
        untouched++;
        continue; // nobody asked the program to cut here
      }
      const double rawGot = machined.At(i, j);
      v.cells++;
      // Below the stock there is no part to scrap: that is the spoilboard,
      // and a through cut is SUPPOSED to reach it. Recorded separately and
      // clamped out of the gouge test — see Verdict::belowStock for what
      // conflating the two told the operator about correct G-code.
      if (floorZ - rawGot > v.belowStock) {
        v.belowStock = floorZ - rawGot;
        v.belowStockAt = {machined.CellX(i), machined.CellY(j)};
      }
      const double got = std::max(rawGot, floorZ);
      // A cell that straddles a vertical WALL has no single designed height:
      // the tool runs tangent to the wall, and whether the cell counts as
      // inside or outside is decided by where its centre happens to fall.
      // Compared cell-to-cell, one such cell reported the wall's whole height
      // as a gouge — the panel said "cuts 6.00 mm INTO the part, that is
      // scrap" about a part the RPC's own check called clean, because their
      // grids landed differently. So a cell is gouged only if it is below
      // everything ITS NEIGHBOURS were designed to be, and left over only if
      // it is above all of them. In the middle of a face all nine agree and
      // nothing changes; on a wall the ambiguity resolves in favour of not
      // crying wolf.
      double wantLo = designed.At(i, j), wantHi = wantLo;
      for (int dj = -1; dj <= 1; dj++)
        for (int di = -1; di <= 1; di++) {
          const int ni = i + di, nj = j + dj;
          if (ni < 0 || nj < 0 || ni >= machined.nx || nj >= machined.ny)
            continue;
          const double w = designed.At(ni, nj);
          wantLo = std::min(wantLo, w);
          wantHi = std::max(wantHi, w);
        }
      // Inside a corner relief's bite the cut below the model is the point
      // (R4): a dogbone that read as scrap would teach the user to ignore
      // the one line that matters.
      bool relieved = false;
      for (const Toolpath::Relief& r : reliefs)
        if (glm::distance(r.at, glm::dvec2(machined.CellX(i),
                                           machined.CellY(j))) <= r.radius) {
          relieved = true;
          break;
        }
      if (!relieved && wantLo - got > v.maxGouge) {
        v.maxGouge = wantLo - got;
        v.gougeAt = {machined.CellX(i), machined.CellY(j)};
      }
      // A tab is material left standing ON PURPOSE, so it is not leftover --
      // but only up to its own top. Excusing it by position alone, the way a
      // relief is excused, would excuse a genuinely uncut slab sitting above
      // it: the whole point of a tab is that it is SHORT.
      bool tabbed = false;
      for (const Toolpath::Tab& t : tabs)
        if (got <= t.topZ + leftTol &&
            glm::distance(t.at, glm::dvec2(machined.CellX(i),
                                           machined.CellY(j))) <= t.radius) {
          tabbed = true;
          break;
        }
      if (!tabbed && got - wantHi > v.maxLeftover) {
        v.maxLeftover = got - wantHi;
        v.leftoverAt = {machined.CellX(i), machined.CellY(j)};
      }
      if (!tabbed && got - wantHi > leftTol)
        over[k] = 1;
    }
  }
  // How much, and in how many separate places. A flood fill over the cells
  // that are over: four corner radii are four regions of a square millimetre
  // each, and a forgotten pocket is one region of hundreds.
  {
    size_t cells = 0;
    std::vector<int> stack;
    std::vector<uint8_t> seen(total, 0);
    for (size_t start = 0; start < total; start++) {
      if (over[start] == 0 || seen[start] != 0)
        continue;
      v.leftoverRegions++;
      stack.push_back(static_cast<int>(start));
      seen[start] = 1;
      Verdict::Region region;
      double sumX = 0.0, sumY = 0.0;
      size_t mine = 0;
      while (!stack.empty()) {
        const int k = stack.back();
        stack.pop_back();
        cells++;
        mine++;
        const int i = k % machined.nx, j = k / machined.nx;
        sumX += machined.CellX(i);
        sumY += machined.CellY(j);
        region.depth = std::max<double>(
            region.depth, static_cast<double>(machined.At(i, j)) -
                              static_cast<double>(designed.At(i, j)));
        const int neigh[4][2] = {
            {i - 1, j}, {i + 1, j}, {i, j - 1}, {i, j + 1}};
        for (const auto& n : neigh) {
          if (n[0] < 0 || n[1] < 0 || n[0] >= machined.nx ||
              n[1] >= machined.ny)
            continue;
          const size_t nk =
              static_cast<size_t>(n[1]) * static_cast<size_t>(machined.nx) +
              static_cast<size_t>(n[0]);
          if (over[nk] == 0 || seen[nk] != 0)
            continue;
          seen[nk] = 1;
          stack.push_back(static_cast<int>(nk));
        }
      }
      region.area = static_cast<double>(mine) * machined.res * machined.res;
      region.at = {sumX / static_cast<double>(mine),
                   sumY / static_cast<double>(mine)};
      v.biggest.push_back(region);
    }
    v.leftoverArea = static_cast<double>(cells) * machined.res * machined.res;
    std::sort(v.biggest.begin(), v.biggest.end(),
              [](const Verdict::Region& a, const Verdict::Region& b) {
                return a.area > b.area;
              });
    if (v.biggest.size() > 16)
      v.biggest.resize(16);
  }
  v.untouchedFraction =
      total > 0 ? static_cast<double>(untouched) / static_cast<double>(total)
                : 0.0;
  return v;
}

// How far a rapid may sit inside the material before it counts. The post
// writes four decimals, so a rapid to the exact top of the stock lands up to
// 50 nm below it purely from rounding — and that IS the normal way to arrive
// at a cut. A micron of graze does not break a cutter; a millimetre does.
constexpr double kGrazeTolerance = 1e-3;

Inspection Inspect(const std::string& gcode, const Setup& setup,
                   const std::vector<Body>& rawBodies, double res,
                   const std::vector<Toolpath::Relief>& reliefs,
                   const std::vector<Toolpath::Tab>& tabs) {
  Inspection out;
  // The G-code is in SETUP space, so the model it is compared against has to
  // be too (C9). Verifying an indexed job against the unrotated part
  // compares a program with a part that is not in front of it.
  const std::vector<Body> bodies = BodiesInSetupFrame(setup, rawBodies);
  glm::dvec3 lo(0.0), hi(0.0);
  if (!StockBounds(setup, rawBodies, lo, hi))
    return out;

  const GProgram program = ProgramInSetupSpace(gcode, setup);
  HeightMap machined = MakeStock(lo, hi, res);

  // Rapids through material: checked BEFORE the cut is applied, because a
  // rapid is only safe over air. This is the check a shape comparison cannot
  // make — the part can come out perfect and the tool still be broken.
  int rapidsThroughStock = 0;
  double worstRapidDepth = 0.0;
  glm::dvec2 worstRapidAt(0.0);
  int worstRapidLine = 0;
  int firstBadLine = 0;
  int holderCollisions = 0;
  double worstHolderDepth = 0.0;
  glm::dvec2 worstHolderAt(0.0);
  int worstHolderLine = 0;
  std::vector<uint8_t> touched(machined.z.size(), 0);
  {
    HeightMap probe = machined;
    glm::dvec3 from(0.0);
    bool have = false;
    for (const GMove& m : program.moves) {
      if (!have) {
        from = m.to;
        have = true;
        continue;
      }
      if (m.rapid) {
        const Tool* tool = setup.FindTool(m.tool);
        const double radius = tool != nullptr ? tool->diameter * 0.5 : 1.0;
        const glm::dvec2 a(from.x, from.y), b(m.to.x, m.to.y);
        const double zLow = std::min(from.z, m.to.z);
        // Only the cells the swept tool can touch, exactly as Simulate does.
        // Scanning the whole grid per rapid was O(moves x cells) and made
        // verification too slow to run on every Generate — which is the
        // difference between a check you remember to run and one you cannot
        // avoid.
        const int i0 = std::max(
            0, static_cast<int>((std::min(a.x, b.x) - radius - probe.x0) /
                                probe.res));
        const int i1 =
            std::min(probe.nx - 1,
                     static_cast<int>((std::max(a.x, b.x) + radius - probe.x0) /
                                      probe.res) +
                         1);
        const int j0 = std::max(
            0, static_cast<int>((std::min(a.y, b.y) - radius - probe.y0) /
                                probe.res));
        const int j1 =
            std::min(probe.ny - 1,
                     static_cast<int>((std::max(a.y, b.y) + radius - probe.y0) /
                                      probe.res) +
                         1);
        bool hit = false;
        double deepest = 0.0;
        glm::dvec2 deepestAt(0.0);
        // No early-out now: the DEPTH is what makes the finding actionable,
        // and the deepest cell may not be the first one found.
        for (int j = j0; j <= j1; j++) {
          for (int i = i0; i <= i1; i++) {
            double t = 0.0;
            const glm::dvec2 c(probe.CellX(i), probe.CellY(j));
            const double d = DistToSegment(c, a, b, t);
            if (d > radius)
              continue;
            // The tool's SHAPE, not a cylinder around it -- the same
            // TipHeight the cut uses. A drill retracting straight up the hole
            // it has just made is the case: its point left a cone, and a
            // cylinder of the drill's diameter reads that cone's wall as
            // material in the way. Every through hole in a job then reported
            // a rapid through stock, which is the kind of false alarm that
            // teaches an operator to ignore the real one.
            const double surface =
                zLow + (tool != nullptr ? TipHeight(*tool, d) : 0.0);
            if (const double into = probe.At(i, j) - surface;
                into > kGrazeTolerance) {
              hit = true;
              if (into > deepest) {
                deepest = into;
                deepestAt = c;
              }
            }
          }
        }
        if (hit) {
          rapidsThroughStock++;
          if (firstBadLine == 0)
            firstBadLine = m.line;
          // Locals, like the count beside them: `out.verdict` is REPLACED by
          // Compare() after this loop, so anything written into it here is
          // thrown away -- which is exactly why the count is re-applied
          // below rather than accumulated in place.
          if (deepest > worstRapidDepth) {
            worstRapidDepth = deepest;
            worstRapidAt = deepestAt;
            worstRapidLine = m.line;
          }
        }
      }
      // The holder, on EVERY move. Unlike the rapid check this is not about
      // how the tool is travelling -- a collet nut buried in a clamp is a
      // crash at 300 mm/min just as surely as at rapid, and the feed only
      // decides how much of the machine goes with it.
      //
      // A lambda, and not the `continue` this shape invites: every move below
      // still has to be CARVED, and an early-out at this level would quietly
      // stop simulating the job it is checking.
      if (const Tool* tool = setup.FindTool(m.tool);
          tool != nullptr && HasHolder(*tool))
        [&] {
          const double rc = tool->collarDiameter * 0.5;
          const glm::dvec2 a(from.x, from.y), b(m.to.x, m.to.y);
          // The collar's underside over the whole move: the tip is lowest at
          // zLow, so that is where the collar hangs lowest too.
          const double zLow = std::min(from.z, m.to.z);
          const double under = zLow + tool->overallLength;
          // Nothing in this job can ever be taller than the uncut block, and a
          // height map only ever comes DOWN -- so a collar riding above the
          // stock's own top cannot hit anything, and the whole scan is skipped
          // on one comparison. That is the common case and it matters: the
          // collar's swept box is five times wider than the cutter's, which
          // took the waveguide's verify from 1.5 s to 4.4 s before this line.
          if (under >= hi.z - kGrazeTolerance)
            return;
          const int i0 = std::max(
              0, static_cast<int>((std::min(a.x, b.x) - rc - probe.x0) /
                                  probe.res));
          const int i1 =
              std::min(probe.nx - 1,
                       static_cast<int>((std::max(a.x, b.x) + rc - probe.x0) /
                                        probe.res) +
                           1);
          const int j0 = std::max(
              0, static_cast<int>((std::min(a.y, b.y) - rc - probe.y0) /
                                  probe.res));
          const int j1 =
              std::min(probe.ny - 1,
                       static_cast<int>((std::max(a.y, b.y) + rc - probe.y0) /
                                        probe.res) +
                           1);
          bool hit = false;
          double deepest = 0.0;
          glm::dvec2 deepestAt(0.0);
          for (int j = j0; j <= j1; j++) {
            for (int i = i0; i <= i1; i++) {
              double t = 0.0;
              const glm::dvec2 c(probe.CellX(i), probe.CellY(j));
              const double d = DistToSegment(c, a, b, t);
              const double top = probe.At(i, j);
              // The same graze tolerance the rapid check uses, for the same
              // reason: a cell standing a nanometre proud of the collar is
              // quantisation, not a crash.
              if (!FoulsHolder(*tool, d, top - zLow - kGrazeTolerance))
                continue;
              const double into = top - under;
              hit = true;
              if (into > deepest) {
                deepest = into;
                deepestAt = c;
              }
            }
          }
          if (hit) {
            holderCollisions++;
            if (deepest > worstHolderDepth) {
              worstHolderDepth = deepest;
              worstHolderAt = deepestAt;
              worstHolderLine = m.line;
            }
          }
        }();
      // Apply every move (cuts remove material; rapids over air do nothing).
      if (!m.rapid) {
        CarveSweep(from, m.to, setup.FindTool(m.tool), probe);
        MarkSwept(from, m, setup, probe, touched);
      }
      from = m.to;
    }
    machined = probe;
  }

  out.designed = SampleModel(bodies, machined, lo.z);
  out.verdict = Compare(machined, out.designed, lo.z, &touched,
                        /*leftoverTolerance=*/0.0, reliefs, tabs);
  out.verdict.reliefs = static_cast<int>(reliefs.size());
  out.verdict.tabs = static_cast<int>(tabs.size());
  out.verdict.rapidsThroughStock = rapidsThroughStock;
  out.verdict.firstBadLine = firstBadLine;
  out.verdict.worstRapidDepth = worstRapidDepth;
  out.verdict.worstRapidAt = worstRapidAt;
  out.verdict.worstRapidLine = worstRapidLine;
  out.verdict.fileErrors = static_cast<int>(program.errors.size());
  if (!program.errors.empty())
    out.verdict.firstFileError = program.errors.front();
  // A file whose part TURNS was not checked by anything above. The stock
  // here is a height map, z(x, y), and a height map cannot represent a part
  // that rotates -- every cut was applied as though it stood still, so the
  // gouge and the leftover below are measurements of a program that does not
  // exist. `InspectRotary` is the model that can (docs/cam/rotary-plan.md)
  // and `VerifyGcode` routes to it; this path is reached by a caller that
  // wants the height map specifically, and it gets told.
  if (RotatesWhileCutting(program)) {
    out.verdict.fileErrors++;
    if (out.verdict.firstFileError.empty())
      out.verdict.firstFileError =
          "the A axis MOVES in this file and this is the 3-axis check: its "
          "stock is a height map, which cannot represent a part that turns";
  }
  out.verdict.fileWarnings = static_cast<int>(program.warnings.size());
  if (!program.warnings.empty())
    out.verdict.firstFileWarning = program.warnings.front();
  out.verdict.holderCollisions = holderCollisions;
  out.verdict.worstHolderDepth = worstHolderDepth;
  out.verdict.worstHolderAt = worstHolderAt;
  out.verdict.worstHolderLine = worstHolderLine;

  // Can the machine actually GO where the file asks? Checked on the posted
  // file, not on the toolpath that produced it -- the generator's own idea of
  // reach was wrong once already, and a check that reads the artifact cannot
  // inherit the mistake. Arcs are chords by now, so this measures what a
  // circle SWEEPS. Skipped when the machine states no work area, which is
  // most of the corpus.
  // ...Framed, because `bodies` here already IS framed (above). MachineBounds
  // frames what it is given, so passing these applied the setup rotation
  // TWICE -- which is invisible on an identity frame and put every move of a
  // baffle clamped face-up 391 mm outside its own machine.
  glm::dvec3 bedLo(0.0), bedHi(0.0);
  if (MachineBoundsFramed(setup, bodies, bedLo, bedHi)) {
    const char axes[2] = {'X', 'Y'};
    double zLo = 0.0, zHi = 0.0;
    bool haveZ = false;
    for (const GMove& mv : program.moves) {
      for (int a = 0; a < 2; a++) {
        const double over = std::max(bedLo[a] - mv.to[a], mv.to[a] - bedHi[a]);
        if (over <= 1e-6)
          continue;
        out.verdict.outsideTravel++;
        if (over > out.verdict.worstOvershoot) {
          out.verdict.worstOvershoot = over;
          out.verdict.outsideAxis = axes[a];
        }
        if (out.verdict.firstOutsideLine == 0)
          out.verdict.firstOutsideLine = mv.line;
      }
      zLo = haveZ ? std::min(zLo, mv.to.z) : mv.to.z;
      zHi = haveZ ? std::max(zHi, mv.to.z) : mv.to.z;
      haveZ = true;
    }
    // Z is a SPAN, not a position, and that distinction is the whole of this
    // paragraph. In X and Y the bed's near corner is assumed to sit on the
    // stock's near corner plus `reach_offset` -- a stated assumption a person
    // can correct. There is no equivalent in Z: nothing says where the
    // machine's Z zero sits relative to the workpiece, because that is what
    // the operator sets when they touch off, AFTER the file is written.
    //
    // Anchoring it anyway is what this check did when it grew its third axis,
    // and it read `lo.z = the stock's underside` -- so the waveguide's five
    // profile moves at Z-0.5, which is a through cut into the spoilboard and
    // the most ordinary thing a router does, came back as "1.0 mm past its Z
    // travel". A check that fires on every through cut is a check that gets
    // scrolled past, which is the failure this whole file exists to avoid.
    //
    // What IS a fact about the machine, needing nothing typed: a program
    // whose deepest point and highest retract are further apart than the
    // Z travel cannot run at any touch-off height at all.
    const double zTravel = bedHi.z - bedLo.z;
    if (haveZ && zHi - zLo > zTravel + 1e-6) {
      out.verdict.outsideTravel++;
      const double over = (zHi - zLo) - zTravel;
      if (over > out.verdict.worstOvershoot) {
        out.verdict.worstOvershoot = over;
        out.verdict.outsideAxis = 'Z';
      }
    }
  }
  out.machined = std::move(machined);
  out.touched = std::move(touched);
  out.ok = true;
  return out;
}

std::vector<glm::dvec3> LeftoverPoints(const Inspection& insp, double tolerance,
                                       size_t maxPoints) {
  std::vector<glm::dvec3> pts;
  if (!insp.ok || insp.machined.nx != insp.designed.nx ||
      insp.machined.ny != insp.designed.ny)
    return pts;
  // Two passes: count first, then take every nth. Sampling on the fly would
  // paint the near corner of the part densely and the far one not at all.
  const bool haveMask = insp.touched.size() == insp.machined.z.size();
  const auto isLeftover = [&](int i, int j) {
    if (haveMask && insp.touched[static_cast<size_t>(j) *
                                     static_cast<size_t>(insp.machined.nx) +
                                 static_cast<size_t>(i)] == 0)
      return false; // never cut here, so nothing was "left"
    return insp.machined.At(i, j) - insp.designed.At(i, j) > tolerance;
  };
  size_t total = 0;
  for (int j = 0; j < insp.machined.ny; j++)
    for (int i = 0; i < insp.machined.nx; i++)
      if (isLeftover(i, j))
        total++;
  if (total == 0)
    return pts;
  // Round the stride UP, or the cap is not a cap: total/max truncates and
  // hands back one or two more points than asked for.
  const size_t stride =
      maxPoints > 0 ? std::max<size_t>(1, (total + maxPoints - 1) / maxPoints)
                    : 1;
  size_t seen = 0;
  for (int j = 0; j < insp.machined.ny; j++)
    for (int i = 0; i < insp.machined.nx; i++) {
      if (!isLeftover(i, j))
        continue;
      if (seen++ % stride != 0)
        continue;
      pts.emplace_back(insp.machined.CellX(i), insp.machined.CellY(j),
                       insp.machined.At(i, j));
    }
  return pts;
}

Verdict VerifyGcode(const std::string& gcode, const Setup& setup,
                    const std::vector<Body>& bodies, double res,
                    const std::vector<Toolpath::Relief>& reliefs,
                    const std::vector<Toolpath::Tab>& tabs) {
  // The STOCK MODEL follows the file, not the setup. A program whose A moves
  // is cut from a cylinder that turns and one whose A does not is cut from a
  // block that does not, and asking the setup would get the answer wrong in
  // the case that matters: a rotary-capable machine posting a 3-axis job.
  // Reliefs and tabs are 2.5D statements about a contour and mean nothing to
  // a rotary pass, so they are not carried into one rather than being
  // silently ignored inside it.
  if (RotatesWhileCutting(ParseGcode(gcode)))
    return InspectRotary(gcode, setup, bodies, res).verdict;
  return Inspect(gcode, setup, bodies, res, reliefs, tabs).verdict;
}

std::vector<VerdictLine> Explain(const Verdict& v, double tolerance,
                                 double minutes, bool breaksAnEdge) {
  std::vector<VerdictLine> out;
  char buf[256];
  // (x, y) in three axes and (x, theta) in four. `Verdict::rotary` says
  // which, because a pair of doubles cannot, and printing an angle under a
  // "Y" is the kind of wrong that reads as right.
  const char* second = v.rotary ? "A" : "Y";

  if (v.cells == 0) {
    out.push_back(
        {VerdictLine::Level::Warn,
         v.untouchedFraction >= 0.999
             ? "This program never touches the stock — it cuts nothing."
             : "Nothing was checked — this job has no stock to cut."});
    return out;
  }
  if (v.reliefs > 0) {
    std::snprintf(buf, sizeof(buf),
                  "%d inside corner%s relieved for a square fit.", v.reliefs,
                  v.reliefs == 1 ? "" : "s");
    out.push_back({VerdictLine::Level::Good, buf});
  }
  if (v.tabs > 0) {
    // Said out loud rather than silently subtracted: four tabs is a job to
    // sign off, and none where four were asked for is a job to look at.
    std::snprintf(buf, sizeof(buf),
                  "%d tab%s left holding the part%s in the sheet — part them "
                  "off after the job.",
                  v.tabs, v.tabs == 1 ? "" : "s", v.tabs == 1 ? "" : "s");
    out.push_back({VerdictLine::Level::Good, buf});
  }
  // Gouging first: it is the one that scraps the part, and the only one a
  // backplot genuinely cannot show you.
  if (v.maxGouge > tolerance) {
    std::snprintf(buf, sizeof(buf),
                  "Cuts %.2f mm INTO the part at X%.1f %s%.1f — that is scrap. "
                  "Check the depths and the tool diameter.",
                  v.maxGouge, v.gougeAt.x, second, v.gougeAt.y);
    out.push_back({VerdictLine::Level::Bad, buf});
    // An edge break removes material the model still has, unless the round
    // or the chamfer is modelled too — so this job gouges BY CONSTRUCTION
    // and the verdict would otherwise read as scrap every time. The fix is
    // the honest one: model the break (a Fillet or Chamfer feature on that
    // edge) and the verifier and the toolpath describe the same part.
    if (breaksAnEdge)
      out.push_back({VerdictLine::Level::Warn,
                     "This job breaks an edge. A round or a chamfer that is "
                     "not in the MODEL reads as a gouge here — model it (a "
                     "Fillet or Chamfer on that edge) and this line goes."});
  }
  if (v.rapidsThroughStock > 0) {
    // HOW DEEP, not just how many. A count on its own cannot be acted on:
    // a ball lifting out of a groove of its own shape grazes a few hundredths
    // and a rapid across a rib buries the cutter, and both used to read as
    // the same sentence. Below a tenth of a millimetre it is a rub, and the
    // line says so rather than sounding like a crash.
    // A depth of zero means NOT MEASURED, not "barely touched": a Verdict
    // assembled by hand (or by an older run) has no depth, and calling that
    // a graze would be reassurance nobody earned. Only a positive depth
    // below the threshold is a rub.
    const bool graze = v.worstRapidDepth > 0.0 && v.worstRapidDepth < 0.1;
    const int atLine =
        v.worstRapidLine != 0 ? v.worstRapidLine : v.firstBadLine;
    std::snprintf(buf, sizeof(buf),
                  "%d rapid%s through material, worst %.2f mm deep at "
                  "X%.1f Y%.1f (line %d)%s",
                  v.rapidsThroughStock, v.rapidsThroughStock == 1 ? "" : "s",
                  v.worstRapidDepth, v.worstRapidAt.x, v.worstRapidAt.y, atLine,
                  graze ? " — a rub, not a crash: the cutter brushes stock it "
                          "has already shaped."
                        : ". If it is a TRAVERSE, raise Safe Z. If it is a "
                          "RETRACT, Safe Z will not help: the cutter is "
                          "climbing out of its own cut and its rim is in the "
                          "strip beside it that has not been cut yet, which is "
                          "worst where the surface is steep.");
    out.push_back(
        {graze ? VerdictLine::Level::Warn : VerdictLine::Level::Bad, buf});
  }
  if (v.fileErrors > 0) {
    // FIRST, above the gouge. Everything below this line was measured by
    // simulating a file the parser could not read straight, so it is an
    // answer about a different program.
    std::snprintf(buf, sizeof(buf),
                  "%d line%s of this file could not be read: %s. Everything "
                  "else here was measured by simulating what WAS readable, so "
                  "read it as a report on a different program.",
                  v.fileErrors, v.fileErrors == 1 ? "" : "s",
                  v.firstFileError.c_str());
    out.push_back({VerdictLine::Level::Bad, buf});
  }
  if (v.fileWarnings > 0) {
    std::snprintf(buf, sizeof(buf),
                  "%d thing%s in this file a CONTROLLER may refuse though the "
                  "parser here survived it: %s",
                  v.fileWarnings, v.fileWarnings == 1 ? "" : "s",
                  v.firstFileWarning.c_str());
    out.push_back({VerdictLine::Level::Warn, buf});
  }
  if (v.holderCollisions > 0) {
    // No graze band here. A rapid brushing a scallop it cut itself is a rub;
    // the collet nut touching anything at all is not, because nothing above
    // the flutes is supposed to reach the work under any circumstances.
    std::snprintf(buf, sizeof(buf),
                  "%d move%s drive the HOLDER into material, worst %.2f mm "
                  "past the collar at X%.1f Y%.1f (line %d). The cutter never "
                  "goes near it, so the part still measures right — stick the "
                  "tool out further, use a shorter-nosed holder, or clear that "
                  "material with a longer tool first.",
                  v.holderCollisions, v.holderCollisions == 1 ? "" : "s",
                  v.worstHolderDepth, v.worstHolderAt.x, v.worstHolderAt.y,
                  v.worstHolderLine);
    out.push_back({VerdictLine::Level::Bad, buf});
  }
  if (v.outsideTravel > 0) {
    std::snprintf(buf, sizeof(buf),
                  "%d move%s ask for somewhere the machine cannot go — %.1f mm "
                  "past its %c travel, first at line %d. The controller alarms "
                  "or hits a stop; say where the bed sits on the sheet "
                  "(reach_offset), or move the part.",
                  v.outsideTravel, v.outsideTravel == 1 ? "" : "s",
                  v.worstOvershoot, v.outsideAxis, v.firstOutsideLine);
    out.push_back({VerdictLine::Level::Bad, buf});
  }
  if (v.maxLeftover > tolerance) {
    // The AREA is what makes this readable: four inside corners a 3 mm cutter
    // cannot reach are four regions of about a square millimetre, and a
    // forgotten operation is one region of hundreds. Both used to print the
    // same sentence.
    // The AREA is what makes this readable, and the PLACES are a separate
    // claim: the rotary compare measures area and does not group it into
    // regions, and printing "in 0 places" beside a millimetre of leftover
    // reads as a contradiction rather than as a gap. Say what was counted.
    char where[128] = "";
    if (v.leftoverRegions > 0)
      std::snprintf(where, sizeof(where), " in %d place%s", v.leftoverRegions,
                    v.leftoverRegions == 1 ? "" : "s");
    std::snprintf(buf, sizeof(buf),
                  "Leaves %.2f mm of material at X%.1f %s%.1f — %.1f mm2%s. %s",
                  v.maxLeftover, v.leftoverAt.x, second, v.leftoverAt.y,
                  v.leftoverArea, where,
                  v.leftoverRegions >= 3 && v.leftoverArea < 25.0
                      ? "Small and scattered, which is what inside corners "
                        "the cutter cannot reach look like — a smaller tool "
                        "gets in there."
                      : "Check that every operation this part needs is in "
                        "the job.");
    out.push_back({VerdictLine::Level::Warn, buf});
  }
  if (out.empty())
    out.push_back({VerdictLine::Level::Good,
                   "Machines the part cleanly. No gouges, nothing "
                   "left over."});
  if (minutes > 0.0) {
    std::snprintf(buf, sizeof(buf), "~%.0f min of cutting at these feeds.",
                  std::max(1.0, minutes));
    out.push_back({VerdictLine::Level::Good, buf});
  }
  return out;
}

} // namespace cam

namespace cam {

std::vector<VerdictLine> MachineLines(const Setup& setup,
                                      const std::vector<Body>& bodies,
                                      const std::vector<Toolpath>* paths) {
  std::vector<VerdictLine> out;
  if (std::string reach = MachineWarning(setup, bodies); !reach.empty()) {
    double clipped = 0.0;
    if (paths != nullptr)
      for (const Toolpath& tp : *paths)
        clipped += tp.clippedMm;
    if (clipped > 0.0) {
      char buf[96];
      std::snprintf(buf, sizeof(buf),
                    ". %.0f mm of cutting beyond it is left out of the file",
                    clipped);
      reach += buf;
    }
    out.push_back({VerdictLine::Level::Warn, reach});
  } else if (const std::string over = MachineOverhang(setup, bodies);
             !over.empty())
    out.push_back({VerdictLine::Level::Warn, over});
  return out;
}

} // namespace cam
