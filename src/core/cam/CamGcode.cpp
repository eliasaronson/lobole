#include "core/cam/CamGcode.h"

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
  double feed = 0.0;
  uint64_t op = 0;
  // Set by an M6 or by a T that differs from the tool in force, and consumed
  // by the next move emitted — a tool change is a line of its own, so the
  // boundary belongs to the first move that cuts with the new tool.
  bool toolChange = false;
  // The rotary word, remembered only to tell an INDEX from a ROTATION: the
  // first A seen, and whether we have already said that it moves.
  double firstA = 0.0;
  bool haveA = false;
  bool saidRotary = false;

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
      } else if (word == 'X' || word == 'Y' || word == 'Z' || word == 'A') {
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
        // The rotary word. A CONSTANT A is the indexed case and needs nothing
        // done: the part is already framed and the file's coordinates are in
        // setup space, so the whole 3-axis check applies unchanged. That is
        // every rotary file this app posts today.
        //
        // A CHANGING A is not that, and it used to fall into `default:` and
        // vanish. A continuous 4-axis file -- hand-edited, or from another
        // CAM -- then verified as though the part stood still, which is a
        // clean verdict on a program nothing has checked. The height map
        // cannot represent a part that turns (docs/cam/simulation.md), so the
        // honest answer is to say so rather than to answer the wrong
        // question convincingly.
        if (!haveA) {
          firstA = value;
          haveA = true;
        } else if (std::abs(value - firstA) > 1e-6 && !saidRotary) {
          saidRotary = true;
          out.errors.push_back(
              "line " + std::to_string(lineNo) +
              ": the A axis MOVES in this file, and this verifier is 3-axis "
              "-- its stock is a height map, which cannot represent a part "
              "that turns. Nothing below has been checked against the "
              "rotation");
        }
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
            m.feed = feed;
            m.op = op;
            // The boundary is the first chord, not every chord of the arc.
            m.toolChange = toolChange;
            toolChange = false;
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
      out.moves.push_back(m);
      pos = to;
      havePos = true;
    }
  }
  return out;
}

GProgram ProgramInSetupSpace(std::string_view text, const Setup& setup) {
  GProgram p = ParseGcode(text);
  if (setup.origin == glm::dvec3(0.0))
    return p;
  for (GMove& m : p.moves)
    m.to += setup.origin;
  return p;
}

} // namespace cam
