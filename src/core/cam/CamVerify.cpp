#include "core/cam/CamVerify.h"

#include "core/cam/CamToolpath.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <limits>

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

} // namespace

GProgram ParseGcode(std::string_view text) {
  GProgram out;
  glm::dvec3 pos(0.0);
  bool rapid = true;
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
        if (value == 0.0) {
          rapid = true;
          motion = true;
        } else if (value == 1.0) {
          rapid = false;
          motion = true;
        }
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
      case 'T':
        tool = static_cast<int>(value);
        break;
      default:
        break;
      }
      k = numEnd - 1;
    }

    if (sawAxis || (motion && havePos)) {
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
        const double toolZ = from.z + (m.to.z - from.z) * t;
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

Verdict Compare(const HeightMap& machined, const HeightMap& designed) {
  Verdict v;
  if (machined.nx != designed.nx || machined.ny != designed.ny)
    return v;
  for (int j = 0; j < machined.ny; j++) {
    for (int i = 0; i < machined.nx; i++) {
      const double got = machined.At(i, j);
      const double want = designed.At(i, j);
      v.cells++;
      if (want - got > v.maxGouge) {
        v.maxGouge = want - got;
        v.gougeAt = {machined.CellX(i), machined.CellY(j)};
      }
      if (got - want > v.maxLeftover) {
        v.maxLeftover = got - want;
        v.leftoverAt = {machined.CellX(i), machined.CellY(j)};
      }
    }
  }
  return v;
}

Inspection Inspect(const std::string& gcode, const Setup& setup,
                   const std::vector<Body>& bodies, double res) {
  Inspection out;
  glm::dvec3 lo(0.0), hi(0.0);
  if (!StockBounds(setup, bodies, lo, hi))
    return out;

  const GProgram program = ParseGcode(gcode);
  HeightMap machined = MakeStock(lo, hi, res);

  // Rapids through material: checked BEFORE the cut is applied, because a
  // rapid is only safe over air. This is the check a shape comparison cannot
  // make — the part can come out perfect and the tool still be broken.
  int rapidsThroughStock = 0;
  int firstBadLine = 0;
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
            if (probe.At(i, j) > zLow + 1e-6)
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
      if (!m.rapid)
        Simulate(one, setup, probe);
      from = m.to;
    }
    machined = probe;
  }

  out.designed = SampleModel(bodies, machined, lo.z);
  out.verdict = Compare(machined, out.designed);
  out.verdict.rapidsThroughStock = rapidsThroughStock;
  out.verdict.firstBadLine = firstBadLine;
  out.machined = std::move(machined);
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
  size_t total = 0;
  for (int j = 0; j < insp.machined.ny; j++)
    for (int i = 0; i < insp.machined.nx; i++)
      if (insp.machined.At(i, j) - insp.designed.At(i, j) > tolerance)
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
      if (insp.machined.At(i, j) - insp.designed.At(i, j) <= tolerance)
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
    out.push_back({VerdictLine::Level::Warn,
                   "Nothing was checked — this job has no stock to cut."});
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
    std::snprintf(buf, sizeof(buf),
                  "Leaves %.2f mm of material at X%.1f Y%.1f — usually an "
                  "inside corner the cutter cannot reach. A smaller tool gets "
                  "in there.",
                  v.maxLeftover, v.leftoverAt.x, v.leftoverAt.y);
    out.push_back({VerdictLine::Level::Warn, buf});
  }
  if (out.empty())
    out.push_back({VerdictLine::Level::Good,
                   "Machines the part. No gouges, nothing "
                   "left over."});
  if (minutes > 0.0) {
    std::snprintf(buf, sizeof(buf), "~%.0f min of cutting at these feeds.",
                  std::max(1.0, minutes));
    out.push_back({VerdictLine::Level::Good, buf});
  }
  return out;
}

} // namespace cam
