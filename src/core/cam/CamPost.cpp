#include "core/cam/CamPost.h"

#include <cmath>
#include <cstdio>
#include <sstream>

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
  // Modal state: G-code repeats nothing it does not have to, and a file that
  // restates X on every line is unreadable when something goes wrong.
  double x = std::nan("");
  double y = std::nan("");
  double z = std::nan("");
  double feed = std::nan("");
  bool haveA = false;
  double a = std::nan("");

  void Comment(const std::string& text) { out << "(" << text << ")\n"; }
  void Raw(const std::string& text) { out << text << "\n"; }

  static bool Differs(double now, double before) {
    return std::isnan(before) || std::abs(now - before) > 1e-9;
  }

  void Motion(const char* code, const glm::dvec3& to, double f, bool withA,
              double aVal) {
    std::ostringstream l;
    l << code;
    if (Differs(to.x, x))
      l << " X" << Num(to.x, decimals);
    if (Differs(to.y, y))
      l << " Y" << Num(to.y, decimals);
    if (Differs(to.z, z))
      l << " Z" << Num(to.z, decimals);
    if (withA && Differs(aVal, a))
      l << " A" << Num(aVal, decimals);
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
struct Dialect {
  const char* id;
  bool toolTable;  // LinuxCNC: T then M6 uses the table; Mach3: manual
  const char* end; // program end word
};

constexpr Dialect kDialects[] = {
    {"mach3", false, "M30"},
    {"linuxcnc", true, "M2"},
};

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

  Emitter e;
  e.decimals = opts.decimals;
  if (opts.comments) {
    e.Comment("Designer CAM - " + setup.name);
    e.Comment("post: " + std::string(dialect->id));
  }
  // Absolute, mm, XY plane, cancel any leftover cutter comp or canned cycle:
  // a preamble that assumes the machine is in a known state is how a job
  // starts in inches.
  e.Raw("G90 G21 G17 G40 G49 G80");
  if (opts.useG54)
    e.Raw("G54");

  int currentTool = -1;
  double currentRpm = -1.0;
  for (const Toolpath& tp : paths) {
    if (!tp.error.empty()) {
      // Skipped, but SAID: an operation that vanishes silently is how a part
      // comes off the machine missing a feature nobody noticed.
      if (opts.comments)
        e.Comment("SKIPPED op " + std::to_string(tp.operationId) + ": " +
                  tp.error);
      continue;
    }
    if (tp.Empty())
      continue;

    if (tp.toolNumber != currentTool) {
      const Tool* tool = setup.FindTool(tp.toolNumber);
      // Retract before any tool change, always: the one move that is never
      // wrong and whose absence breaks things.
      e.Motion("G0", {e.x, e.y, setup.safeZ}, 0.0, false, 0.0);
      std::ostringstream t;
      t << "T" << tp.toolNumber << " M6";
      e.Raw(t.str());
      if (opts.comments && tool != nullptr)
        e.Comment("tool " + std::to_string(tool->number) + ": " + tool->name +
                  " D" + Num(tool->diameter, 3));
      if (!dialect->toolTable && opts.pauseOnToolChange)
        e.Raw("M0 (change tool, set Z zero, then cycle start)");
      currentTool = tp.toolNumber;
      const double rpm = tool != nullptr ? tool->spindleRpm : 0.0;
      if (rpm > 0.0 &&
          (currentRpm < 0.0 || std::abs(rpm - currentRpm) > 1e-9)) {
        std::ostringstream s;
        s << "S" << Num(rpm, 0) << " M3";
        e.Raw(s.str());
        currentRpm = rpm;
      }
    }

    if (opts.comments)
      e.Comment("op " + std::to_string(tp.operationId));
    for (const Move& m : tp.moves) {
      const char* code = m.kind == Move::Kind::Rapid ? "G0" : "G1";
      e.Motion(code, m.to, m.kind == Move::Kind::Rapid ? 0.0 : m.feed, m.hasA,
               m.a);
    }
  }

  // Retract, spindle off, done. In that order.
  e.Motion("G0", {e.x, e.y, setup.safeZ}, 0.0, false, 0.0);
  e.Raw("M5");
  e.Raw(dialect->end);
  out = e.out.str();
  return true;
}

} // namespace cam
