#include "core/cam/CamVerify.h"

#include "core/cam/CamMount.h"
#include "core/cam/CamOtherSetups.h"

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

double CarveSweep(const glm::dvec3& from, const glm::dvec3& to,
                  const Tool* tool, HeightMap& stock) {
  double removed = 0.0;
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
      // ...and while it MOVES, the lowest the tool reaches over the stretch
      // of the move during which this cell lies under it -- not its height
      // at the closest approach. A descending move goes on down after it
      // has passed closest, with the cell still under its rim: taking Z at
      // the closest point left the cell up to the whole descent too high,
      // which read as leftover on every ramp and as a "rapid through
      // material" when the tool then lifted out of its own cut (0.12 mm on
      // the sampler part's 3D rough). The stretch is where |p(t) - c| <= r,
      // an interval of t; a flat end is lowest at one of its ends, and a
      // shaped tip is sampled across it.
      double surface = 0.0;
      if (xyStill) {
        surface = std::min(from.z, to.z) +
                  (tool != nullptr ? TipHeight(*tool, d) : 0.0);
      } else {
        const glm::dvec2 e = b - a;
        const glm::dvec2 d0 = a - c;
        const double qa = glm::dot(e, e);
        const double qb = 2.0 * glm::dot(d0, e);
        const double qc = glm::dot(d0, d0) - radius * radius;
        const double disc = std::max(0.0, qb * qb - 4.0 * qa * qc);
        const double t1 =
            std::clamp((-qb - std::sqrt(disc)) / (2.0 * qa), 0.0, 1.0);
        const double t2 =
            std::clamp((-qb + std::sqrt(disc)) / (2.0 * qa), 0.0, 1.0);
        const auto at = [&](double u) {
          const glm::dvec2 p = a + e * u;
          return from.z + (to.z - from.z) * u +
                 (tool != nullptr ? TipHeight(*tool, glm::length(p - c)) : 0.0);
        };
        surface = std::min({at(t), at(t1), at(t2)});
        if (tool != nullptr && tool->type != ToolType::Flat)
          for (int k = 1; k < 8; k++)
            surface = std::min(surface, at(t1 + (t2 - t1) * k / 8.0));
      }
      if (const double top = stock.At(i, j); top > surface) {
        removed += top - surface;
        stock.Set(i, j, static_cast<float>(surface));
      }
    }
  }
  return removed * stock.res * stock.res;
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
                const std::vector<Toolpath::Tab>& tabs = {},
                const OtherSetups* others = nullptr) {
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
      // Excess another setup of the job removes is not left on the part: said
      // as that setup's cut, and out of every leftover measure
      // (docs/cam/simulation.md#leftover-another-setup-removes).
      if (!tabbed && others != nullptr && got - wantHi > leftTol) {
        const glm::dvec3 from(machined.CellX(i), machined.CellY(j),
                              wantHi + leftTol);
        if (const uint64_t by =
                others->Claims(from, {from.x, from.y, got}, leftTol)) {
          v.AddOtherCut(by, others->NameOf(by), machined.res * machined.res);
          continue;
        }
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

namespace {
// The FIXTURES as a height field of their own
// (docs/cam/simulation.md#fixture-collisions):
// each cell the top of the tallest fixture over it, and which fixture that
// was. On its own grid, over the fixtures' footprint -- a vise beside the
// stock has no cells in the stock's map -- and never carved: the path going
// through a clamp does not remove the clamp.
constexpr float kNoFixture = -1.0e9f;
struct FixtureField {
  HeightMap top;
  std::vector<int> owner; // index into the fixture list, -1 = none
  double tallest = -1.0e9;
};

FixtureField BuildFixtureField(const std::vector<FixtureSolid>& fixtures,
                               double res) {
  FixtureField field;
  glm::dvec3 lo(1e30), hi(-1e30);
  for (const FixtureSolid& f : fixtures)
    for (const Body& b : f.bodies) {
      if (!b.shape.IsValid())
        continue;
      glm::vec3 blo, bhi;
      b.shape.GetBoundingBox(blo, bhi);
      lo = glm::min(lo, glm::dvec3(blo));
      hi = glm::max(hi, glm::dvec3(bhi));
    }
  if (lo.x > hi.x)
    return field; // nothing with geometry
  field.top = MakeStock(lo, hi, res);
  field.top.z.assign(field.top.z.size(), kNoFixture);
  field.owner.assign(field.top.z.size(), -1);
  for (size_t k = 0; k < fixtures.size(); k++) {
    const HeightMap one = SampleModel(fixtures[k].bodies, field.top,
                                      static_cast<double>(kNoFixture));
    for (size_t c = 0; c < one.z.size(); c++)
      if (one.z[c] > field.top.z[c]) {
        field.top.z[c] = one.z[c];
        field.owner[c] = static_cast<int>(k);
        field.tallest = std::max(field.tallest, static_cast<double>(one.z[c]));
      }
  }
  return field;
}
} // namespace

Inspection Inspect(const std::string& gcode, const Setup& setup,
                   const std::vector<Body>& rawBodies, double res,
                   const std::vector<Toolpath::Relief>& reliefs,
                   const std::vector<Toolpath::Tab>& tabs,
                   const std::vector<FixtureSolid>& fixtures,
                   const OtherSetups* others) {
  Inspection out;
  // The G-code is in SETUP space, so the model it is compared against has to
  // be too (C9). Verifying an indexed job against the unrotated part
  // compares a program with a part that is not in front of it.
  const std::vector<Body> bodies = BodiesInSetupFrame(setup, rawBodies);
  glm::dvec3 lo(0.0), hi(0.0);
  if (!StockBounds(setup, rawBodies, lo, hi))
    return out;

  const GProgram program = ProgramInSetupSpace(gcode, setup);
  // Where the earlier setups of the job have already cut, this one starts
  // from what they left -- the same start the playback plays from
  // (docs/cam/simulation.md#leftover-another-setup-removes).
  HeightMap machined;
  if (!StartingStock(setup, rawBodies, res, others, machined))
    return out;

  // Rapids through material: checked BEFORE the cut is applied, because a
  // rapid is only safe over air. This is the check a shape comparison cannot
  // make — the part can come out perfect and the tool still be broken.
  int rapidsThroughStock = 0;
  double worstRapidDepth = 0.0;
  glm::dvec2 worstRapidAt(0.0);
  int worstRapidLine = 0;
  int firstBadLine = 0;
  int shankCollisions = 0;
  double worstShankDepth = 0.0;
  glm::dvec2 worstShankAt(0.0);
  int worstShankLine = 0;
  int holderCollisions = 0;
  double worstHolderDepth = 0.0;
  glm::dvec2 worstHolderAt(0.0);
  int worstHolderLine = 0;
  // Fixtures, on a setup whose bed they sit on. An indexed (A) setup turns
  // the part under the spindle, and a bed-space clamp has no meaning in that
  // frame: said in the verdict, not checked against the wrong picture.
  const bool fixturesSkipped = !fixtures.empty() && EmitsRotary(setup);
  const FixtureField field =
      fixturesSkipped ? FixtureField{} : BuildFixtureField(fixtures, res);
  int fixtureCollisions = 0;
  double worstFixtureDepth = 0.0;
  glm::dvec2 worstFixtureAt(0.0);
  double worstFixtureZ = 0.0;
  int worstFixtureLine = 0;
  int worstFixtureOwner = -1;
  bool worstFixtureByHolder = false;
  std::vector<uint8_t> touched(machined.z.size(), 0);
  // The bite (Verdict::overBites). Measured over a WINDOW of cutting, not per
  // move: an arc posted as eight 0.6 mm chords removes a crescent per chord
  // that the grid quantises to within a cell, and volume over length on one
  // of them is mostly that quantisation. A couple of millimetres of travel
  // averages it out and is still far shorter than any cut worth reporting.
  int overBites = 0;
  double worstBite = 0.0, worstBiteAllowed = 0.0;
  glm::dvec2 worstBiteAt(0.0);
  int worstBiteLine = 0;
  uint64_t worstBiteOp = 0;
  std::map<uint64_t, std::pair<double, double>> biteByOp;
  // What an op's passes take, or 0 for an op this does not check: drilling
  // and edge breaks plunge or skim by design, and the surfacing and rotary
  // passes have no stepdown to measure against.
  const auto allowedBite = [&](uint64_t opId) {
    const Operation* op = nullptr;
    for (const Operation& o : setup.operations)
      if (o.id == opId)
        op = &o;
    if (op == nullptr)
      return 0.0;
    switch (op->type) {
    case OpType::Profile:
    case OpType::Pocket:
    case OpType::Adaptive:
    case OpType::Face:
      break;
    default:
      return 0.0;
    }
    const double pass =
        op->stepdown > 0.0 ? op->stepdown : op->topZ - op->bottomZ;
    const double tab = op->tabCount > 0 ? op->tabHeight : 0.0;
    return std::max(pass, tab);
  };
  double winVol = 0.0, winLen = 0.0;
  uint64_t winOp = 0;
  int winTool = 0;
  double winAllowed = 0.0;
  const auto judgeBite = [&](double bite, const glm::dvec2& at, int line,
                             uint64_t opId, double allowed) {
    auto& [worst, limit] = biteByOp[opId];
    worst = std::max(worst, bite);
    limit = allowed;
    // A quarter over, plus the grid: the window's two ends are each good to
    // a cell, and a stepdown is a request the ramp meets only on average.
    if (bite <= allowed * 1.25 + 0.2)
      return;
    overBites++;
    if (bite > worstBite) {
      worstBite = bite;
      worstBiteAllowed = allowed;
      worstBiteAt = at;
      worstBiteLine = line;
      worstBiteOp = opId;
    }
  };
  glm::dvec2 winAt(0.0);
  int winLine = 0;
  double winDiameter = 0.0;
  const auto flushBite = [&] {
    if (winAllowed > 0.0 && winLen > 0.5 && winDiameter > 0.0)
      judgeBite(winVol / (winDiameter * winLen), winAt, winLine, winOp,
                winAllowed);
    winVol = winLen = 0.0;
  };
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
      // A shank WIDER than the cutter, on every move: material within its
      // radius but outside the cutter's, standing above the top of the
      // flutes, is a wall the shank is being driven into
      // (docs/cam/simulation.md#shank-collisions). Inside the cutter's own
      // radius the tool is the tool, as for the holder: a cut deeper than
      // the flutes is stated at the op (cam::ReachCheck), in one vocabulary.
      if (const Tool* tool = setup.FindTool(m.tool);
          tool != nullptr && tool->shankDiameter > tool->diameter + 1e-6)
        [&] {
          const double rc = tool->diameter * 0.5;
          const double rs = tool->shankDiameter * 0.5;
          const double zLow = std::min(from.z, m.to.z);
          // Above the uncut block, nothing can stand over the flutes.
          if (zLow + tool->fluteLength >= hi.z - kGrazeTolerance)
            return;
          const glm::dvec2 a(from.x, from.y), b(m.to.x, m.to.y);
          const int i0 = std::max(
              0, static_cast<int>((std::min(a.x, b.x) - rs - probe.x0) /
                                  probe.res));
          const int i1 =
              std::min(probe.nx - 1,
                       static_cast<int>((std::max(a.x, b.x) + rs - probe.x0) /
                                        probe.res) +
                           1);
          const int j0 = std::max(
              0, static_cast<int>((std::min(a.y, b.y) - rs - probe.y0) /
                                  probe.res));
          const int j1 =
              std::min(probe.ny - 1,
                       static_cast<int>((std::max(a.y, b.y) + rs - probe.y0) /
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
              // A cell's worth off the cutter's edge: the wall the cutter
              // itself leaves stands exactly at its radius, and quantised to
              // the grid half of it lands just outside.
              if (d <= rc + probe.res || d > rs)
                continue;
              const double into = probe.At(i, j) - (zLow + tool->fluteLength);
              if (into <= kGrazeTolerance)
                continue;
              hit = true;
              if (into > deepest) {
                deepest = into;
                deepestAt = c;
              }
            }
          }
          if (hit) {
            shankCollisions++;
            if (deepest > worstShankDepth) {
              worstShankDepth = deepest;
              worstShankAt = deepestAt;
              worstShankLine = m.line;
            }
          }
        }();
      // Fixtures, on EVERY move, like the holder: a cutter fed into a clamp
      // breaks just as a rapid through one does. The cutter is its TipHeight
      // shape; beyond its radius, out to the collar's, it is the collar's
      // underside when the tool states one. The tool's height is taken where
      // the move passes closest to the cell, not at the move's lowest Z, so a
      // ramp past a clamp is not charged a depth it never reached beside it.
      if (!field.top.z.empty())
        [&] {
          const Tool* tool = setup.FindTool(m.tool);
          const double rc = tool != nullptr ? tool->diameter * 0.5 : 1.0;
          const bool holder = tool != nullptr && HasHolder(*tool);
          const double reach = holder ? tool->collarDiameter * 0.5 : rc;
          if (std::min(from.z, m.to.z) >= field.tallest - kGrazeTolerance)
            return; // above every fixture: the common case, one compare
          const HeightMap& f = field.top;
          const glm::dvec2 a(from.x, from.y), b(m.to.x, m.to.y);
          const bool vertical = glm::length(b - a) < 1e-9;
          const int i0 = std::max(
              0, static_cast<int>((std::min(a.x, b.x) - reach - f.x0) / f.res));
          const int i1 = std::min(
              f.nx - 1,
              static_cast<int>((std::max(a.x, b.x) + reach - f.x0) / f.res) +
                  1);
          const int j0 = std::max(
              0, static_cast<int>((std::min(a.y, b.y) - reach - f.y0) / f.res));
          const int j1 = std::min(
              f.ny - 1,
              static_cast<int>((std::max(a.y, b.y) + reach - f.y0) / f.res) +
                  1);
          bool hit = false;
          double deepest = 0.0;
          glm::dvec2 deepestAt(0.0);
          double deepestTop = 0.0;
          int deepestOwner = -1;
          bool deepestByHolder = false;
          for (int j = j0; j <= j1; j++) {
            for (int i = i0; i <= i1; i++) {
              const float top = f.At(i, j);
              if (top <= kNoFixture)
                continue;
              double t = 0.0;
              const glm::dvec2 c(f.CellX(i), f.CellY(j));
              const double d = DistToSegment(c, a, b, t);
              if (d > reach)
                continue;
              const double z = vertical ? std::min(from.z, m.to.z)
                                        : from.z + (m.to.z - from.z) * t;
              const bool byHolder = d > rc;
              const double under =
                  byHolder ? z + tool->overallLength
                           : z + (tool != nullptr ? TipHeight(*tool, d) : 0.0);
              const double into = top - under;
              if (into <= kGrazeTolerance)
                continue;
              hit = true;
              if (into > deepest) {
                deepest = into;
                deepestAt = c;
                deepestTop = top;
                deepestOwner = field.owner[static_cast<size_t>(j) *
                                               static_cast<size_t>(f.nx) +
                                           static_cast<size_t>(i)];
                deepestByHolder = byHolder;
              }
            }
          }
          if (hit) {
            fixtureCollisions++;
            if (deepest > worstFixtureDepth) {
              worstFixtureDepth = deepest;
              worstFixtureAt = deepestAt;
              worstFixtureZ = deepestTop;
              worstFixtureLine = m.line;
              worstFixtureOwner = deepestOwner;
              worstFixtureByHolder = deepestByHolder;
            }
          }
        }();
      // Apply every move (cuts remove material; rapids over air do nothing).
      if (!m.rapid) {
        const Tool* tool = setup.FindTool(m.tool);
        const double allowed = allowedBite(m.op);
        const double dia = tool != nullptr ? tool->diameter : 2.0;
        const double len = glm::length(glm::dvec2(m.to - from));
        // Steep enough to be a plunge. Not `len == 0`: the arc before a tab
        // exit is re-expanded to chords here, and its last chord can end a
        // few microns off the file's own point -- so the plunge off the tab
        // read as a lateral move of no length carrying a whole disc of
        // volume. The ramp is 3 degrees; this is anything past about 63.
        const bool plunge = len < 1e-3 || from.z - m.to.z > 2.0 * len;
        // A plunge's bite is the mean depth under the cutter's face, read
        // BEFORE it cuts -- and without the face's outermost cell. An arc is
        // simulated as chords, whose sagitta leaves a ring of cells at the
        // kerf's edge standing at full stock height; a lateral stretch
        // brushes that ring along its length, but a plunge averages over the
        // disc, and a tab exit that took 1.5 mm read as 3.9.
        double plungeBite = 0.0;
        if (allowed > 0.0 && plunge) {
          const double inner = dia * 0.5 - 1.5 * probe.res;
          const double zLow = std::min(from.z, m.to.z);
          double sum = 0.0;
          int cells = 0;
          const int i0 = std::max(
              0, static_cast<int>((m.to.x - inner - probe.x0) / probe.res));
          const int i1 = std::min(
              probe.nx - 1,
              static_cast<int>((m.to.x + inner - probe.x0) / probe.res) + 1);
          const int j0 = std::max(
              0, static_cast<int>((m.to.y - inner - probe.y0) / probe.res));
          const int j1 = std::min(
              probe.ny - 1,
              static_cast<int>((m.to.y + inner - probe.y0) / probe.res) + 1);
          for (int j = j0; j <= j1; j++)
            for (int i = i0; i <= i1; i++) {
              const double d = glm::length(
                  glm::dvec2(probe.CellX(i) - m.to.x, probe.CellY(j) - m.to.y));
              if (inner <= 0.0 || d > inner)
                continue;
              const double surface =
                  zLow + (tool != nullptr ? TipHeight(*tool, d) : 0.0);
              sum += std::max(0.0, probe.At(i, j) - surface);
              cells++;
            }
          plungeBite = cells > 0 ? sum / cells : 0.0;
        }
        // Judged in pieces no longer than the window: one 50 mm move that
        // starts and ends in air would otherwise average its bite over the
        // air as well. Carving the pieces removes exactly what carving the
        // whole move would.
        const double window = std::max(2.0, dia * 0.5);
        const int pieces = allowed > 0.0 && !plunge && len > window
                               ? static_cast<int>(std::ceil(len / window))
                               : 1;
        double removed = 0.0;
        for (int k = 1; k <= pieces; k++) {
          const glm::dvec3 a = from + (m.to - from) * ((k - 1.0) / pieces);
          const glm::dvec3 b = from + (m.to - from) * (k / double(pieces));
          const double took = CarveSweep(a, b, tool, probe);
          if (pieces > 1) {
            flushBite();
            winOp = m.op;
            winTool = m.tool;
            winAllowed = allowed;
            winDiameter = dia;
            winVol = took;
            winLen = len / pieces;
            winAt = glm::dvec2(b);
            winLine = m.line;
            flushBite();
          }
          removed += took;
        }
        MarkSwept(from, m, setup, probe, touched);
        if (m.op != winOp || m.tool != winTool)
          flushBite();
        winOp = m.op;
        winTool = m.tool;
        winAllowed = allowed;
        winDiameter = dia;
        if (allowed > 0.0 && plunge) {
          // A plunge, measured above; the window it interrupts carries on
          // after it.
          judgeBite(plungeBite, glm::dvec2(m.to), m.line, m.op, allowed);
        } else if (allowed > 0.0 && pieces == 1) {
          winVol += removed;
          winLen += len;
          winAt = glm::dvec2(m.to);
          winLine = m.line;
          if (winLen >= window)
            flushBite();
        }
      } else {
        flushBite();
      }
      from = m.to;
    }
    flushBite();
    machined = probe;
  }

  out.designed = SampleModel(bodies, machined, lo.z);
  out.verdict = Compare(machined, out.designed, lo.z, &touched,
                        /*leftoverTolerance=*/0.0, reliefs, tabs, others);
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
  // exist. `InspectRotary` is the model that can
  // (docs/cam/rotary.md#verifying-a-turning-part) and `VerifyGcode` routes to
  // it; this path is reached by a caller that wants the height map
  // specifically, and it gets told.
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
  out.verdict.overBites = overBites;
  out.verdict.worstBite = worstBite;
  out.verdict.worstBiteAllowed = worstBiteAllowed;
  out.verdict.worstBiteAt = worstBiteAt;
  out.verdict.worstBiteLine = worstBiteLine;
  out.verdict.worstBiteOp = worstBiteOp;
  out.verdict.biteByOp = std::move(biteByOp);
  CheckHolding(program, setup, rawBodies, out.verdict);
  out.verdict.shankCollisions = shankCollisions;
  out.verdict.worstShankDepth = worstShankDepth;
  out.verdict.worstShankAt = worstShankAt;
  out.verdict.worstShankLine = worstShankLine;
  out.verdict.holderCollisions = holderCollisions;
  out.verdict.worstHolderDepth = worstHolderDepth;
  out.verdict.worstHolderAt = worstHolderAt;
  out.verdict.worstHolderLine = worstHolderLine;
  out.verdict.fixtureCollisions = fixtureCollisions;
  out.verdict.worstFixtureDepth = worstFixtureDepth;
  out.verdict.worstFixtureAt = worstFixtureAt;
  out.verdict.worstFixtureZ = worstFixtureZ;
  out.verdict.worstFixtureLine = worstFixtureLine;
  out.verdict.worstFixtureByHolder = worstFixtureByHolder;
  if (worstFixtureOwner >= 0)
    out.verdict.worstFixtureName = fixtures[worstFixtureOwner].name;
  out.verdict.fixturesChecked =
      fixturesSkipped ? 0 : static_cast<int>(fixtures.size());
  out.verdict.fixturesSkippedRotary = fixturesSkipped;

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
        out.verdict.outsideXY++;
        if (over > out.verdict.worstOvershoot) {
          out.verdict.worstOvershoot = over;
          out.verdict.outsideAxis = axes[a];
        }
        if (over > out.verdict.worstXYOvershoot) {
          out.verdict.worstXYOvershoot = over;
          out.verdict.xyAxis = axes[a];
        }
        if (out.verdict.firstOutsideLine == 0)
          out.verdict.firstOutsideLine = mv.line;
      }
      if (!haveZ || mv.to.z < zLo) {
        zLo = mv.to.z;
        out.verdict.zDeepestLine = mv.line;
      }
      if (!haveZ || mv.to.z > zHi) {
        zHi = mv.to.z;
        out.verdict.zHighestLine = mv.line;
      }
      haveZ = true;
    }
    // Z is a SPAN, not a position, and that distinction is the whole of this
    // paragraph. In X and Y the window is centred on a stock that fits, or at
    // the near corner of a longer one, plus `reach_offset` -- a stated
    // assumption a person can correct. There is no equivalent in Z: nothing
    // says where the machine's Z zero sits relative to the workpiece, because
    // that is what the operator sets when they touch off, AFTER the file is
    // written.
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
      out.verdict.zSpanOver = over;
      out.verdict.zSpan = zHi - zLo;
      out.verdict.zTravel = zTravel;
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
                    const std::vector<Toolpath::Tab>& tabs,
                    const std::vector<FixtureSolid>& fixtures,
                    const OtherSetups* others) {
  // The STOCK MODEL follows the file, not the setup. A program whose A moves
  // is cut from a cylinder that turns and one whose A does not is cut from a
  // block that does not, and asking the setup would get the answer wrong in
  // the case that matters: a rotary-capable machine posting a 3-axis job.
  // Reliefs and tabs are 2.5D statements about a contour and mean nothing to
  // a rotary pass, so they are not carried into one rather than being
  // silently ignored inside it.
  // Fixtures likewise: a bed-space clamp means nothing in the frame of a
  // part that turns, so a rotary file says they were not checked.
  const GProgram program = ProgramInSetupSpace(gcode, setup);
  if (RotatesWhileCutting(program)) {
    Verdict v = InspectRotary(gcode, setup, bodies, res, others).verdict;
    if (others != nullptr)
      v.otherSetupsNotChecked = others->notChecked;
    v.fixturesSkippedRotary = !fixtures.empty();
    // The machine's travel, which a turning program was never checked
    // against. Spans, not positions: where the part sits on the rotary's
    // axis and where Z zero is are the operator's, set after the file is
    // written -- but a program longer along the axis, or taller, than the
    // machine travels cannot run at any setting.
    if (setup.machine.HasWorkArea() && !program.moves.empty()) {
      const glm::dvec3 travel = TravelSpanInSetup(setup);
      glm::dvec3 lo(program.moves.front().to), hi(lo);
      for (const GMove& m : program.moves) {
        lo = glm::min(lo, m.to);
        hi = glm::max(hi, m.to);
      }
      for (const int a : {0, 2}) {
        const double over = (hi[a] - lo[a]) - travel[a];
        if (over <= 1e-6)
          continue;
        v.outsideTravel++;
        if (over > v.worstOvershoot) {
          v.worstOvershoot = over;
          v.outsideAxis = a == 0 ? 'X' : 'Z';
        }
        if (a == 2) {
          v.zSpanOver = over;
          v.zSpan = hi.z - lo.z;
          v.zTravel = travel.z;
        }
      }
    }
    return v;
  }
  Verdict v =
      Inspect(gcode, setup, bodies, res, reliefs, tabs, fixtures, others)
          .verdict;
  if (others != nullptr)
    v.otherSetupsNotChecked = others->notChecked;
  return v;
}

std::vector<VerdictLine> Explain(const Verdict& v, double tolerance,
                                 double minutes, bool breaksAnEdge) {
  std::vector<VerdictLine> out;
  // Long enough for the longest sentence below with its numbers: at 256 the
  // rapid-through-material line lost its last words ("...not been cut y").
  char buf[1024];
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
  if (v.holdingCollisions > 0) {
    std::snprintf(buf, sizeof(buf),
                  "%d move%s reach into %s, worst %.1f mm in at X%.1f (line "
                  "%d). Nothing may come nearer the axis there: keep the "
                  "operation off that end of the part, or grip less of it.",
                  v.holdingCollisions, v.holdingCollisions == 1 ? "" : "s",
                  v.worstHoldingWhat.c_str(), v.worstHoldingDepth,
                  v.worstHoldingX, v.worstHoldingLine);
    out.push_back({VerdictLine::Level::Bad, buf});
  }
  if (!v.holdingDefaults.empty()) {
    std::string list;
    for (const std::string& d : v.holdingDefaults)
      list += (list.empty() ? "" : ", ") + d;
    out.push_back({VerdictLine::Level::Warn,
                   "The chuck and tailstock were checked with defaults, not "
                   "your machine's sizes: " +
                       list +
                       ". They are bigger than most, never smaller -- measure "
                       "yours and put them in the machine profile."});
  }
  if (v.shankCollisions > 0) {
    std::snprintf(buf, sizeof(buf),
                  "%d move%s drive the SHANK into a wall, worst %.2f mm of "
                  "material above the flutes at X%.1f Y%.1f (line %d). The "
                  "shank is wider than the cut, so below the flutes it cannot "
                  "follow the cutter down -- use a cutter with longer flutes, "
                  "or take the depth from both sides.",
                  v.shankCollisions, v.shankCollisions == 1 ? "" : "s",
                  v.worstShankDepth, v.worstShankAt.x, v.worstShankAt.y,
                  v.worstShankLine);
    out.push_back({VerdictLine::Level::Bad, buf});
  }
  if (v.overBites > 0) {
    std::snprintf(buf, sizeof(buf),
                  "%d stretch%s of cutting bite deeper than the operation's "
                  "passes, worst op %llu at X%.1f Y%.1f (line %d): as deep as "
                  "a full-width cut %.1f mm deep, where its passes take %.1f. "
                  "That is the load that breaks cutters and shifts the work.",
                  v.overBites, v.overBites == 1 ? "" : "es",
                  static_cast<unsigned long long>(v.worstBiteOp),
                  v.worstBiteAt.x, v.worstBiteAt.y, v.worstBiteLine,
                  v.worstBite, v.worstBiteAllowed);
    out.push_back({VerdictLine::Level::Bad, buf});
  }
  if (v.fixtureCollisions > 0) {
    // The name travels with the finding: "a fixture" on a bed with four
    // clamps sends the operator to check all four.
    const std::string name = v.worstFixtureName.empty()
                                 ? "a fixture"
                                 : "fixture \"" + v.worstFixtureName + "\"";
    char fb[512]; // the name is the user's, and may be long
    std::snprintf(fb, sizeof(fb),
                  "%d move%s drive the %s into %s, worst %.2f mm at X%.1f "
                  "Y%.1f (line %d). Move the fixture, or keep the path clear "
                  "of it.",
                  v.fixtureCollisions, v.fixtureCollisions == 1 ? "" : "s",
                  v.worstFixtureByHolder ? "HOLDER" : "cutter", name.c_str(),
                  v.worstFixtureDepth, v.worstFixtureAt.x, v.worstFixtureAt.y,
                  v.worstFixtureLine);
    out.push_back({VerdictLine::Level::Bad, fb});
  }
  if (v.fixturesSkippedRotary)
    out.push_back({VerdictLine::Level::Warn,
                   "Fixtures were not checked: this setup turns the part, and "
                   "a clamp on the bed has no place in its turning frame."});
  if (v.outsideXY > 0) {
    std::snprintf(buf, sizeof(buf),
                  "%d move%s ask for somewhere the machine cannot go — %.1f mm "
                  "past its %c travel, first at line %d. The controller alarms "
                  "or hits a stop; say where the bed sits on the sheet "
                  "(reach_offset), or move the part.",
                  v.outsideXY, v.outsideXY == 1 ? "" : "s", v.worstXYOvershoot,
                  v.xyAxis, v.firstOutsideLine);
    out.push_back({VerdictLine::Level::Bad, buf});
  }
  if (v.zSpanOver > 0.0) {
    // A span, not a position: no touch-off height makes it fit, and it has
    // no "first" line — the two ends of the span are what to look at.
    std::snprintf(buf, sizeof(buf),
                  "The program spans %.1f mm of Z (deepest at line %d, highest "
                  "at line %d) and the machine has %.1f mm of Z travel, so it "
                  "cannot run at any touch-off height. Lower the safe or "
                  "clearance height, or split the depth across setups.",
                  v.zSpan, v.zDeepestLine, v.zHighestLine, v.zTravel);
    out.push_back({VerdictLine::Level::Bad, buf});
  }
  if (v.rotary && v.maxLeftover > tolerance &&
      v.leftoverSurfaceMax <= tolerance) {
    // Everything left is on faces turned toward the ends of the axis, which
    // no rotary pass reaches whatever the pattern: said as that, not as the
    // missing operation the sentence below suspects.
    std::snprintf(buf, sizeof(buf),
                  "Leaves %.2f mm on the end faces (%.0f mm2) -- faces turned "
                  "toward the ends of the axis, which no rotary pass reaches. "
                  "The rest of the surface is within tolerance: %.3f mm at "
                  "most.",
                  v.maxLeftover, v.leftoverEndFaceArea, v.leftoverSurfaceMax);
    out.push_back({VerdictLine::Level::Warn, buf});
  } else if (v.rotary && v.leftoverSurfaceMax > tolerance) {
    std::snprintf(buf, sizeof(buf),
                  "Leaves %.2f mm on the surface at X%.1f A%.1f (99%% of it "
                  "within %.3f mm)%s. Check that every operation this part "
                  "needs is in the job, and that the passes are close "
                  "enough.",
                  v.leftoverSurfaceMax, v.leftoverSurfaceAt.x,
                  v.leftoverSurfaceAt.y, v.leftoverSurfaceP99,
                  v.leftoverEndFaceArea > 0.0
                      ? "; the end faces, which no rotary pass "
                        "reaches, leave more"
                      : "");
    out.push_back({VerdictLine::Level::Warn, buf});
  } else if (v.maxLeftover > tolerance) {
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
  // What this setup leaves that another one cuts: said, not counted as
  // leftover (docs/cam/simulation.md#leftover-another-setup-removes).
  for (const Verdict::OtherCut& c : v.leftoverOtherSetups) {
    std::snprintf(buf, sizeof(buf), "%.0f mm2 left here is cut by setup '%s'.",
                  std::max(1.0, c.area), c.name.c_str());
    out.push_back({VerdictLine::Level::Good, buf});
  }
  for (const std::string& why : v.otherSetupsNotChecked)
    out.push_back(
        {VerdictLine::Level::Warn, "Leftover is not compared with " + why +
                                       ": what that cuts reads as left here."});
  if (minutes > 0.0) {
    std::snprintf(buf, sizeof(buf), "~%.0f min of cutting at these feeds.",
                  std::max(1.0, minutes));
    out.push_back({VerdictLine::Level::Good, buf});
  }
  return out;
}

} // namespace cam

namespace cam {

void CheckHolding(const GProgram& program, const Setup& setup,
                  const std::vector<Body>& bodies, Verdict& v) {
  const HoldingEnvelope env =
      HoldingEnvelopeOf(setup, BodiesInSetupFrame(setup, bodies));
  if (!env.ok || program.moves.empty())
    return;
  v.holdingDefaults = env.defaulted;
  // From the first place the file SAYS the tool is. A program begins with a
  // retract at an XY it never states -- the reader's X0 Y0 -- and the rapid
  // out of that is where the machine happens to be parked, which no file
  // knows: it read as a pass over the chuck in every rotary program.
  size_t first = 1;
  while (first < program.moves.size() &&
         glm::dvec2(program.moves[first].to) ==
             glm::dvec2(program.moves.front().to))
    first++;
  if (first >= program.moves.size())
    return;
  glm::dvec3 from = program.moves[first].to;
  for (size_t k = first + 1; k < program.moves.size(); k++) {
    const GMove& m = program.moves[k];
    const Tool* tool = setup.FindTool(m.tool);
    const double r = tool != nullptr ? tool->diameter * 0.5 : 1.0;
    const double len = glm::length(m.to - from);
    const int steps = std::max(1, static_cast<int>(std::ceil(len / 0.5)));
    double worst = 0.0, worstX = 0.0;
    for (int i = 0; i <= steps; i++) {
      const glm::dvec3 p =
          from + (m.to - from) * (static_cast<double>(i) / steps);
      const double d = HoldingDepth(env, p, r);
      if (d > worst) {
        worst = d;
        worstX = p.x;
      }
    }
    if (worst > kGrazeTolerance) {
      v.holdingCollisions++;
      if (worst > v.worstHoldingDepth) {
        v.worstHoldingDepth = worst;
        v.worstHoldingX = worstX;
        v.worstHoldingLine = m.line;
        v.worstHoldingWhat =
            worstX - r <= env.chuckTo ? "the chuck" : "the tailstock";
      }
    }
    from = m.to;
  }
}

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
  } else {
    if (const std::string over = MachineOverhang(setup, bodies); !over.empty())
      out.push_back({VerdictLine::Level::Warn, over});
    // Clipped cutting on parts that FIT is said too, and by operation. It
    // used to ride on the "parts are bigger than the machine" line alone, so
    // an outline clipped by a window in the wrong place was a number on the
    // op and nothing else -- and the verifier, reading the clipped file,
    // called the uncut strip leftover and suggested another operation.
    if (paths != nullptr) {
      std::string which;
      double clipped = 0.0;
      for (const Toolpath& tp : *paths)
        if (tp.clippedMm > 0.0) {
          clipped += tp.clippedMm;
          which += (which.empty() ? "" : ", ") + std::to_string(tp.operationId);
        }
      if (clipped > 0.0) {
        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "%.0f mm of cutting lies beyond %s's travel and is left "
                      "out of the file (op%s ",
                      clipped,
                      setup.machine.name.empty() ? "the machine"
                                                 : setup.machine.name.c_str(),
                      which.find(',') == std::string::npos ? "" : "s");
        out.push_back(
            {VerdictLine::Level::Warn,
             std::string(buf) + which +
                 "). Say where the bed is on the stock (reach_offset), or "
                 "move the work: what is not in the file is not cut"});
      }
    }
  }
  // Cutting left out beside the chuck or the tailstock is the part's own
  // material, and the verifier reads it as leftover: said here, by
  // operation, so the leftover has its reason next to it.
  if (paths != nullptr) {
    std::string which;
    double clipped = 0.0;
    for (const Toolpath& tp : *paths)
      if (tp.holdingClippedMm > 0.0) {
        clipped += tp.holdingClippedMm;
        which += (which.empty() ? "" : ", ") + std::to_string(tp.operationId);
      }
    if (clipped > 0.0) {
      char buf[160];
      std::snprintf(buf, sizeof(buf),
                    "%.0f mm of cutting within reach of the chuck or the "
                    "tailstock is left out (op%s ",
                    clipped, which.find(',') == std::string::npos ? "" : "s");
      out.push_back({VerdictLine::Level::Warn,
                     std::string(buf) + which +
                         "): the stock there stays uncut, and reads as "
                         "leftover. Grip less, or finish that end in another "
                         "setup"});
    }
  }
  // A cut that turns is posted at a SCALED feed, held to what the axes can
  // do, and near the axis that hold bites: the tip then runs slower than the
  // feed chosen. Said here, once per operation, with the same function the
  // post scaled with -- the file says it too, in a comment nobody reads
  // before the job runs. And where the machine does not state how fast its
  // A turns, what the program ASKS of it is said instead, once: that is the
  // number to compare with the motor tuning.
  if (paths != nullptr) {
    const RotaryFeedModel model = FeedModelOf(setup.machine.post);
    const AxisLimits limits = AxisLimitsOf(setup);
    double peakARate = 0.0;
    for (const Toolpath& tp : *paths) {
      size_t capped = 0;
      double wanted = 0.0;
      for (size_t i = 1; i < tp.moves.size(); i++) {
        const RotarySegment seg = SegmentOf(tp.moves[i - 1], tp.moves[i]);
        if (!TurnsWhileCutting(seg))
          continue;
        const ScaledFeed f = PlanFeed(seg, 0.0, model, limits).feed;
        peakARate = std::max(peakARate, f.aRate);
        if (f.clamped) {
          capped++;
          wanted = std::max(wanted, f.wanted);
        }
      }
      if (capped == 0)
        continue;
      char buf[256];
      std::snprintf(buf, sizeof(buf),
                    "Op %llu: %zu turning moves need up to F%.0f to hold the "
                    "feed at the tip, past what the machine's axes can do, so "
                    "they are held back and cut slower. The need grows as the "
                    "radius shrinks; a lower feed keeps them under it.",
                    static_cast<unsigned long long>(tp.operationId), capped,
                    wanted);
      out.push_back({VerdictLine::Level::Warn, buf});
    }
    if (limits.rotary <= 0.0 && peakARate > 0.0) {
      char buf[256];
      std::snprintf(buf, sizeof(buf),
                    "The program turns A at up to %.0f deg/min while cutting. "
                    "The machine does not state its A top speed "
                    "(a_max_deg_min), so the cycle time assumes the axis "
                    "keeps up.",
                    peakARate);
      out.push_back({VerdictLine::Level::Warn, buf});
    }
  }
  return out;
}

} // namespace cam
