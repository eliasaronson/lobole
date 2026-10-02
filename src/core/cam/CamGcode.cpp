#include "core/cam/CamGcode.h"

#include "core/cam/CamMount.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <numbers>

namespace cam {

namespace {

// How finely an arc is re-expanded into chords for simulation: the same
// tolerance the toolpath was flattened at, so a fitted arc returns to the
// polyline it came from.
constexpr double kArcChordTolerance = 0.02;

} // namespace

GProgram ParseGcode(std::string_view text, char rotaryWord) {
  GProgram out;
  glm::dvec3 pos(0.0);
  bool rapid = true;
  // 0 = G0, 1 = G1, 2 = G2 (clockwise arc), 3 = G3. Modal, like everything
  // else here: a block with only coordinates repeats the last motion.
  int mode = 0;
  int tool = 1;
  int lineNo = 0;
  bool havePos = false;
  double feed = 0.0;
  uint64_t op = 0;
  // Set by an M6 or by a T that differs from the tool in force, and consumed
  // by the next move emitted — a tool change is a line of its own, so the
  // boundary belongs to the first move that cuts with the new tool.
  bool toolChange = false;
  // `a` is the angle the axis is AT, continuous through every revolution.
  // A `G92 A` renames the current angle without moving anything, so the words
  // after it are read through `aOffset` -- and the moves keep the continuous
  // angle, which is what lets a program that winds a spiral and then
  // re-zeroes play without the part spinning back six hundred turns on the
  // screen and in the clock.
  double a = 0.0;
  double aOffset = 0.0;
  bool haveA = false;

  size_t i = 0;
  while (i <= text.size()) {
    const size_t end = std::min(text.find('\n', i), text.size());
    std::string_view line = text.substr(i, end - i);
    i = end + 1;
    lineNo++;

    // Strip parenthesised comments and anything after a semicolon — but KEEP
    // the comment text, because the post writes the operation boundary into
    // it (`(op 7)`) and that is the only place the file says which operation
    // a move belongs to.
    std::string clean, comment;
    clean.reserve(line.size());
    int depth = 0;
    for (const char c : line) {
      if (c == '(') {
        // A controller does not nest these. Ours survives it and used to say
        // nothing, so a file Mach3 refuses to load ("Nested comment found")
        // passed verification.
        if (depth > 0)
          out.warnings.push_back(
              "line " + std::to_string(lineNo) +
              ": nested comment -- a second '(' before the first is closed. "
              "Mach3 refuses the whole file for this");
        depth++;
      } else if (c == ')')
        depth = std::max(0, depth - 1);
      else if (c == ';')
        break;
      else if (depth == 0)
        clean.push_back(static_cast<char>(std::toupper(c)));
      else
        comment.push_back(c);
    }
    // Exactly `op <digits>`, so the post's own "(SKIPPED op 7: ...)" — which
    // means the opposite — cannot be read as the start of that operation.
    if (comment.rfind("op ", 0) == 0) {
      const std::string digits = comment.substr(3);
      if (!digits.empty() &&
          std::all_of(digits.begin(), digits.end(),
                      [](unsigned char c) { return std::isdigit(c) != 0; })) {
        try {
          op = std::stoull(digits);
        } catch (...) {
        }
      }
    }
    if (clean.empty())
      continue;

    glm::dvec3 to = pos;
    bool motion = false;
    bool sawAxis = false;
    bool g92 = false;
    bool lineHasA = false;
    double lineA = 0.0;
    // Arc centre, INCREMENTAL from the arc start (the post states G91.1).
    glm::dvec2 ij(0.0);
    for (size_t k = 0; k < clean.size(); k++) {
      char word = clean[k];
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
      } else if (word == 'X' || word == 'Y' || word == 'Z' || word == 'A' ||
                 word == 'B') {
        // "Xnan" lands here: the axis word exists with no parseable number.
        out.errors.push_back("line " + std::to_string(lineNo) +
                             ": axis word with no number");
      }
      // The rotary's own letter is read below as the rotary; the OTHER
      // rotary letter is said and not read, because reading it would turn a
      // part about an axis this machine does not have that way.
      if ((word == 'A' || word == 'B') && word != rotaryWord) {
        out.warnings.push_back("line " + std::to_string(lineNo) + ": a " +
                               std::string(1, word) +
                               " word, and this machine's rotary is " +
                               std::string(1, rotaryWord) + ": not read");
        k = numEnd - 1;
        continue;
      }
      if (word == rotaryWord)
        word = 'A'; // the rotary, whichever letter drives it
      switch (word) {
      case 'G':
        if (value == 0.0 || value == 1.0 || value == 2.0 || value == 3.0) {
          mode = static_cast<int>(value);
          rapid = mode == 0;
          motion = true;
        } else if (value == 41.0 || value == 42.0) {
          // Cutter compensation on (docs/cam/post.md#cutter-compensation).
          // The file is the tool centre and the controller adds only the
          // wear, so what is read here is the NOMINAL cut -- the program as
          // it runs with no wear entered -- and the op is said.
          if (std::find(out.compensatedOps.begin(), out.compensatedOps.end(),
                        op) == out.compensatedOps.end())
            out.compensatedOps.push_back(op);
        } else if (value == 92.0) {
          // Set the named axes' CURRENT position to the values given, moving
          // nothing. The post writes it for A alone, after a spiral has
          // wound the axis round many turns (CamPost.cpp).
          g92 = true;
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
        if (static_cast<int>(value) != tool)
          toolChange = true;
        tool = static_cast<int>(value);
        break;
      case 'F':
        // Modal, and only ever positive. A negative or zero F is a file that
        // would stall the spindle in the cut; it is reported, not adopted.
        if (value > 1e-9)
          feed = value;
        else
          out.errors.push_back("line " + std::to_string(lineNo) + ": feed of " +
                               num + " is not a feed");
        break;
      case 'M':
        if (static_cast<int>(value) == 6)
          toolChange = true;
        break;
      case 'A':
        // Modal, and RECORDED. It used to fall into `default:` and vanish,
        // so a file whose part turns was simulated as though it stood still.
        // What to do about that is decided where the stock model is chosen;
        // here it is simply read, like every other word.
        // Applied after the whole line is read: whether it is a move or a
        // rename depends on a G92 that may stand anywhere on the line.
        lineA = value;
        lineHasA = true;
        break;
      default:
        break;
      }
      k = numEnd - 1;
    }

    if (g92) {
      if (sawAxis)
        out.errors.push_back(
            "line " + std::to_string(lineNo) +
            ": G92 on a linear axis moves the work zero, which this reader "
            "does not model -- the post never writes one");
      if (lineHasA) {
        if (!haveA)
          a = lineA; // nothing stated yet: the axis is where it is called
        aOffset = a - lineA;
        haveA = true;
      }
      continue;
    }
    if (lineHasA) {
      a = lineA + aOffset;
      haveA = true;
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
            // `ang`, not `a`: the modal A WORD is called `a` in this
            // function, and an arc's own angle shadowing it wrote the arc
            // parameter into every move's rotary axis. An indexed job then
            // read as one whose trunnion swept from -pi to pi, which routed
            // its verification to the rotary stock model and reported 36 mm
            // of gouge on a correct program. Caught by the indexed-frame
            // test in one run.
            const double ang = a0 + sweep * t;
            GMove m;
            m.rapid = false;
            m.to = {centre.x + radius * std::cos(ang),
                    centre.y + radius * std::sin(ang),
                    pos.z + (to.z - pos.z) * t};
            m.tool = tool;
            m.line = lineNo;
            m.feed = feed;
            m.op = op;
            // The boundary is the first chord, not every chord of the arc.
            m.toolChange = toolChange;
            toolChange = false;
            m.hasA = haveA;
            m.a = a;
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
      m.feed = feed;
      m.op = op;
      m.toolChange = toolChange;
      toolChange = false;
      m.hasA = haveA;
      m.a = a;
      out.moves.push_back(m);
      pos = to;
      havePos = true;
    }
  }
  return out;
}

GProgram ProgramInSetupSpace(std::string_view text, const Setup& setup) {
  const Mount mount = MountOf(setup);
  GProgram p = ParseGcode(text, mount.word);
  for (GMove& m : p.moves) {
    m.to = ToSetup(mount, m.to) + setup.origin;
    m.a *= mount.sign;
  }
  return p;
}

bool RotatesWhileCutting(const GProgram& p) {
  bool have = false;
  double first = 0.0;
  for (const GMove& m : p.moves) {
    if (!m.hasA)
      continue;
    if (!have) {
      first = m.a;
      have = true;
    } else if (std::abs(m.a - first) > 1e-6) {
      return true;
    }
  }
  return false;
}

} // namespace cam
