#include "core/cam/CamPost.h"

#include "core/cam/CamMount.h"

#include "core/cam/CamRotaryFeed.h"
#include "core/cam/CamToolpath.h"

#include <glm/geometric.hpp>
#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numbers>
#include <sstream>
#include <vector>

namespace cam {

namespace {

// A coordinate, trimmed of trailing zeros. Machines do not care, humans
// reading a diff do, and the golden files are read by both.
std::string Num(double v, int decimals) {
  char buf[64];
  std::snprintf(buf, sizeof buf, "%.*f", decimals, v);
  std::string s(buf);
  if (s.find('.') != std::string::npos) {
    while (!s.empty() && s.back() == '0')
      s.pop_back();
    if (!s.empty() && s.back() == '.')
      s.pop_back();
  }
  if (s == "-0")
    s = "0";
  return s;
}

struct Emitter {
  std::ostringstream out;
  int decimals = 4;
  int line = 0;
  // The work origin, in setup space. EVERY coordinate this emitter writes is
  // relative to it, and this is the only place the subtraction happens — the
  // engine works in setup space throughout, and the file is the one artifact
  // that has to speak the operator's numbers. Without it the panel's "Z zero
  // at 16.000" was a claim no line of the file honoured
  // (docs/cam/post.md#work-origin-in-the-file).
  glm::dvec3 origin{0.0, 0.0, 0.0};
  // How the rotary is installed (CamMount.h): the quarter turn from setup
  // space to the machine, the sign of A and its letter. Identity for a setup
  // on the bed -- and the same place as the origin, so one function says
  // where every coordinate in the file lands.
  Mount mount;
  [[nodiscard]] glm::dvec3 Work(const glm::dvec3& p) const {
    return ToMachine(mount, p - origin);
  }
  // The rotary word as the machine reads it: its letter, its sign.
  [[nodiscard]] std::string AWord(double internal, int dp) const {
    return std::string(1, mount.word) + Num(mount.sign * internal, dp);
  }
  // Modal state: G-code repeats nothing it does not have to, and a file that
  // restates X on every line is unreadable when something goes wrong.
  double x = std::nan("");
  double y = std::nan("");
  double z = std::nan("");
  double feed = std::nan("");
  bool haveA = false;
  double a = std::nan("");

  // A comment carries text nobody here chose: the setup's name, a tool's
  // name, an operation's name inside a SKIPPED line. G-code comments do NOT
  // NEST, so one parenthesis in any of them is a file the controller refuses
  // to load -- Mach3 says "Nested comment found" and stops, with the offending
  // name being something a person typed in a panel weeks earlier. Brackets
  // say the same thing and cannot nest; a newline would end the comment and
  // turn its tail into a command, so those go too.
  // Text safe to sit between parentheses. Brackets say the same thing and
  // cannot nest; a newline would end the comment and turn its tail into a
  // command.
  static std::string Safe(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
      if (c == '(')
        out += '[';
      else if (c == ')')
        out += ']';
      else if (c == '\n' || c == '\r')
        out += ' ';
      else
        out += c;
    }
    return out;
  }
  void Comment(const std::string& text) { out << "(" << Safe(text) << ")\n"; }
  void Raw(const std::string& text) { out << text << "\n"; }

  static bool Differs(double now, double before) {
    return std::isnan(before) || std::abs(now - before) > 1e-9;
  }

  // A Z-only rapid. Retracts happen BEFORE anything has moved (the first
  // tool change) and after the last move, and passing the modal X/Y through
  // Motion() emitted "G0 Xnan Ynan Z20" — a line that is at best rejected and
  // at worst interpreted. Retract touches Z and nothing else.
  void Retract(double zTo) {
    const double w = zTo - origin.z;
    if (!Differs(w, z))
      return;
    out << "G0 Z" << Num(w, decimals) << "\n";
    z = w;
  }

  // An arc in the XY plane (G17), centre given INCREMENTALLY from the arc
  // start as I/J — which is why the preamble states G91.1. On a machine
  // configured for absolute centres, incremental I/J puts the centre near
  // the work origin and the tool takes a full-radius swing across the table;
  // stating the mode is the difference between a curve and a crash.
  void Arc(bool clockwise, const glm::dvec3& to, const glm::dvec2& centre,
           double f) {
    const glm::dvec3 w = Work(to);
    // The centre moves with everything else, so I/J — being the difference
    // between two points in the SAME space — come out unchanged. Shifting it
    // here anyway rather than relying on that keeps one rule instead of two.
    const glm::dvec3 cw =
        ToMachine(mount, glm::dvec3(centre.x, centre.y, 0.0) - origin);
    const glm::dvec2 c(cw.x, cw.y);
    std::ostringstream l;
    l << (clockwise ? "G2" : "G3");
    l << " X" << Num(w.x, decimals) << " Y" << Num(w.y, decimals);
    if (Differs(w.z, z))
      l << " Z" << Num(w.z, decimals);
    l << " I" << Num(c.x - x, decimals) << " J" << Num(c.y - y, decimals);
    if (f > 0.0 && Differs(f, feed)) {
      l << " F" << Num(f, decimals);
      feed = f;
    }
    out << l.str() << "\n";
    x = w.x;
    y = w.y;
    z = w.z;
  }

  void Motion(const char* code, const glm::dvec3& in, double f, bool withA,
              double aVal) {
    const glm::dvec3 to = Work(in);
    std::ostringstream l;
    l << code;
    if (Differs(to.x, x))
      l << " X" << Num(to.x, decimals);
    if (Differs(to.y, y))
      l << " Y" << Num(to.y, decimals);
    if (Differs(to.z, z))
      l << " Z" << Num(to.z, decimals);
    if (withA && Differs(aVal, a))
      l << " " << AWord(aVal, decimals);
    if (f > 0.0 && Differs(f, feed)) {
      l << " F" << Num(f, decimals);
      feed = f;
    }
    const std::string body = l.str();
    // Nothing changed: a bare "G1" is legal and useless.
    if (body.size() == std::string(code).size())
      return;
    out << body << "\n";
    x = to.x;
    y = to.y;
    z = to.z;
    if (withA) {
      a = aVal;
      haveA = true;
    }
  }
};

// Mach3 and LinuxCNC agree on the motion words this generator emits; they
// differ in the preamble, the tool change, and how a program ends. Keeping
// the difference to those three places is why the interface is this small.
// How a controller takes a tool change (docs/cam/post.md#dialects):
//   Table  -- T then M6, and the controller's tool table does the rest
//             (LinuxCNC).
//   Manual -- T M6, then M0 with a message: the operator changes the tool,
//             sets Z zero, presses cycle start (Mach3).
//   Pause  -- NO M6 at all: GRBL ignores it unless built for it, and the
//             one CAM surveyed that emits an inline "M6 T2" for GRBL runs
//             straight on into the next cut with the wrong tool. M5, the
//             message, M0 -- a feed hold every GRBL honours -- then the
//             spindle again.
enum class ToolChange { Table, Manual, Pause };

struct Dialect {
  const char* id;
  ToolChange change;
  const char* end; // program end word
  // G2/G3 with a Z word interpolates a helix (docs/cam/post.md#helical-arcs):
  // Mach3's helical interpolation, LinuxCNC's G2/G3 with the third axis
  // linear, GRBL 1.1's arcs with Z.
  bool helical;
};

constexpr Dialect kDialects[] = {
    {"mach3", ToolChange::Manual, "M30", true},
    {"linuxcnc", ToolChange::Table, "M2", true},
    // Every G word in the preamble is GRBL-legal (G91.1 included, since 1.1),
    // and the drill posts plain moves, so nothing needs translating -- the
    // difference is only the tool change and the end word.
    {"grbl", ToolChange::Pause, "M2", true},
};

// --- C7: fitting arcs back onto the polyline -------------------------------
//
// Offsetting flattens every curve, so a circular pocket leaves here as
// hundreds of G1s. The fit runs at POST time on purpose: the toolpath stays a
// polyline, so the backplot, the height-map verifier and every existing test
// see exactly what they saw before, and only this file and the G-code parser
// know arcs exist.
//
// An arc must fit the polyline FAR better than the polyline fits the true
// curve it came from, or the fit is inventing curvature rather than
// recovering it. A quarter of the chord tolerance the sectioner used.
constexpr double kChordTolerance = 0.02; // mm, matches CamToolpath
constexpr double kArcTolerance = kChordTolerance * 0.25;
// Three segments minimum: two segments define a circle through three points
// with no evidence left over to test it against.
constexpr size_t kMinArcSegments = 3;

struct Circle {
  glm::dvec2 centre{0.0};
  double radius = 0.0;
  bool ok = false;
};

// Least-squares circle (Kasa) through every point of the run, then the
// centre slid onto the perpendicular bisector of the first and last points.
//
// Both halves are load-bearing. A circle through three ADJACENT tessellation
// points is ill-conditioned — the first attempt fitted a 3 mm corner arc
// 0.014 mm off centre and broke each corner into two arcs. And a
// least-squares centre is not exactly equidistant from the endpoints, which
// is precisely what LinuxCNC rejects as an arc endpoint radius mismatch: the
// projection makes |start - centre| == |end - centre| by construction.
Circle FitCircle(const std::vector<glm::dvec2>& pts) {
  Circle out;
  if (pts.size() < 3)
    return out;
  double sx = 0, sy = 0, sxx = 0, syy = 0, sxy = 0, sxz = 0, syz = 0, sz = 0;
  const double n = static_cast<double>(pts.size());
  for (const glm::dvec2& p : pts) {
    const double zz = p.x * p.x + p.y * p.y;
    sx += p.x;
    sy += p.y;
    sxx += p.x * p.x;
    syy += p.y * p.y;
    sxy += p.x * p.y;
    sxz += p.x * zz;
    syz += p.y * zz;
    sz += zz;
  }
  // Normal equations for x^2 + y^2 + D x + E y + F = 0.
  const double m11 = sxx, m12 = sxy, m13 = sx;
  const double m22 = syy, m23 = sy;
  const double det = m11 * (m22 * n - m23 * m23) - m12 * (m12 * n - m23 * m13) +
                     m13 * (m12 * m23 - m22 * m13);
  if (std::abs(det) < 1e-12)
    return out; // collinear: a straight run declines to become an arc
  const double r1 = -sxz, r2 = -syz, r3 = -sz;
  const double d = (r1 * (m22 * n - m23 * m23) - m12 * (r2 * n - m23 * r3) +
                    m13 * (r2 * m23 - m22 * r3)) /
                   det;
  const double e = (m11 * (r2 * n - m23 * r3) - r1 * (m12 * n - m23 * m13) +
                    m13 * (m12 * r3 - r2 * m13)) /
                   det;
  glm::dvec2 centre(-d * 0.5, -e * 0.5);

  // Slide onto the bisector of the endpoints so the radii match exactly.
  const glm::dvec2& a = pts.front();
  const glm::dvec2& b = pts.back();
  const glm::dvec2 chord = b - a;
  const double len2 = glm::dot(chord, chord);
  if (len2 > 1e-18) {
    const glm::dvec2 mid = (a + b) * 0.5;
    const glm::dvec2 dir(-chord.y, chord.x);
    centre = mid + dir * (glm::dot(centre - mid, dir) / glm::dot(dir, dir));
  }
  out.centre = centre;
  out.radius = glm::length(a - centre);
  // A metre-radius "arc" is a straight line with rounding noise: emitting it
  // gains nothing and invites a controller's own arc tolerance to reject it.
  out.ok = out.radius > 1e-6 && out.radius < 1.0e3;
  return out;
}

// The last move of the straight run starting at `i`, or `i` itself when the
// next move turns. Everything between is dropped: the controller draws the
// same line from one G1 as it does from forty.
//
// Only reached when the arc fitter has DECLINED this run, so it can never eat
// an arc. That ordering is the whole safety argument — decimating first would
// remove exactly the points an arc is recognised from.
//
// The tolerance is the same chord tolerance the toolpath was flattened at, so
// this is not an approximation of an exact path; it is a shorter way of
// writing an approximation that was already made. Adaptive clearing hands
// back polylines with hundreds of nearly-collinear points, which is why a
// 40 x 20 pocket posted 29,856 lines with arcs turned off — the setting a
// controller that mishandles arcs is left with, and those are the slow ones.
size_t LastCollinear(const std::vector<Move>& moves, size_t i,
                     const glm::dvec3& from, double tol) {
  // Bounded: a run of thousands is merged in chunks rather than costing
  // O(n^2) over the whole thing.
  constexpr size_t kMaxRun = 256;
  const Move& first = moves[i];
  size_t best = i;
  // A move that TURNS the part is never merged forward: the run's end would
  // carry its A, so a turn followed by a straight feed became one move doing
  // both at once -- a different cut. Only a spiral makes the first of those,
  // which is why nothing met it before.
  if (i > 0 && first.hasA && moves[i - 1].hasA &&
      Emitter::Differs(first.a, moves[i - 1].a))
    return i;
  for (size_t j = i + 1; j < moves.size() && j - i < kMaxRun; j++) {
    const Move& m = moves[j];
    if (m.kind != first.kind || Emitter::Differs(m.feed, first.feed) ||
        m.hasA != first.hasA || (m.hasA && Emitter::Differs(m.a, first.a)))
      break;
    // Every point between `from` and this candidate end must lie on the line.
    const glm::dvec3 dir = m.to - from;
    const double len = glm::length(dir);
    if (len < 1e-12)
      break;
    bool straight = true;
    for (size_t k = i; k < j && straight; k++)
      straight = glm::length(glm::cross(moves[k].to - from, dir)) / len <= tol;
    if (!straight)
      break;
    best = j;
  }
  return best;
}

// Signed turn from `from` to `to` about `centre`, in (-pi, pi].
double SweptAngle(const glm::dvec2& centre, const glm::dvec2& from,
                  const glm::dvec2& to) {
  const glm::dvec2 u = from - centre, v = to - centre;
  return std::atan2(u.x * v.y - u.y * v.x, glm::dot(u, v));
}

// Longest run of moves from `first` that lies on one circle. Returns the
// index of the last move the arc absorbs, or `first - 1` when there is no
// arc — so the caller emits a line and steps on.
size_t FitArc(const std::vector<Move>& moves, size_t first, Circle& circle,
              bool& clockwise) {
  if (first == 0 || first + kMinArcSegments > moves.size())
    return first - 1;
  const glm::dvec3 start = moves[first - 1].to;
  const double z = start.z;
  const double feed = moves[first].feed;
  // The A the run starts at. An arc across a CHANGING A is what is unsafe --
  // `Emitter::Arc` writes no A word and does not touch the modal one, so a
  // fitted arc would swallow the rotation and desync the file from the
  // machine. A CONSTANT A is not that, and refusing it cost every indexed job
  // its arcs: a round part that posted ten G2s at A=0 posted zero the moment
  // the trunnion was turned, with the panel still showing "Arcs (G2/G3)"
  // ticked. `LastCollinear`, five lines up, already compared A rather than
  // requiring its absence.
  const bool hasA = moves[first].hasA;
  const double a = moves[first].a;
  // ...including against the move the arc STARTS from. If the run's A differs
  // from the one already emitted, the rotation happens on the arc's first
  // move and the arc would swallow it. A spiral's every cut changes A, so
  // this is what keeps a rotary helix out of the arc fitter.
  if (hasA != moves[first - 1].hasA ||
      (hasA && Emitter::Differs(a, moves[first - 1].a)))
    return first - 1;

  const auto usable = [&](const Move& m) {
    return m.kind == Move::Kind::Cut && m.hasA == hasA &&
           (!hasA || !Emitter::Differs(m.a, a)) &&
           std::abs(m.to.z - z) < 1e-9 && std::abs(m.feed - feed) < 1e-9;
  };
  for (size_t k = first; k < first + kMinArcSegments; k++)
    if (k >= moves.size() || !usable(moves[k]))
      return first - 1;

  // Grow the run one move at a time, refitting the whole thing each step: a
  // fit is only trusted against the points it was made from.
  std::vector<glm::dvec2> pts{glm::dvec2(start)};
  size_t last = first - 1;
  for (size_t k = first; k < moves.size(); k++) {
    if (!usable(moves[k]))
      break;
    pts.push_back(glm::dvec2(moves[k].to));
    if (pts.size() < kMinArcSegments + 1)
      continue;

    const Circle candidate = FitCircle(pts);
    if (!candidate.ok)
      break;
    bool fits = true;
    double swept = 0.0;
    bool cw = false;
    for (size_t i = 1; i < pts.size() && fits; i++) {
      if (std::abs(glm::length(pts[i] - candidate.centre) - candidate.radius) >
          kArcTolerance) {
        fits = false;
        break;
      }
      const double step = SweptAngle(candidate.centre, pts[i - 1], pts[i]);
      // A reversal, a stall, or a jump past a quarter turn between samples
      // is a different curve, not this one.
      if (std::abs(step) < 1e-12 || std::abs(step) > std::numbers::pi * 0.5) {
        fits = false;
        break;
      }
      // How far the arc bows away from the straight segment it replaces,
      // BETWEEN the samples. Checking only at the samples let a 1.5 mm
      // straight edge be swallowed into a 10 mm-radius arc that passed
      // within 5 um at every point and bulged 27 um in the middle — a curve
      // cut where the part wants a flat. The bound is the chord tolerance
      // the polyline itself was built to, because that is how far from the
      // true curve these points already are.
      if (candidate.radius * (1.0 - std::cos(std::abs(step) * 0.5)) >
          kChordTolerance) {
        fits = false;
        break;
      }
      if (i == 1)
        cw = step < 0.0;
      else if ((step < 0.0) != cw)
        fits = false;
      swept += step;
    }
    // Stop short of a full turn: a G2 whose start and end coincide is
    // ambiguous, and controllers disagree about it.
    if (fits && std::abs(swept) > std::numbers::pi * 1.9)
      fits = false;
    if (!fits) {
      pts.pop_back();
      break;
    }
    circle = candidate;
    clockwise = cw;
    last = k;
  }
  return last - first + 1 >= kMinArcSegments ? last : first - 1;
}

// Is the run a generator declared as ONE arc (Toolpath::Arc) still one? A
// clip, a holding cut or a merge may have moved the moves since it was
// declared, so the declaration is a claim this checks before writing it as a
// G2/G3: every chord on the circle, turning one way, Z linear in the angle,
// one feed, one A, and short of a full turn (a G2 whose ends coincide is
// ambiguous). A run that fails is written as its chords -- the same cut.
bool ExactArc(const std::vector<Move>& moves, const Toolpath::Arc& arc) {
  if (arc.first == 0 || arc.last < arc.first || arc.last >= moves.size())
    return false;
  constexpr double kOn = 1e-4; // mm: the generator puts every chord ON it
  const Move& before = moves[arc.first - 1];
  const glm::dvec3 start = before.to;
  const double radius = glm::length(glm::dvec2(start) - arc.centre);
  if (radius < 1e-6)
    return false;
  const double feed = moves[arc.first].feed;
  std::vector<double> swept;
  swept.reserve(arc.last - arc.first + 1);
  double total = 0.0;
  glm::dvec2 at(start);
  for (size_t k = arc.first; k <= arc.last; k++) {
    const Move& m = moves[k];
    if (m.kind != Move::Kind::Cut || std::abs(m.feed - feed) > 1e-9 ||
        m.hasA != before.hasA || (m.hasA && Emitter::Differs(m.a, before.a)))
      return false;
    if (std::abs(glm::length(glm::dvec2(m.to) - arc.centre) - radius) > kOn)
      return false;
    const double step = SweptAngle(arc.centre, at, glm::dvec2(m.to));
    if (std::abs(step) < 1e-12 || (step < 0.0) != arc.clockwise)
      return false;
    total += step;
    swept.push_back(total);
    at = glm::dvec2(m.to);
  }
  if (std::abs(total) >= 2.0 * std::numbers::pi - 1e-6)
    return false;
  const double rise = moves[arc.last].to.z - start.z;
  for (size_t k = arc.first; k <= arc.last; k++)
    if (std::abs(moves[k].to.z - start.z -
                 rise * swept[k - arc.first] / total) > kOn)
      return false;
  return true;
}

const Dialect* Find(const std::string& id) {
  for (const Dialect& d : kDialects)
    if (id == d.id)
      return &d;
  return nullptr;
}

} // namespace

std::vector<std::string> PostIds() {
  std::vector<std::string> ids;
  for (const Dialect& d : kDialects)
    ids.emplace_back(d.id);
  return ids;
}

bool PostExists(const std::string& id) { return Find(id) != nullptr; }

bool PostWritesHelicalArcs(const std::string& id) {
  const Dialect* d = Find(id);
  return d != nullptr && d->helical;
}

bool PostProcess(const std::string& postId, const Setup& setup,
                 const std::vector<Toolpath>& paths, const PostOptions& opts,
                 std::string& out, std::string* error) {
  const Dialect* dialect = Find(postId);
  if (dialect == nullptr) {
    if (error != nullptr) {
      std::string known;
      for (const Dialect& d : kDialects)
        known += (known.empty() ? "" : ", ") + std::string(d.id);
      *error = "unknown post '" + postId + "'; this build has: " + known;
    }
    return false;
  }

  // Held in the rotary of a machine that does not say how it is installed:
  // refused, naming what to state
  // (docs/cam/rotary.md#how-the-rotary-is-installed).
  if (const std::string why = MountRefusal(setup); !why.empty()) {
    if (error != nullptr)
      *error = why;
    return false;
  }
  Emitter e;
  e.decimals = opts.decimals;
  e.origin = setup.origin;
  e.mount = MountOf(setup);
  if (opts.comments) {
    e.Comment("Designer CAM - " + setup.name);
    e.Comment("post: " + std::string(dialect->id));
    // Every toleranced wall the program aims at, and where in its band
    // (docs/cam/post.md#what-the-file-says): the machinist gauges against
    // the band, and re-posts with the cutter's measured size if it is off.
    std::vector<const Toolpath::Fit*> said;
    for (const Toolpath& tp : paths)
      for (const Toolpath::Fit& f : tp.fits) {
        bool again = false;
        for (const Toolpath::Fit* s : said)
          again = again ||
                  (s->op == f.op && glm::distance(s->centre, f.centre) < 1e-3 &&
                   std::abs(s->nominal - f.nominal) < 1e-6);
        if (again)
          continue;
        said.push_back(&f);
        e.Comment("fit op " + std::to_string(f.op) + ": " +
                  (f.internal ? "bore D" : "boss D") + Num(f.nominal, 3) + " " +
                  f.label + " " + Num(f.Min(), 4) + "-" + Num(f.Max(), 4) +
                  ", aimed at " + Num(f.aim, 4));
      }
  }
  // Absolute, mm, XY plane, cancel any leftover cutter comp or canned cycle:
  // a preamble that assumes the machine is in a known state is how a job
  // starts in inches.
  // G91.1: arc centres are INCREMENTAL from the arc start. LinuxCNC defaults
  // that way and Mach3 is configurable, so a file that does not say puts the
  // centre wherever the machine was left — see Emitter::Arc.
  e.Raw(opts.fitArcs ? "G90 G91.1 G21 G17 G40 G49 G80"
                     : "G90 G21 G17 G40 G49 G80");
  if (opts.useG54)
    e.Raw("G54");

  int currentTool = -1;
  double currentRpm = -1.0;
  const RotaryFeedModel feedModel = FeedModelOf(dialect->id);
  const AxisLimits axisLimits = AxisLimitsOf(setup);
  for (size_t pathIndex = 0; pathIndex < paths.size(); pathIndex++) {
    const Toolpath& tp = paths[pathIndex];
    if (!tp.error.empty()) {
      // Skipped, but SAID: an operation that vanishes silently is how a part
      // comes off the machine missing a feature nobody noticed.
      if (opts.comments)
        e.Comment("SKIPPED op " + std::to_string(tp.operationId) + ": " +
                  tp.error);
      continue;
    }
    // A target this operation could not follow, while others could: the file
    // is short of a hole and nothing else would say so.
    if (opts.comments)
      for (const std::string& why : tp.skipped)
        e.Comment("SKIPPED on op " + std::to_string(tp.operationId) + ": " +
                  why);
    if (tp.Empty())
      continue;

    if (tp.toolNumber != currentTool) {
      const Tool* tool = setup.FindTool(tp.toolNumber);
      // Retract before any tool change, always: the one move that is never
      // wrong and whose absence breaks things.
      e.Retract(setup.safeZ);
      if (dialect->change != ToolChange::Pause) {
        std::ostringstream t;
        t << "T" << tp.toolNumber << " M6";
        e.Raw(t.str());
      } else {
        // The T word WITHOUT M6: GRBL parses it, stores it and moves nothing,
        // which is the whole point -- M6 is what it ignores unless built for
        // a changer, and an inline "M6 T2" runs straight on into the next cut
        // with the wrong tool. Stating it costs nothing at the machine and is
        // what makes the FILE say which cutter each move belongs to: without
        // it every reader of a grbl program -- this project's own verifier
        // included -- has to assume tool 1, and a job whose second tool is
        // fatter than its first verifies clean while gouging (found on a
        // 2 mm cutter's slots simulated with a 3.175 one).
        std::ostringstream sel;
        sel << "T" << tp.toolNumber;
        e.Raw(sel.str());
        // The spindle off before the hold, and the change spelled out where
        // the operator will read it: the T word is for the file's readers,
        // the message is for the person at the machine.
        e.Raw("M5");
        std::ostringstream t;
        // Parenthesised, so the tool's NAME has to be safe here too -- this
        // one is built by hand rather than through Comment().
        t << "change to T" << tp.toolNumber;
        if (tool != nullptr)
          t << ": " << tool->name << " D" << Num(tool->diameter, 3);
        t << " - set Z zero, then cycle start";
        e.Comment(t.str());
      }
      if (opts.comments && tool != nullptr) {
        // The SHAPE goes in the comment too. "T2 D6" is the same line for a
        // 6 mm end mill and a 6 mm ball nose, and the operator at the machine
        // is the one who has to tell them apart before pressing cycle start.
        std::string shape;
        switch (tool->type) {
        case ToolType::Ball:
          shape = " ball";
          break;
        case ToolType::VBit:
          shape = " V " + Num(tool->tipAngleDeg, 1) + " deg";
          break;
        case ToolType::Drill:
          shape = " drill " + Num(tool->tipAngleDeg, 1) + " deg";
          break;
        default:
          break;
        }
        e.Comment("tool " + std::to_string(tool->number) + ": " + tool->name +
                  " D" + Num(tool->diameter, 3) + shape);
      }
      const bool paused =
          dialect->change == ToolChange::Pause ||
          (dialect->change == ToolChange::Manual && opts.pauseOnToolChange);
      if (paused)
        e.Raw(dialect->change == ToolChange::Pause
                  ? "M0"
                  : "M0 (change tool, set Z zero, then cycle start)");
      currentTool = tp.toolNumber;
      const double rpm = tool != nullptr ? tool->spindleRpm : 0.0;
      // Restate the spindle after a PAUSE even when the speed has not
      // changed. A manual tool change is done with the spindle stopped, and
      // the line after M0 was a cutting move: two tools at the same rpm
      // produced a file that never turned the spindle back on. Found by
      // reading the coupon's file (docs/cam/post.md#what-the-file-says).
      if (rpm > 0.0 &&
          (paused || currentRpm < 0.0 || std::abs(rpm - currentRpm) > 1e-9)) {
        std::ostringstream s;
        s << "S" << Num(rpm, 0) << " M3";
        e.Raw(s.str());
        currentRpm = rpm;
      }
    }

    if (opts.comments)
      e.Comment("op " + std::to_string(tp.operationId));
    // The index this setup is cut at, stated before the first move of it and
    // at safe Z. A file that assumes the table is already at A0 is a file
    // that cuts the wrong face.
    if (EmitsRotary(setup) && !e.haveA) {
      e.Retract(setup.safeZ);
      e.Raw("G0 " + e.AWord(setup.rotationA, opts.decimals));
      e.haveA = true;
      e.a = setup.rotationA;
    }
    // Cuts that turn, whose F was scaled for the controller and hit the
    // machine's cap. Counted and said ONCE for the operation: a feed nobody
    // chose is worth a line, and not one per move.
    size_t capped = 0;
    double cappedWanted = 0.0, cappedRadius = 1e18;
    // A path that DECLARES its arcs (a thread mill's helix) is written from
    // them and not fitted: the fitter knows only flat arcs, and would take a
    // flat lead-in and the helix's first chords for one circle. Everything
    // else in such a path is a straight move.
    const bool declared = !tp.arcs.empty();
    size_t nextArc = 0;
    for (size_t i = 0; i < tp.moves.size();) {
      const Move& m = tp.moves[i];
      if (declared) {
        while (nextArc < tp.arcs.size() && tp.arcs[nextArc].first < i)
          nextArc++;
        if (opts.fitArcs && nextArc < tp.arcs.size() &&
            tp.arcs[nextArc].first == i &&
            ExactArc(tp.moves, tp.arcs[nextArc])) {
          const Toolpath::Arc& arc = tp.arcs[nextArc];
          e.Arc(arc.clockwise, tp.moves[arc.last].to, arc.centre,
                tp.moves[i].feed);
          i = arc.last + 1;
          nextArc++;
          continue;
        }
      }
      Circle circle;
      bool clockwise = false;
      const size_t last = opts.fitArcs && !declared
                              ? FitArc(tp.moves, i, circle, clockwise)
                              : i - 1;
      if (opts.fitArcs && !declared && i > 0 && last >= i) {
        e.Arc(clockwise, tp.moves[last].to, circle.centre, tp.moves[i].feed);
        i = last + 1;
        continue;
      }
      // C9: a rotary INDEX never happens with the tool down. The toolpath
      // is supposed to arrive already retracted, but "supposed to" is how a
      // cutter ends up sweeping an arc through the part while the trunnion
      // turns — so the post enforces it rather than trusting its caller.
      //
      // An index is a RAPID that turns A. A feed move that turns it is the
      // other thing entirely -- continuous 4-axis, the tool down by design
      // (a spiral) -- and retracting there would lift the cutter out of
      // every step of the cut.
      if (m.kind == Move::Kind::Rapid && m.hasA && e.haveA &&
          Emitter::Differs(m.a, e.a))
        e.Retract(setup.safeZ);
      const char* code = m.kind == Move::Kind::Rapid ? "G0" : "G1";
      // A straight run collapses to its endpoint. Same line, fewer of them.
      size_t straightTo =
          i > 0 ? LastCollinear(tp.moves, i, tp.moves[i - 1].to, kArcTolerance)
                : i;
      // …but never past the start of an arc. The first points of an arc that
      // leaves tangentially ARE collinear with the line feeding into it, to
      // well inside this tolerance, so an unchecked run swallowed them and
      // the fitter — which is only ever asked at the run's far end — lost the
      // arc: 3633 arcs became 3317. Ask at every index the run covers, which
      // is exactly what this loop did before the merge existed.
      // ...nor into a declared arc, which is written whole from its first.
      if (declared && nextArc < tp.arcs.size() && tp.arcs[nextArc].first > i &&
          straightTo >= tp.arcs[nextArc].first)
        straightTo = tp.arcs[nextArc].first - 1;
      if (opts.fitArcs && !declared)
        for (size_t k = i + 1; k <= straightTo; k++) {
          Circle ahead;
          bool aheadCw = false;
          if (FitArc(tp.moves, k, ahead, aheadCw) >= k) {
            straightTo = k - 1;
            break;
          }
        }
      const Move& end = tp.moves[straightTo];
      double feed = end.kind == Move::Kind::Rapid ? 0.0 : end.feed;
      // A cut that turns: F scaled so the controller's reading of it moves
      // the TIP at the feed asked for (CamRotaryFeed.h). A merged run never
      // turns -- LastCollinear stops at a change of A -- so the move before
      // is where this one starts.
      if (feed > 0.0 && straightTo > 0) {
        const RotarySegment seg = SegmentOf(tp.moves[straightTo - 1], end);
        if (TurnsWhileCutting(seg)) {
          const ScaledFeed f = PlanFeed(seg, 0.0, feedModel, axisLimits).feed;
          // A tenth is finer than any controller's feed override, and a
          // file that writes F to four places on every line of a spiral is
          // a third longer for nothing.
          feed = std::max(0.1, std::round(f.feed * 10.0) / 10.0);
          // Keep the F already in force when this one is within 1% of it: a
          // scaled F drifts by a tenth from line to line, and restating it
          // on every line made a spiral 35 MB, mostly F words. A move held
          // to an axis limit only keeps it when that F is under the limit.
          if (!std::isnan(e.feed) && e.feed > 0.0 &&
              std::abs(e.feed - feed) <= 0.01 * feed &&
              (!f.clamped || e.feed <= f.feed))
            feed = e.feed;
          if (f.clamped) {
            capped++;
            cappedWanted = std::max(cappedWanted, f.wanted);
            cappedRadius = std::min(cappedRadius,
                                    std::min(std::hypot(seg.from.y, seg.from.z),
                                             std::hypot(seg.to.y, seg.to.z)));
          }
        }
      }
      e.Motion(code, end.to, feed, end.hasA, end.a);
      i = straightTo + 1;
    }
    if (capped > 0 && opts.comments)
      e.Comment("op " + std::to_string(tp.operationId) + ": " +
                std::to_string(capped) +
                " turning moves held to the machine's axis speeds - down to "
                "radius " +
                Num(cappedRadius, 1) + " mm they needed up to F" +
                Num(cappedWanted, 0) + ", so the tip runs slower there");

    // A spiral winds the axis round hundreds of turns: the rolling pin at a
    // half-millimetre pitch ends near A-216000. The next operation's first
    // rapid would ask for all of them back. `G92 A` renames where the axis
    // IS without moving it, and a whole number of turns names the same
    // physical angle -- so the file carries on from the nearest equivalent
    // of the next angle it needs, and the part never unwinds. Mach3 and
    // LinuxCNC both honour it; the parser reads it (CamGcode.cpp), so the
    // verifier and the simulator stay on the same angle as the machine.
    // Modulo-360 output would be the other answer, and it is wrong for a
    // cut: a feed move from 359 to 1 turns BACK 358 degrees, tool down, on
    // any controller that does not take the short way. Renamed whenever the
    // next angle is half a turn or more away, not only after an op that
    // wound: a spiral STARTS wound -- its angle counts from the axis's end --
    // and the finish before it left the axis near zero.
    if (e.haveA) {
      double next = setup.rotationA;
      bool found = false;
      for (size_t n = pathIndex + 1; n < paths.size() && !found; n++) {
        if (!paths[n].error.empty())
          continue;
        for (const Move& mv : paths[n].moves)
          if (mv.hasA) {
            next = mv.a;
            found = true;
            break;
          }
      }
      const double renamed = e.a - 360.0 * std::round((e.a - next) / 360.0);
      if (Emitter::Differs(renamed, e.a)) {
        e.Raw("G92 " + e.AWord(renamed, opts.decimals));
        e.a = renamed;
      }
    }
  }

  // Retract, spindle off, done. In that order.
  e.Retract(setup.safeZ);
  e.Raw("M5");
  e.Raw(dialect->end);
  out = e.out.str();
  return true;
}

bool PostProcessSplit(const std::string& postId, const Setup& setup,
                      const std::vector<Toolpath>& paths,
                      const PostOptions& opts, std::vector<PostFile>& out,
                      std::string* error) {
  out.clear();
  std::vector<std::vector<Toolpath>> groups;
  std::vector<int> tools;
  for (const Toolpath& tp : paths) {
    if (!tp.error.empty() || tp.Empty())
      continue; // the single-file post skips these too, with a comment
    if (tools.empty() || tools.back() != tp.toolNumber) {
      for (const int seen : tools)
        if (seen == tp.toolNumber) {
          if (error != nullptr)
            *error = "tool " + std::to_string(tp.toolNumber) +
                     " is used twice with another tool between: a file per "
                     "tool needs the operations in tool order";
          return false;
        }
      tools.push_back(tp.toolNumber);
      groups.emplace_back();
    }
    groups.back().push_back(tp);
  }
  for (size_t g = 0; g < groups.size(); g++) {
    PostFile file;
    file.tool = tools[g];
    if (!PostProcess(postId, setup, groups[g], opts, file.gcode, error))
      return false;
    out.push_back(std::move(file));
  }
  return true;
}

} // namespace cam
