#include "core/cam/CamComp.h"

#include "core/cam/CamToolpath.h"
#include "core/cam/CamToolpathInternal.h"

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numbers>
#include <optional>
#include <vector>

namespace cam {

namespace {

using detail::kDeflection;
using detail::Round2;

constexpr double kSame = 1e-9; // mm: two XY positions that are one

glm::dvec2 Left(const glm::dvec2& v) { return {-v.y, v.x}; }
double Cross(const glm::dvec2& a, const glm::dvec2& b) {
  return a.x * b.y - a.y * b.x;
}

// The direction the run [first, last] travels in XY at its start (`atEnd`
// false) or its end, ignoring moves that only change Z (a ramp's steep first
// step still has the contour's heading; a tab's lift has none).
bool Heading(const std::vector<Move>& moves, size_t first, size_t last,
             bool atEnd, glm::dvec2& dir) {
  for (size_t n = 0; n <= last - first; n++) {
    const size_t k = atEnd ? last - n : first + n;
    const glm::dvec2 d = glm::dvec2(moves[k].to) - glm::dvec2(moves[k - 1].to);
    if (glm::length(d) > 1e-6) {
      dir = glm::normalize(d);
      return true;
    }
  }
  return false;
}

// A quarter turn round `centre` from `from` (not included), turning
// anticlockwise when `ccw`: the chords' ends, the last being the turn's end.
// As many as the chord tolerance the walls were flattened to asks for, and at
// least four, so the post fits it back into one arc.
std::vector<glm::dvec2> QuarterTurn(const glm::dvec2& centre,
                                    const glm::dvec2& from, bool ccw) {
  const double r = glm::length(from - centre);
  const double perSeg =
      r > kDeflection ? 2.0 * std::acos(1.0 - kDeflection / r) : 1.0;
  const int n =
      std::max(4, static_cast<int>(std::ceil(std::numbers::pi * 0.5 / perSeg)));
  const double a0 = std::atan2(from.y - centre.y, from.x - centre.x);
  std::vector<glm::dvec2> out;
  for (int i = 1; i <= n; i++) {
    const double a = a0 + (ccw ? 1.0 : -1.0) * std::numbers::pi * 0.5 * i / n;
    out.push_back(centre + r * glm::dvec2(std::cos(a), std::sin(a)));
  }
  return out;
}

// The direction of travel at `p` on a circle round `centre`.
glm::dvec2 Tangent(const glm::dvec2& centre, const glm::dvec2& p, bool ccw) {
  const glm::dvec2 t = glm::normalize(Left(p - centre));
  return ccw ? t : -t;
}

// Winding number of `p` about the closed polyline `poly`: non-zero inside.
int Winding(const std::vector<glm::dvec2>& poly, const glm::dvec2& p) {
  int w = 0;
  for (size_t i = 0; i < poly.size(); i++) {
    const glm::dvec2& a = poly[i];
    const glm::dvec2& b = poly[(i + 1) % poly.size()];
    if (a.y <= p.y) {
      if (b.y > p.y && Cross(b - a, p - a) > 0.0)
        w++;
    } else if (b.y <= p.y && Cross(b - a, p - a) < 0.0) {
      w--;
    }
  }
  return w;
}

// Where a controller offsetting `pts` sideways by `w` (either way) cannot:
// at a turn TOWARD the offset the two offset segments meet short of the
// corner, `w tan(theta / 2)` back along each, and a segment shorter than what
// its two ends take reverses -- LinuxCNC refuses that ("gouging"), Mach3 cuts
// it. A path that doubles back is the same at any wear. Empty when every
// segment survives.
struct Uncompensable {
  glm::dvec2 at{0.0};
  double length = 0.0; // the segment that does not survive, mm
  double turnDeg = 0.0;
  bool reversal = false;
};
std::optional<Uncompensable>
FindUncompensable(const std::vector<glm::dvec2>& pts, double w) {
  const size_t n = pts.size();
  if (n < 3)
    return std::nullopt;
  std::vector<double> turn(n, 0.0);
  for (size_t i = 1; i + 1 < n; i++) {
    const glm::dvec2 a = pts[i] - pts[i - 1], b = pts[i + 1] - pts[i];
    turn[i] = std::atan2(Cross(a, b), glm::dot(a, b));
    if (std::abs(turn[i]) > std::numbers::pi * (175.0 / 180.0)) {
      Uncompensable u;
      u.at = pts[i];
      u.turnDeg = std::abs(turn[i]) * 180.0 / std::numbers::pi;
      u.reversal = true;
      return u;
    }
  }
  for (const double side : {1.0, -1.0}) {
    const auto consumed = [&](size_t i) {
      return turn[i] * side > 0.0 ? w * std::tan(std::abs(turn[i]) * 0.5) : 0.0;
    };
    for (size_t j = 0; j + 1 < n; j++) {
      const double len = glm::length(pts[j + 1] - pts[j]);
      const double need = consumed(j) + consumed(j + 1);
      if (need > len + 1e-9) {
        Uncompensable u;
        u.at = (pts[j] + pts[j + 1]) * 0.5;
        u.length = len;
        u.turnDeg = std::max(std::abs(turn[j]), std::abs(turn[j + 1])) * 180.0 /
                    std::numbers::pi;
        return u;
      }
    }
  }
  return std::nullopt;
}

std::string At(const glm::dvec2& p, const glm::dvec3& origin) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "X%.3f Y%.3f", p.x - origin.x,
                p.y - origin.y);
  return buf;
}

} // namespace

bool CompensateWalls(Toolpath& tp, const Tool& tool, const glm::dvec3& origin,
                     std::string& why) {
  tp.comps.clear();
  if (tp.walls.empty())
    return true;
  if (!tp.arcs.empty()) {
    // Nothing that declares arcs declares walls today; a strategy that does
    // both needs its arcs moved with the inserted leads.
    why = "cutter compensation on a path that declares its own arcs is not "
          "built";
    return false;
  }
  const double wear = CompCheckedWear(tool);
  const double w = wear * 0.5;             // on the radius
  const double rho = tool.diameter * 0.25; // lead arc: half the radius
  const double line = tool.diameter * 0.25;
  std::vector<Toolpath::Wall> walls = tp.walls;
  std::sort(walls.begin(), walls.end(),
            [](const Toolpath::Wall& a, const Toolpath::Wall& b) {
              return a.first < b.first;
            });
  const std::vector<Move>& in = tp.moves;
  std::vector<Move> out;
  out.reserve(in.size() + walls.size() * 24);
  std::vector<Toolpath::Comp> comps;
  size_t cursor = 0;
  for (const Toolpath::Wall& wall : walls) {
    if (wall.first == 0 || wall.first > wall.last || wall.last >= in.size() ||
        wall.first < cursor) {
      why = "a wall pass was declared over moves that are not one (a bug: "
            "please report it)";
      return false;
    }
    const glm::dvec3 p0 = in[wall.first - 1].to;
    const glm::dvec3 pn = in[wall.last].to;
    glm::dvec2 t0(0.0), tn(0.0);
    if (!Heading(in, wall.first, wall.last, false, t0) ||
        !Heading(in, wall.first, wall.last, true, tn)) {
      why = "a wall pass with no length to compensate";
      return false;
    }
    // The waste is on the side away from the material.
    const auto wasteOf = [&](const glm::dvec2& t) {
      return wall.materialRight ? Left(t) : -Left(t);
    };

    // The lead-in: a quarter arc onto the wall's first point, tangent to it,
    // and the line before it that compensation comes on along.
    const glm::dvec2 c0 = glm::dvec2(p0) + wasteOf(t0) * rho;
    const bool ccwIn = Cross(glm::dvec2(p0) - c0, t0) > 0.0;
    std::vector<glm::dvec2> back = QuarterTurn(c0, glm::dvec2(p0), !ccwIn);
    const glm::dvec2 a = back.back();
    const glm::dvec2 l = a - Tangent(c0, a, ccwIn) * line;
    std::vector<glm::dvec2> arcIn(back.rbegin() + 1, back.rend());
    arcIn.push_back(glm::dvec2(p0));
    // The lead-out: a quarter arc off the wall into the waste, and the line
    // compensation goes off along.
    const glm::dvec2 cn = glm::dvec2(pn) + wasteOf(tn) * rho;
    const bool ccwOut = Cross(glm::dvec2(pn) - cn, tn) > 0.0;
    const std::vector<glm::dvec2> arcOut =
        QuarterTurn(cn, glm::dvec2(pn), ccwOut);
    const glm::dvec2 b = arcOut.back();
    const glm::dvec2 e = b + Tangent(cn, b, ccwOut) * line;

    // Room: every lead point on the waste side of the wall pass -- inside the
    // bore, the pocket or the opening it walks round, outside a part it
    // walks round. The pass itself, in XY, is the boundary.
    std::vector<glm::dvec2> pass{glm::dvec2(p0)};
    for (size_t k = wall.first; k <= wall.last; k++)
      if (glm::length(glm::dvec2(in[k].to) - pass.back()) > kSame)
        pass.push_back(glm::dvec2(in[k].to));
    double area2 = 0.0;
    for (size_t i = 0; i < pass.size(); i++)
      area2 += Cross(pass[i], pass[(i + 1) % pass.size()]);
    const bool wasteInside = wall.materialRight == (area2 > 0.0);
    for (const glm::dvec2& p :
         {l, a, back[back.size() / 2], b, e, arcOut[arcOut.size() / 2]}) {
      if ((Winding(pass, p) != 0) != wasteInside) {
        why = "no room for cutter compensation's lead-in and lead-out by the "
              "wall at " +
              At(glm::dvec2(p0), origin) + ": they need " + Round2(rho + line) +
              " mm on the " + (wasteInside ? "inside" : "outside") +
              " of the cut, clear of the far wall. Use a smaller tool, or "
              "turn cutter_comp off for this operation";
        return false;
      }
    }

    // Everything before the wall, with the approach rerouted to the lead-in's
    // start: the rapids that came down over the wall's first point come down
    // over it instead, and a link at depth runs to it.
    out.insert(out.end(), in.begin() + static_cast<std::ptrdiff_t>(cursor),
               in.begin() + static_cast<std::ptrdiff_t>(wall.first));
    if (out.empty() ||
        (!comps.empty() && out.size() == comps.back().last + 1)) {
      // Nothing between this wall and the last one's lead-out (or before it
      // at all): a link at depth from where the tool stands.
      Move link;
      link.kind = Move::Kind::Cut;
      link.to = glm::dvec3(l, p0.z);
      link.feed = tool.feedCut;
      link.hasA = in[wall.first].hasA;
      link.a = in[wall.first].a;
      out.push_back(link);
    } else {
      size_t k = out.size();
      if (out[k - 1].kind == Move::Kind::Rapid) {
        while (k > 0 && out[k - 1].kind == Move::Kind::Rapid &&
               glm::length(glm::dvec2(out[k - 1].to) - glm::dvec2(p0)) <
                   kSame) {
          out[k - 1].to.x = l.x;
          out[k - 1].to.y = l.y;
          k--;
        }
        if (k == out.size()) {
          why = "a wall pass that does not start where its approach ends (a "
                "bug: please report it)";
          return false;
        }
      } else {
        out[k - 1].to.x = l.x;
        out[k - 1].to.y = l.y;
      }
    }
    const Move& firstMove = in[wall.first];
    const auto emit = [&](const glm::dvec2& p, double z) {
      Move m;
      m.kind = Move::Kind::Cut;
      m.to = glm::dvec3(p, z);
      m.feed = tool.feedCut;
      m.hasA = firstMove.hasA;
      m.a = firstMove.a;
      out.push_back(m);
    };
    Toolpath::Comp comp;
    comp.left = wall.materialRight;
    comp.checkedWear = wear;
    comp.first = out.size();
    emit(a, p0.z);
    for (const glm::dvec2& p : arcIn)
      emit(p, p0.z);
    out.insert(out.end(), in.begin() + static_cast<std::ptrdiff_t>(wall.first),
               in.begin() + static_cast<std::ptrdiff_t>(wall.last) + 1);
    for (const glm::dvec2& p : arcOut)
      emit(p, pn.z);
    comp.last = out.size();
    emit(e, pn.z);

    // What the controller is asked to offset, checked: the lead-in line's
    // start, the run, the lead-out line's end.
    std::vector<glm::dvec2> run{l};
    for (size_t k = comp.first; k <= comp.last; k++)
      if (glm::length(glm::dvec2(out[k].to) - run.back()) > kSame)
        run.push_back(glm::dvec2(out[k].to));
    if (const std::optional<Uncompensable> bad = FindUncompensable(run, w)) {
      if (bad->reversal)
        why = "the path doubles back on itself at " + At(bad->at, origin) +
              " (a corner relief's bite does), which no controller can "
              "offset: turn the corner relief or cutter_comp off";
      else
        why = "the path turns " + Round2(bad->turnDeg) + " deg within " +
              Round2(bad->length) + " mm at " + At(bad->at, origin) +
              ": an inside corner whose radius is within " + Round2(w) +
              " mm of the cutter's, which the controller cannot offset by "
              "the " +
              Round2(wear) +
              " mm of wear the program is checked for. Give the corner a "
              "radius at least that much larger than the cutter's, or a "
              "smaller cutter, or turn cutter_comp off";
      return false;
    }
    comps.push_back(comp);
    cursor = wall.last + 1;
    // The retract after the lead-out: a rapid straight up over the wall's
    // last point rises over the lead-out's end instead.
    if (cursor < in.size() && in[cursor].kind == Move::Kind::Rapid &&
        glm::length(glm::dvec2(in[cursor].to) - glm::dvec2(pn)) < kSame) {
      Move up = in[cursor];
      up.to.x = e.x;
      up.to.y = e.y;
      out.push_back(up);
      cursor++;
    }
  }
  out.insert(out.end(), in.begin() + static_cast<std::ptrdiff_t>(cursor),
             in.end());
  tp.moves = std::move(out);
  tp.comps = std::move(comps);
  return true;
}

} // namespace cam
