#include "core/cam/CamVerify.h"

#include "core/cam/CamToolpath.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <limits>
#include <numbers>

namespace cam {

namespace {

// How finely an arc is re-expanded into chords for simulation: the same
// tolerance the toolpath was flattened at, so a fitted arc returns to the
// polyline it came from.
constexpr double kArcChordTolerance = 0.02;

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

GProgram ParseGcode(std::string_view text) {
  GProgram out;
  glm::dvec3 pos(0.0);
  bool rapid = true;
  // 0 = G0, 1 = G1, 2 = G2 (clockwise arc), 3 = G3. Modal, like everything
  // else here: a block with only coordinates repeats the last motion.
  int mode = 0;
  int tool = 1;
  int lineNo = 0;
  bool havePos = false;

  size_t i = 0;
  while (i <= text.size()) {
    const size_t end = std::min(text.find('\n', i), text.size());
    std::string_view line = text.substr(i, end - i);
    i = end + 1;
    lineNo++;

    // Strip parenthesised comments and anything after a semicolon.
    std::string clean;
    clean.reserve(line.size());
    int depth = 0;
    for (const char c : line) {
      if (c == '(')
        depth++;
      else if (c == ')')
        depth = std::max(0, depth - 1);
      else if (c == ';')
        break;
      else if (depth == 0)
        clean.push_back(static_cast<char>(std::toupper(c)));
    }
    if (clean.empty())
      continue;

    glm::dvec3 to = pos;
    bool motion = false;
    bool sawAxis = false;
    // Arc centre, INCREMENTAL from the arc start (the post states G91.1).
    glm::dvec2 ij(0.0);
    for (size_t k = 0; k < clean.size(); k++) {
      const char word = clean[k];
      if (!std::isalpha(static_cast<unsigned char>(word)))
        continue;
      size_t numEnd = k + 1;
      while (numEnd < clean.size() &&
             (std::isdigit(static_cast<unsigned char>(clean[numEnd])) ||
              clean[numEnd] == '.' || clean[numEnd] == '-' ||
              clean[numEnd] == '+'))
        numEnd++;
      const std::string num(clean.substr(k + 1, numEnd - k - 1));
      double value = 0.0;
      if (!num.empty()) {
        try {
          value = std::stod(num);
        } catch (...) {
          out.errors.push_back("line " + std::to_string(lineNo) +
                               ": unreadable number '" + num + "'");
          k = numEnd - 1;
          continue;
        }
      } else if (word == 'X' || word == 'Y' || word == 'Z') {
        // "Xnan" lands here: the axis word exists with no parseable number.
        out.errors.push_back("line " + std::to_string(lineNo) +
                             ": axis word with no number");
      }
      switch (word) {
      case 'G':
        if (value == 0.0 || value == 1.0 || value == 2.0 || value == 3.0) {
          mode = static_cast<int>(value);
          rapid = mode == 0;
          motion = true;
        }
        // Everything else (G17/G21/G90/G91.1/G40…) is state this simulator
        // does not model. G91.1 in particular is ASSUMED — the post always
        // emits incremental centres, and reading them as absolute would put
        // every arc somewhere else entirely.
        break;
      case 'X':
        to.x = value;
        sawAxis = true;
        break;
      case 'Y':
        to.y = value;
        sawAxis = true;
        break;
      case 'Z':
        to.z = value;
        sawAxis = true;
        break;
      case 'I':
        ij.x = value;
        break;
      case 'J':
        ij.y = value;
        break;
      case 'T':
        tool = static_cast<int>(value);
        break;
      default:
        break;
      }
      k = numEnd - 1;
    }

    if (sawAxis || (motion && havePos)) {
      if (mode >= 2 && havePos) {
        // An arc becomes chords here, so Simulate and every check downstream
        // keep seeing straight segments. The sagitta bound is the same chord
        // tolerance the toolpath was flattened at, so re-expanding an arc
        // the post fitted returns to where it started.
        const glm::dvec2 centre = glm::dvec2(pos) + ij;
        const double radius = glm::length(glm::dvec2(pos) - centre);
        if (radius > 1e-9) {
          const glm::dvec2 u = glm::dvec2(pos) - centre;
          const glm::dvec2 v = glm::dvec2(to) - centre;
          double sweep = std::atan2(u.x * v.y - u.y * v.x, glm::dot(u, v));
          // G2 is clockwise seen from +Z: a non-negative sweep means the arc
          // goes the long way round.
          if (mode == 2 && sweep > 0.0)
            sweep -= 2.0 * std::numbers::pi;
          if (mode == 3 && sweep < 0.0)
            sweep += 2.0 * std::numbers::pi;
          const double perSeg =
              radius > kArcChordTolerance
                  ? 2.0 * std::acos(1.0 - kArcChordTolerance / radius)
                  : 1.0;
          const int steps = std::clamp(
              static_cast<int>(std::ceil(std::abs(sweep) / perSeg)), 1, 4096);
          const double a0 = std::atan2(u.y, u.x);
          for (int k = 1; k <= steps; k++) {
            const double t = static_cast<double>(k) / steps;
            const double a = a0 + sweep * t;
            GMove m;
            m.rapid = false;
            m.to = {centre.x + radius * std::cos(a),
                    centre.y + radius * std::sin(a),
                    pos.z + (to.z - pos.z) * t};
            m.tool = tool;
            m.line = lineNo;
            out.moves.push_back(m);
          }
          pos = to;
          havePos = true;
          continue;
        }
        out.errors.push_back("line " + std::to_string(lineNo) +
                             ": arc with no radius (I/J missing?)");
      }
      GMove m;
      m.rapid = rapid;
      m.to = to;
      m.tool = tool;
      m.line = lineNo;
      out.moves.push_back(m);
      pos = to;
      havePos = true;
    }
  }
  return out;
}

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

void Simulate(const GProgram& program, const Setup& setup, HeightMap& stock) {
  glm::dvec3 from(0.0);
  bool have = false;
  for (const GMove& m : program.moves) {
    if (!have) {
      from = m.to;
      have = true;
      continue;
    }
    const Tool* tool = setup.FindTool(m.tool);
    const double radius = tool != nullptr ? tool->diameter * 0.5 : 1.0;

    // Cells the swept tool can touch, from the segment's bounding box grown
    // by the radius. Everything outside cannot be reached, so it is not
    // tested at all.
    const glm::dvec2 a(from.x, from.y), b(m.to.x, m.to.y);
    const bool xyStill = glm::length(b - a) < 1e-9;
    const double lo_x = std::min(a.x, b.x) - radius;
    const double hi_x = std::max(a.x, b.x) + radius;
    const double lo_y = std::min(a.y, b.y) - radius;
    const double hi_y = std::max(a.y, b.y) + radius;
    const int i0 = std::max(0, static_cast<int>((lo_x - stock.x0) / stock.res));
    const int i1 = std::min(
        stock.nx - 1, static_cast<int>((hi_x - stock.x0) / stock.res) + 1);
    const int j0 = std::max(0, static_cast<int>((lo_y - stock.y0) / stock.res));
    const int j1 = std::min(
        stock.ny - 1, static_cast<int>((hi_y - stock.y0) / stock.res) + 1);

    for (int j = j0; j <= j1; j++) {
      for (int i = i0; i <= i1; i++) {
        double t = 0.0;
        const glm::dvec2 c(stock.CellX(i), stock.CellY(j));
        if (DistToSegment(c, a, b, t) > radius)
          continue;
        // A flat end mill: the tool bottom is the interpolated Z. (A profile
        // function would go here for ball/bull/V — the shape FreeCAD's
        // cSimTool uses, and the reason to keep this one line separate.)
        //
        // `t` is the parameter of the CLOSEST point on the XY segment, which
        // is 0 for a move that does not travel in XY at all — so a straight
        // plunge removed material down to where it STARTED, which is to say
        // none. Every drilled hole came out full of stock, and the retract
        // out of it was then reported as a rapid through material. A move
        // that stands still in XY reaches its lowest point over every cell it
        // covers.
        const double toolZ =
            xyStill ? std::min(from.z, m.to.z) : from.z + (m.to.z - from.z) * t;
        if (stock.At(i, j) > toolZ)
          stock.Set(i, j, static_cast<float>(toolZ));
      }
    }
    from = m.to;
  }
}

HeightMap SampleModel(const std::vector<Body>& bodies, const HeightMap& like,
                      double floorZ) {
  HeightMap out = like;
  out.z.assign(out.z.size(), static_cast<float>(floorZ));

  for (const Body& body : bodies) {
    if (!body.shape.IsValid())
      continue;
    const CADShape::Mesh mesh = body.shape.GenerateMesh(0.05);
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
                const std::vector<uint8_t>* touched, double leftoverTolerance) {
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
      const double got = machined.At(i, j);
      v.cells++;
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
      if (wantLo - got > v.maxGouge) {
        v.maxGouge = wantLo - got;
        v.gougeAt = {machined.CellX(i), machined.CellY(j)};
      }
      if (got - wantHi > v.maxLeftover) {
        v.maxLeftover = got - wantHi;
        v.leftoverAt = {machined.CellX(i), machined.CellY(j)};
      }
      if (got - wantHi > leftTol)
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
                   const std::vector<Body>& rawBodies, double res) {
  Inspection out;
  // The G-code is in SETUP space, so the model it is compared against has to
  // be too (C9). Verifying an indexed job against the unrotated part
  // compares a program with a part that is not in front of it.
  const std::vector<Body> bodies = BodiesInSetupFrame(setup, rawBodies);
  glm::dvec3 lo(0.0), hi(0.0);
  if (!StockBounds(setup, rawBodies, lo, hi))
    return out;

  const GProgram program = ParseGcode(gcode);
  HeightMap machined = MakeStock(lo, hi, res);

  // Rapids through material: checked BEFORE the cut is applied, because a
  // rapid is only safe over air. This is the check a shape comparison cannot
  // make — the part can come out perfect and the tool still be broken.
  int rapidsThroughStock = 0;
  int firstBadLine = 0;
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
        for (int j = j0; j <= j1 && !hit; j++) {
          for (int i = i0; i <= i1 && !hit; i++) {
            double t = 0.0;
            const glm::dvec2 c(probe.CellX(i), probe.CellY(j));
            if (DistToSegment(c, a, b, t) > radius)
              continue;
            if (probe.At(i, j) > zLow + kGrazeTolerance)
              hit = true;
          }
        }
        if (hit) {
          rapidsThroughStock++;
          if (firstBadLine == 0)
            firstBadLine = m.line;
        }
      }
      // Apply every move (cuts remove material; rapids over air do nothing).
      GProgram one;
      one.moves = {GMove{false, from, m.tool, m.line}, m};
      if (!m.rapid) {
        Simulate(one, setup, probe);
        MarkSwept(from, m, setup, probe, touched);
      }
      from = m.to;
    }
    machined = probe;
  }

  out.designed = SampleModel(bodies, machined, lo.z);
  out.verdict = Compare(machined, out.designed, &touched);
  out.verdict.rapidsThroughStock = rapidsThroughStock;
  out.verdict.firstBadLine = firstBadLine;
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
                    const std::vector<Body>& bodies, double res) {
  return Inspect(gcode, setup, bodies, res).verdict;
}

std::vector<VerdictLine> Explain(const Verdict& v, double tolerance,
                                 double minutes) {
  std::vector<VerdictLine> out;
  char buf[256];

  if (v.cells == 0) {
    out.push_back(
        {VerdictLine::Level::Warn,
         v.untouchedFraction >= 0.999
             ? "This program never touches the stock — it cuts nothing."
             : "Nothing was checked — this job has no stock to cut."});
    return out;
  }
  // Gouging first: it is the one that scraps the part, and the only one a
  // backplot genuinely cannot show you.
  if (v.maxGouge > tolerance) {
    std::snprintf(buf, sizeof(buf),
                  "Cuts %.2f mm INTO the part at X%.1f Y%.1f — that is scrap. "
                  "Check the depths and the tool diameter.",
                  v.maxGouge, v.gougeAt.x, v.gougeAt.y);
    out.push_back({VerdictLine::Level::Bad, buf});
  }
  if (v.rapidsThroughStock > 0) {
    std::snprintf(buf, sizeof(buf),
                  "Rapid through material at line %d — raise Safe Z, or start "
                  "the job higher.",
                  v.firstBadLine);
    out.push_back({VerdictLine::Level::Bad, buf});
  }
  if (v.maxLeftover > tolerance) {
    // The AREA is what makes this readable: four inside corners a 3 mm cutter
    // cannot reach are four regions of about a square millimetre, and a
    // forgotten operation is one region of hundreds. Both used to print the
    // same sentence.
    std::snprintf(buf, sizeof(buf),
                  "Leaves %.2f mm of material at X%.1f Y%.1f — %.1f mm2 in %d "
                  "place%s. %s",
                  v.maxLeftover, v.leftoverAt.x, v.leftoverAt.y, v.leftoverArea,
                  v.leftoverRegions, v.leftoverRegions == 1 ? "" : "s",
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
