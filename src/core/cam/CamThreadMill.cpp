// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/cam/CamThreadMill.h"

#include "core/ThreadStandards.h"
#include "core/cam/CamPost.h"
#include "core/cam/CamToolpath.h"
#include "core/cam/CamToolpathInternal.h"

#include <BRepAdaptor_Surface.hxx>
#include <BRepClass3d_SolidClassifier.hxx>
#include <Standard_Failure.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <gp_Cylinder.hxx>
#include <gp_Pnt.hxx>

#include <glm/geometric.hpp>
#include <glm/trigonometric.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <numbers>

// docs/cam/operations.md#thread-mill. The numbers are FreeCAD's
// (CamThreadMill.h says which function each follows); what drives them is
// the Thread on the face.
namespace cam {

namespace {

using detail::Emit;
using detail::kDeflection;

constexpr double kPi = std::numbers::pi;

// flat / (2 tan(angle / 2)): how far a V's sharp point stands past a flat of
// width `flat` -- 0.866 flat at 60 degrees. FreeCAD's `crestFactor`.
double CrestFactor(double angleDeg) {
  return 1.0 / (2.0 * std::tan(glm::radians(angleDeg * 0.5)));
}

std::string Mm(double v, int places = 3) {
  char buf[48];
  std::snprintf(buf, sizeof buf, "%.*f", places, v);
  return buf;
}

std::string ToolName(const Tool& t) {
  return "tool " + std::to_string(t.number) + " (" + Mm(t.diameter, 2) + " mm)";
}

// The face at `idx` of `body`, or a null face.
TopoDS_Face FaceAt(const Body& body, int idx) {
  int i = 0;
  for (TopExp_Explorer ex(body.shape.GetShape(), TopAbs_FACE); ex.More();
       ex.Next(), i++)
    if (i == idx)
      return TopoDS::Face(ex.Current());
  return {};
}

// Is there material at radius `r` from the axis at height `z`, at any of
// four angles? What says whether a thread's end opens onto air (overtravel)
// or onto metal (a floor, or the plain bore the thread stops short of).
bool SolidAround(const Body& body, const glm::dvec2& centre, double r,
                 double z) {
  try {
    for (int k = 0; k < 4; k++) {
      const double a = kPi * 0.5 * k + kPi * 0.25;
      BRepClass3d_SolidClassifier cls(
          body.shape.GetShape(),
          gp_Pnt(centre.x + r * std::cos(a), centre.y + r * std::sin(a), z),
          1e-6);
      if (cls.State() == TopAbs_IN)
        return true;
    }
  } catch (const Standard_Failure&) {
    return true; // unknown: taken as closed, the cautious reading
  }
  return false;
}

// Flatten an arc from the path's last point into chords at the section's
// chord tolerance, and declare it as one arc for the post.
void EmitArc(Toolpath& tp, const glm::dvec3& to, const glm::dvec2& centre,
             bool clockwise, double feed) {
  const glm::dvec3 from = tp.moves.back().to;
  const glm::dvec2 u = glm::dvec2(from) - centre;
  const glm::dvec2 v = glm::dvec2(to) - centre;
  const double radius = glm::length(u);
  double sweep = std::atan2(u.x * v.y - u.y * v.x, glm::dot(u, v));
  if (clockwise && sweep > 0.0)
    sweep -= 2.0 * kPi;
  if (!clockwise && sweep < 0.0)
    sweep += 2.0 * kPi;
  if (radius < 1e-9 || std::abs(sweep) < 1e-9) {
    Emit(tp, Move::Kind::Cut, to, feed);
    return;
  }
  const double perSeg = radius > kDeflection
                            ? 2.0 * std::acos(1.0 - kDeflection / radius)
                            : kPi * 0.25;
  const int steps = std::clamp(
      static_cast<int>(std::ceil(std::abs(sweep) / perSeg)), 2, 4096);
  const double a0 = std::atan2(u.y, u.x);
  Toolpath::Arc arc;
  arc.first = tp.moves.size();
  arc.centre = centre;
  arc.clockwise = clockwise;
  for (int k = 1; k <= steps; k++) {
    const double t = static_cast<double>(k) / steps;
    const double a = a0 + sweep * t;
    const glm::dvec3 p = k == steps
                             ? to
                             : glm::dvec3(centre.x + radius * std::cos(a),
                                          centre.y + radius * std::sin(a),
                                          from.z + (to.z - from.z) * t);
    Emit(tp, Move::Kind::Cut, p, feed);
  }
  arc.last = tp.moves.size() - 1;
  tp.arcs.push_back(arc);
}

} // namespace

HelixRadii ThreadRadii(bool internal, double majorDia, double minorDia,
                       double toolDia, double tipFlat, double angleDeg) {
  // FreeCAD's threadRadii: H from the diameters, (D - d)/2 = 5/8 H, and the
  // sharp-V tip H/8 past the major (inside) or H/4 inside the minor (out).
  const double H = (majorDia - minorDia) * 0.5 * 1.6;
  const double crest = tipFlat * CrestFactor(angleDeg);
  HelixRadii r;
  if (internal) {
    const double tip = majorDia * 0.5 + H / 8.0 - crest;
    r.first = (minorDia - toolDia) * 0.5;
    r.last = tip - toolDia * 0.5;
  } else {
    const double tip = minorDia * 0.5 - H / 4.0 + crest;
    r.first = (majorDia + toolDia) * 0.5;
    r.last = tip + toolDia * 0.5;
  }
  return r;
}

std::vector<double> ThreadPassRadii(int count, double first, double last) {
  std::vector<double> out;
  count = std::max(1, count);
  for (int i = 0; i < count; i++)
    out.push_back(first + (last - first) * std::sqrt((i + 1.0) / count));
  return out;
}

int DerivedThreadPasses(double depth, const Tool& tool) {
  const double bite = ProfileDepth(tool) * 0.5;
  if (bite <= 1e-9 || depth <= 1e-9)
    return 1;
  const double ratio = depth / bite;
  return std::max(1, static_cast<int>(std::ceil(ratio * ratio - 1e-9)));
}

double ThreadMillHeadWidth(const Tool& tool) {
  return tool.tipFlat + 2.0 * ProfileDepth(tool) *
                            std::tan(glm::radians(tool.tipAngleDeg * 0.5));
}

HelixDirection ThreadDirection(bool internal, bool rightHand, bool climb) {
  HelixDirection d;
  // Climb inside a hole walks the centre counter-clockwise (G3), round a boss
  // clockwise (G2), with a spindle turning M3.
  d.clockwise = internal ? !climb : climb;
  // A right-hand helix rises as it turns counter-clockwise, seen from +Z.
  d.upward = rightHand ? !d.clockwise : d.clockwise;
  return d;
}

std::vector<HelixSegment> HelixPass(const glm::dvec2& centre, bool clockwise,
                                    double zStart, double zFinal, double pitch,
                                    double radius, bool leadInOut,
                                    double elevator, bool internal) {
  std::vector<HelixSegment> out;
  const double half = (zFinal >= zStart ? pitch : -pitch) * 0.5;
  const double yMin = centre.y - radius, yMax = centre.y + radius;
  const double elevY = centre.y + elevator;
  // The lead-in turns WITH the helix in a hole and against it round a boss:
  // either way it meets the helix tangentially at +Y.
  const bool leadClockwise = internal ? clockwise : !clockwise;
  HelixSegment in;
  in.to = {centre.x, yMax, zStart};
  if (leadInOut) {
    in.arc = true;
    in.clockwise = leadClockwise;
    in.centre = {centre.x, 0.5 * (elevY + yMax)};
  }
  out.push_back(in);

  double z = zStart;
  int i = 0;
  const auto overshoots = [&](double at) {
    return half < 0.0 ? at + half < zFinal : at + half > zFinal;
  };
  while (std::abs(z - zFinal) > 1e-7 && !overshoots(z)) {
    HelixSegment s;
    s.arc = true;
    s.clockwise = clockwise;
    s.centre = centre;
    s.to = {centre.x, (i & 1) == 0 ? yMin : yMax, z + half};
    out.push_back(s);
    z += half;
    i++;
  }
  glm::dvec2 end(centre.x, (i & 1) != 0 ? yMin : yMax);
  if (std::abs(z - zFinal) > 1e-7) {
    // The partial last arc: the fraction of a half turn left, from +Y after
    // an even number of half turns and -Y after an odd one.
    const double n = std::abs(zFinal - zStart) / std::abs(half);
    const double k = n - std::floor(n);
    const double a0 = (i & 1) != 0 ? -kPi * 0.5 : kPi * 0.5;
    const double a = a0 + (clockwise ? -1.0 : 1.0) * k * kPi;
    end = centre + radius * glm::dvec2(std::cos(a), std::sin(a));
    HelixSegment s;
    s.arc = true;
    s.clockwise = clockwise;
    s.centre = centre;
    s.to = {end.x, end.y, zFinal};
    out.push_back(s);
  }
  // Back to the elevator's radius along the radial line the helix ended on.
  const glm::dvec2 ray = glm::normalize(end - centre);
  const glm::dvec2 back = centre + ray * elevator;
  HelixSegment o;
  o.to = {back.x, back.y, zFinal};
  if (leadInOut) {
    o.arc = true;
    o.clockwise = leadClockwise;
    o.centre = 0.5 * (end + back);
  }
  out.push_back(o);
  return out;
}

std::string HelicalArcRefusal(const Setup& setup) {
  if (!PostWritesHelicalArcs(setup.machine.post))
    return "post '" + setup.machine.post +
           "' does not write helical arcs, and a thread mill is nothing but "
           "a helix of G2/G3 arcs with Z";
  if (!setup.machine.fitArcs)
    return "machine '" +
           (setup.machine.name.empty() ? std::string("(this setup's)")
                                       : setup.machine.name) +
           "' is set to post no arcs (fit_arcs off), and a thread mill is a "
           "helix of G2/G3 arcs: turn arcs on for it, or mill the thread on a "
           "machine that takes them";
  return {};
}

std::string ThreadToolRefusal(const Tool& tool, double pitch) {
  if (tool.type != ToolType::ThreadMill)
    return ToolName(tool) +
           " is not a thread mill: a thread is milled with a single-profile "
           "60 degree head on a neck -- set its tool_type to thread_mill";
  if (std::abs(tool.tipAngleDeg - 60.0) > 0.5)
    return ToolName(tool) + "'s V is " + Mm(tool.tipAngleDeg, 1) +
           " degrees and ISO and unified threads have 60 degree flanks";
  if (tool.neckDiameter <= 0.0 || tool.neckDiameter >= tool.diameter)
    return ToolName(tool) + " states no neck narrower than its head "
                            "(neck_diameter): a thread mill's head stands out "
                            "from its neck by the depth it can cut";
  if (tool.tipFlat > pitch / 8.0 + 1e-9)
    return ToolName(tool) + "'s tip flat is " + Mm(tool.tipFlat) +
           " mm and a " + Mm(pitch) +
           " mm pitch's root is P/8 = " + Mm(pitch / 8.0) +
           " mm: the flat would cut the root wider than the thread allows. "
           "The thread is too fine for this cutter";
  return {};
}

ThreadPlan PlanThread(const Setup& setup, const Operation& op, const Tool& tool,
                      const std::vector<Body>& framed, const Target& target) {
  ThreadPlan out;
  if (std::string why = HelicalArcRefusal(setup); !why.empty()) {
    out.why = std::move(why);
    return out;
  }
  if (!target.IsSet()) {
    out.why = "a thread mill cuts the thread on a face: target the "
              "cylindrical face a Thread (or a tapped Hole) is on";
    return out;
  }
  const Body* body = nullptr;
  int idx = -1;
  for (const Body& b : framed) {
    if (b.reference || !b.shape.IsValid() ||
        b.createdBy != target.ref.featureId)
      continue;
    if (target.jobPartId != 0 && b.jobPartId != 0 &&
        b.jobPartId != target.jobPartId)
      continue;
    if (const std::optional<int> i = b.elementMap.Resolve(target.ref.name)) {
      body = &b;
      idx = *i;
      break;
    }
  }
  if (body == nullptr) {
    out.why = "that face is not on a body this job can machine";
    return out;
  }
  // The thread on it: the LAST one written, since a Thread on a tapped
  // Hole's bore comes after the Hole and says more (class, hand).
  // By the face each name resolves to, not by spelling: a name written when
  // the Hole drilled can gain a suffix as later features touch the face.
  const ThreadMark* mark = nullptr;
  for (const ThreadMark& m : body->threads)
    if (m.face.featureId == target.ref.featureId &&
        (m.face.name == target.ref.name ||
         body->elementMap.Resolve(m.face.name) == std::optional<int>(idx)))
      mark = &m;
  if (mark == nullptr) {
    out.why = "face '" + target.ref.name +
              "' carries no thread: a thread mill cuts the thread a Thread "
              "feature (or a Hole's tapped fit) puts on a face -- add one to "
              "it, and pick the face again";
    return out;
  }
  out.mark = *mark;
  const ThreadMark& m = out.mark;
  const double P = m.pitch;
  if (P <= 1e-9) {
    out.why = "the thread on that face has no pitch";
    return out;
  }
  // A thread mill's form is the 60-degree profile; milled into a thread of
  // another flank angle it cuts the wrong groove, so it is refused.
  if (std::abs(m.flankDeg - 30.0) > 1e-9) {
    char buf[200];
    std::snprintf(buf, sizeof(buf),
                  "the thread on that face has %g-degree flanks; a thread "
                  "mill cuts the 60-degree profile (30-degree flanks) only",
                  m.flankDeg);
    out.why = buf;
    return out;
  }
  if (std::string why = ThreadToolRefusal(tool, P); !why.empty()) {
    out.why = std::move(why);
    return out;
  }

  const TopoDS_Face face = FaceAt(*body, idx);
  if (face.IsNull()) {
    out.why = "that face could not be resolved";
    return out;
  }
  glm::dvec2 centre(0.0);
  double r = 0.0, zLo = 0.0, zHi = 0.0, faceLo = 0.0, faceHi = 0.0;
  try {
    const BRepAdaptor_Surface surf(face);
    if (surf.GetType() != GeomAbs_Cylinder) {
      out.why = "a thread is on a cylindrical face, and that one is not";
      return out;
    }
    const gp_Cylinder cyl = surf.Cylinder();
    const gp_Dir dir = cyl.Axis().Direction();
    if (std::abs(dir.Z()) < 0.999) {
      out.why = "that thread's axis does not point at the spindle -- lay a "
                "face up so it does, or mill it in another setup";
      return out;
    }
    const gp_Pnt loc = cyl.Location();
    r = cyl.Radius();
    // The threaded span: the face's V range (axial), clipped to `length`
    // from its V-min end -- the Thread's own reading of it.
    double v0 = surf.FirstVParameter(), v1 = surf.LastVParameter();
    if (v1 < v0)
      std::swap(v0, v1);
    const double t1 = m.length > 1e-9 ? std::min(v1, v0 + m.length) : v1;
    const auto zAt = [&](double v) { return loc.Z() + v * dir.Z(); };
    const auto xyAt = [&](double v) {
      return glm::dvec2(loc.X() + v * dir.X(), loc.Y() + v * dir.Y());
    };
    centre = 0.5 * (xyAt(v0) + xyAt(v1));
    zLo = std::min(zAt(v0), zAt(t1));
    zHi = std::max(zAt(v0), zAt(t1));
    faceLo = std::min(zAt(v0), zAt(v1));
    faceHi = std::max(zAt(v0), zAt(v1));
  } catch (const Standard_Failure& e) {
    out.why =
        std::string("that face could not be read: ") + e.GetMessageString();
    return out;
  }

  const bool internal = !m.external;
  // A bore has to open toward the spindle: one tapped up from the underside
  // has metal over its top, and a vertical axis alone let the head go in
  // through it.
  // Over a drill's point, when the model's floor is one (HoleMark): its
  // cone is air up to the apex, and a bore whose floor is up read as open.
  double point = 0.0;
  for (const HoleMark& h : body->holes)
    if (!h.through && h.pointAngleDeg < 180.0 - 1e-6 &&
        body->elementMap.Resolve(h.bore.name) == std::optional<int>(idx))
      point = r / std::tan(glm::radians(h.pointAngleDeg * 0.5));
  if (internal && (SolidAround(*body, centre, r * 0.5, faceHi + 0.05) ||
                   (point > 0.0 && SolidAround(*body, centre, r * 0.5,
                                               faceHi + point + 0.05)))) {
    out.why = "that thread's bore opens downward: its top, at Z " +
              Mm(faceHi, 2) +
              ", is closed by the part. Mill it from the side it opens on "
              "-- a setup with that face up (Flip)";
    return out;
  }
  const double major = m.majorDia > 0.0 ? m.majorDia
                       : m.external
                           ? 2.0 * r
                           : 2.0 * r + 1.0825 * P; // a custom thread's own
                                                   // rule (30 degrees: above)
  // The class: the one stated, else the series' general-purpose one, SAID.
  const std::optional<threads::Standard> table = threads::Lookup(m.designation);
  const threads::Series series =
      table ? table->series : threads::Series::Metric;
  std::string cls = m.threadClass;
  const bool stated = !cls.empty();
  if (!stated) {
    cls = threads::DefaultClass(series, m.external);
    out.notes.push_back("the thread states no class, so the last pass aims at "
                        "the middle of " +
                        cls +
                        ", its series' general-purpose class -- give the "
                        "Thread a thread_class to aim at another");
  }
  if (!table)
    out.notes.push_back("'" + m.designation +
                        "' is a custom thread: its band is ISO 965's " + cls +
                        " for its diameter and pitch");
  std::string bandWhy;
  const std::optional<threads::PitchBand> band =
      threads::PitchDiameterBand(series, major, P, cls, m.external, &bandWhy);
  if (!band) {
    out.why = bandWhy;
    return out;
  }

  // Aim the last pass at the middle of the class's pitch-diameter band: the
  // tool's rim sits where the thread's sharp V would put its point for that
  // pitch diameter, less the flat's own reach.
  const double d = tool.diameter;
  const double k = CrestFactor(tool.tipAngleDeg);
  const double H = P * k; // the sharp V's height
  const double aim = band->Mid();
  const double apex = internal ? aim * 0.5 + H * 0.5 : aim * 0.5 - H * 0.5;
  const double tipR =
      internal ? apex - tool.tipFlat * k : apex + tool.tipFlat * k;
  out.finalRadius = internal ? tipR - d * 0.5 : tipR + d * 0.5;
  out.startRadius = internal ? r - d * 0.5 : r + d * 0.5;
  out.depth = internal ? tipR - r : r - tipR;
  if (out.depth <= 1e-6) {
    out.why = std::string("the face (") + Mm(2.0 * r) + " mm) is already " +
              (internal ? "past" : "inside") + " the thread's root at " +
              Mm(2.0 * tipR) +
              " mm: there is nothing to cut -- is this the tap-drill bore?";
    return out;
  }
  if (internal && d + 2.0 * out.depth > 2.0 * r + 1e-9) {
    out.why = ToolName(tool) +
              " is too large for this tap-drill bore: the "
              "cutter plus twice the thread's depth (" +
              Mm(d, 2) + " + 2 x " + Mm(out.depth) + " = " +
              Mm(d + 2.0 * out.depth) + " mm) must fit the " + Mm(2.0 * r) +
              " mm bore, or the helix has no room inside it. Use a cutter no "
              "wider than " +
              Mm(2.0 * r - 2.0 * out.depth, 2) + " mm";
    return out;
  }
  if (ProfileDepth(tool) < out.depth + 1e-6) {
    out.why = ToolName(tool) + "'s head stands " + Mm(ProfileDepth(tool)) +
              " mm out from its neck and this thread is " + Mm(out.depth) +
              " mm deep from the " + (internal ? "bore" : "boss") +
              ": the neck would rub the crests before the last pass reached "
              "the root. Use a cutter with a deeper profile (a narrower neck)";
    return out;
  }
  if (op.threadPasses < 0) {
    out.why = "passes must be 1 or more (0 derives them)";
    return out;
  }
  const int n = op.threadPasses > 0 ? op.threadPasses
                                    : DerivedThreadPasses(out.depth, tool);
  out.passes = ThreadPassRadii(n, out.startRadius, out.finalRadius);
  // The elevator: where the tool goes up and down, half a pitch clear of the
  // bore's wall inside (FreeCAD's millimetre, taken from the thread), half a
  // pitch clear of the boss outside.
  out.elevator = internal ? std::max(0.0, out.startRadius - P * 0.5)
                          : out.startRadius + P * 0.5;
  out.headWidth = ThreadMillHeadWidth(tool);
  const double w = out.headWidth;

  // The ends. Air past an end at the thread's wall means the helix may run
  // past it; metal means a floor (the head's tip stops on it) or the plain
  // bore a short thread stops in (the V stops at the thread's end).
  const double wallR = internal ? r + out.depth * 0.5 : r - out.depth * 0.5;
  const bool bottomOpen = !SolidAround(*body, centre, wallR, zLo - P);
  const bool topOpen = !SolidAround(*body, centre, wallR, zHi + P);
  double c0 = 0.0; // the V's middle, lowest and highest
  if (bottomOpen)
    c0 = zLo - P; // overtravel: a pitch past the last full thread
  else if (zLo > faceLo + 1e-6)
    c0 = zLo; // the thread stops mid-bore
  else
    c0 = faceLo + w * 0.5; // a floor: the head's tip on it
  const double c1 = topOpen ? zHi + P * 0.5 : zHi;
  if (c1 - c0 < P) {
    out.why = "there is not a pitch of room for the head's " + Mm(w) +
              " mm V between the floor and the top of the thread";
    return out;
  }
  out.tipLo = c0 - w * 0.5;
  out.tipHi = c1 - w * 0.5;
  out.direction = ThreadDirection(internal, m.rightHand, op.climb);

  Toolpath::Thread& t = out.thread;
  t.op = op.id;
  t.centre = centre;
  t.internal = internal;
  t.rightHand = m.rightHand;
  t.pitch = P;
  t.designation = m.designation;
  t.threadClass = cls;
  t.classStated = stated;
  t.pdMin = band->min;
  t.pdMax = band->max;
  t.pdAim = aim;
  t.boreDia = 2.0 * r;
  t.zLo = zLo;
  t.zHi = zHi;
  t.through = bottomOpen;
  t.fullLo = std::clamp(c0 + P * 0.5, zLo, zHi);
  t.fullHi = std::clamp(c1 - P * 0.5, zLo, zHi);
  t.tool = tool;

  if (t.fullLo > zLo + 1e-6)
    out.notes.push_back(
        "the last " + Mm(t.fullLo - zLo, 2) +
        " mm of the thread are not full depth: " +
        (zLo > faceLo + 1e-6
             ? std::string("the thread stops mid-bore, and the V stops there")
             : "the floor stops the head, whose V is " + Mm(w, 2) +
                   " mm tall"));
  if (bottomOpen) {
    out.notes.push_back("through: the helix runs a pitch past the last full "
                        "turn, its tip down to Z " +
                        Mm(out.tipLo, 2) +
                        " -- support the part so the head runs out into air "
                        "or the spoilboard");
    glm::dvec3 stockLo(0.0), stockHi(0.0);
    if (StockBoundsFramed(setup, framed, stockLo, stockHi) &&
        stockLo.z < zLo - 1e-6)
      out.notes.push_back(
          "the stock runs " + Mm(zLo - stockLo.z, 2) +
          " mm below the thread: the tap drill has to go through it too (a "
          "derived drill does), or the head's overtravel meets it -- the "
          "check says so if it does");
  }
  (void)faceHi;
  out.ok = true;
  return out;
}

namespace detail {

void GenerateThreadMill(const Setup& setup, const Operation& op,
                        const Tool& tool, const std::vector<Body>& bodies,
                        Toolpath& tp) {
  if (op.targets.empty()) {
    tp.error = "a thread mill needs its target: the face the thread is on";
    return;
  }
  const ThreadPlan plan = PlanThread(setup, op, tool, bodies, op.targets[0]);
  if (!plan.ok) {
    tp.error = plan.why;
    return;
  }
  const Toolpath::Thread& t = plan.thread;
  glm::dvec3 lo(0.0), hi(0.0);
  const double stockTop =
      StockBoundsFramed(setup, bodies, lo, hi) ? hi.z : setup.safeZ;
  const glm::dvec2 c = t.centre;
  const glm::dvec3 elevatorTop(c.x, c.y + plan.elevator, setup.safeZ);
  // The feed at the tool's CENTRE that runs its rim at the cutting feed: in
  // a bore the rim travels (R + d/2) / R as far as the centre does, so the
  // centre is slowed by that; round a boss the rim travels less, and the
  // centre is held at the cutting feed rather than sped up.
  const auto feedAt = [&](double radius) {
    if (!t.internal || radius <= 1e-9)
      return tool.feedCut;
    return tool.feedCut * radius / (radius + tool.diameter * 0.5);
  };
  const double zStart = plan.direction.upward ? plan.tipLo : plan.tipHi;
  const double zFinal = plan.direction.upward ? plan.tipHi : plan.tipLo;

  // Out of whatever came before, straight up, then over to the elevator.
  if (!tp.moves.empty() && tp.moves.back().to.z < setup.safeZ - 1e-9)
    Emit(tp, Move::Kind::Rapid,
         {tp.moves.back().to.x, tp.moves.back().to.y, setup.safeZ}, 0.0);
  for (size_t i = 0; i < plan.passes.size(); i++) {
    const double radius = plan.passes[i];
    const glm::dvec3 entry(c.x, c.y + plan.elevator, zStart);
    if (i == 0 || !t.internal) {
      // Down to the entry: at rapid above the stock, at feed below it --
      // through a bore that is air if the tap drill was cut, and the
      // verifier says so if it was not.
      if (i > 0)
        Emit(tp, Move::Kind::Rapid,
             {tp.moves.back().to.x, tp.moves.back().to.y, setup.safeZ}, 0.0);
      Emit(tp, Move::Kind::Rapid, elevatorTop, 0.0);
      // A rapid never goes below the stock top
      // (docs/cam/README.md#rapids-profile-outlines-and-through-cuts).
      const double floor =
          std::clamp(std::max(zStart, stockTop), zStart, setup.safeZ);
      Emit(tp, Move::Kind::Rapid, {entry.x, entry.y, floor}, 0.0);
    }
    // Inside a bore the next pass starts from where the last one left the
    // tool: at the elevator's radius, so the straight move to the next entry
    // stays inside that circle, in the air of the bore.
    Emit(tp, Move::Kind::Cut, entry,
         EntryFeed(tool, tp.moves.back().to, entry));
    const double feed = feedAt(radius);
    for (const HelixSegment& s :
         HelixPass(c, plan.direction.clockwise, zStart, zFinal, t.pitch, radius,
                   op.leadArcs, plan.elevator, t.internal)) {
      if (s.arc)
        EmitArc(tp, s.to, s.centre, s.clockwise, feed);
      else
        Emit(tp, Move::Kind::Cut, s.to, feed);
    }
  }
  // Out of the thread straight up.
  Emit(tp, Move::Kind::Rapid,
       {tp.moves.back().to.x, tp.moves.back().to.y, setup.safeZ}, 0.0);
  tp.threads.push_back(t);
}

} // namespace detail

// --- judging
// ------------------------------------------------------------------

namespace {

struct HelixRun {
  double radius = 0.0;
  double swept = 0.0; // signed, radians
  double rise = 0.0;  // signed, mm
  double zMin = 0.0, zMax = 0.0;
  int moves = 0;
};

} // namespace

ThreadVerdict JudgeThread(const GProgram& program,
                          const Toolpath::Thread& decl) {
  ThreadVerdict v;
  v.op = decl.op;
  v.designation = decl.designation;
  v.threadClass = decl.threadClass;
  v.classStated = decl.classStated;
  v.internal = decl.internal;
  v.pitch = decl.pitch;
  v.rightHand = decl.rightHand;
  v.pdMin = decl.pdMin;
  v.pdMax = decl.pdMax;
  v.zLo = decl.zLo;
  v.zHi = decl.zHi;
  const glm::dvec2 c = decl.centre;
  std::vector<HelixRun> runs;
  bool any = false, inRun = false;
  v.tipLowest = std::numeric_limits<double>::max();
  for (size_t j = 1; j < program.moves.size(); j++) {
    const GMove& m = program.moves[j];
    if (m.op != decl.op) {
      inRun = false;
      continue;
    }
    any = true;
    v.tipLowest = std::min(v.tipLowest, m.to.z);
    const glm::dvec3 from = program.moves[j - 1].to;
    const double rf = glm::length(glm::dvec2(from) - c);
    const double rt = glm::length(glm::dvec2(m.to) - c);
    const bool helical = !m.rapid && rf > 1e-3 && std::abs(rf - rt) < 1e-3 &&
                         std::abs(m.to.z - from.z) > 1e-9 &&
                         glm::length(glm::dvec2(m.to - from)) > 1e-9;
    if (!helical) {
      inRun = false;
      continue;
    }
    const glm::dvec2 u = glm::dvec2(from) - c, w = glm::dvec2(m.to) - c;
    const double step = std::atan2(u.x * w.y - u.y * w.x, glm::dot(u, w));
    if (!inRun || std::abs(runs.back().radius - rt) > 1e-3) {
      runs.push_back({rt, 0.0, 0.0, std::min(from.z, m.to.z),
                      std::max(from.z, m.to.z), 0});
      inRun = true;
    }
    HelixRun& run = runs.back();
    run.swept += step;
    run.moves++;
    run.rise += m.to.z - from.z;
    run.zMin = std::min({run.zMin, from.z, m.to.z});
    run.zMax = std::max({run.zMax, from.z, m.to.z});
  }
  if (!any) {
    v.tipLowest = 0.0;
    v.why = "operation " + std::to_string(decl.op) + " is not in the file";
    return v;
  }
  // A helix is chords of at least a half turn; a single straight move that
  // happens to keep its radius (a link between passes) is not one.
  std::erase_if(runs, [](const HelixRun& r) {
    return r.moves < 3 || std::abs(r.swept) < kPi - 1e-6;
  });
  if (runs.empty()) {
    v.why = "no helix of operation " + std::to_string(decl.op) +
            " in the file: nothing turned a half turn while it rose or fell";
    return v;
  }
  v.passes = static_cast<int>(runs.size());
  // The last pass: the one whose tip went furthest into the thread.
  const HelixRun& last = *std::max_element(
      runs.begin(), runs.end(), [&](const HelixRun& a, const HelixRun& b) {
        return decl.internal ? a.radius < b.radius : a.radius > b.radius;
      });
  v.pitchMeasured = std::abs(last.rise) * 2.0 * kPi / std::abs(last.swept);
  v.handMeasured = (last.swept > 0.0) == (last.rise > 0.0);
  const Tool& tool = decl.tool;
  const double k = CrestFactor(tool.tipAngleDeg);
  const double H = decl.pitch * k;
  const double tipR = decl.internal ? last.radius + tool.diameter * 0.5
                                    : last.radius - tool.diameter * 0.5;
  const double apex =
      decl.internal ? tipR + tool.tipFlat * k : tipR - tool.tipFlat * k;
  v.pdReached = decl.internal ? 2.0 * apex - H : 2.0 * apex + H;
  const double w = ThreadMillHeadWidth(tool);
  v.fullFrom = last.zMin + w * 0.5 + decl.pitch * 0.5;
  v.fullTo = last.zMax + w * 0.5 - decl.pitch * 0.5;
  v.shortMm = std::max(0.0, v.fullFrom - decl.fullLo) +
              std::max(0.0, decl.fullHi - v.fullTo);
  v.floorLimited = decl.fullLo > decl.zLo + 1e-6;
  v.neckClearance =
      decl.internal
          ? decl.boreDia * 0.5 - (last.radius + tool.neckDiameter * 0.5)
          : (last.radius - tool.neckDiameter * 0.5) - decl.boreDia * 0.5;
  v.judged = true;
  return v;
}

void MeasureBoreBefore(const GProgram& program, const Setup& setup,
                       const HeightMap& stock, double stockBottom,
                       const Toolpath::Thread& decl, ThreadVerdict& v) {
  size_t first = program.moves.size();
  for (size_t j = 0; j < program.moves.size(); j++)
    if (program.moves[j].op == decl.op) {
      first = j;
      break;
    }
  if (first == program.moves.size() || stock.nx == 0)
    return;
  const Tool& tool = decl.tool;
  // Where the head travels: the bore inside the tap drill's wall, or the ring
  // round the boss the head's width out.
  const double rIn = decl.internal ? 0.0 : decl.boreDia * 0.5 + stock.res;
  const double rOut = decl.internal ? decl.boreDia * 0.5 - stock.res
                                    : decl.boreDia * 0.5 + tool.diameter;
  const glm::dvec2 c = decl.centre;
  const int i0 =
      std::max(0, static_cast<int>((c.x - rOut - stock.x0) / stock.res));
  const int i1 = std::min(
      stock.nx - 1, static_cast<int>((c.x + rOut - stock.x0) / stock.res) + 1);
  const int j0 =
      std::max(0, static_cast<int>((c.y - rOut - stock.y0) / stock.res));
  const int j1 = std::min(
      stock.ny - 1, static_cast<int>((c.y + rOut - stock.y0) / stock.res) + 1);
  if (i1 < i0 || j1 < j0)
    return;
  // The crop, as the program before the op leaves it: every cut before it.
  HeightMap crop;
  crop.res = stock.res;
  crop.x0 = stock.x0 + i0 * stock.res;
  crop.y0 = stock.y0 + j0 * stock.res;
  crop.nx = i1 - i0 + 1;
  crop.ny = j1 - j0 + 1;
  crop.z.resize(static_cast<size_t>(crop.nx) * static_cast<size_t>(crop.ny));
  for (int j = 0; j < crop.ny; j++)
    for (int i = 0; i < crop.nx; i++)
      crop.Set(i, j, stock.At(i0 + i, j0 + j));
  for (size_t j = 1; j < first; j++) {
    const GMove& m = program.moves[j];
    if (m.rapid)
      continue;
    CarveSweep(program.moves[j - 1].to, m.to, setup.FindTool(m.tool), crop);
  }
  for (int j = 0; j < crop.ny; j++)
    for (int i = 0; i < crop.nx; i++) {
      const double r =
          glm::length(glm::dvec2(crop.CellX(i), crop.CellY(j)) - c);
      if (r < rIn || r > rOut)
        continue;
      const double standing =
          crop.At(i, j) - std::max(v.tipLowest, stockBottom);
      if (standing > v.boreStanding) {
        v.boreStanding = standing;
        v.boreStandingAt = {crop.CellX(i), crop.CellY(j)};
      }
    }
}

std::vector<float> ThreadExcuse(const HeightMap& grid,
                                const std::vector<Toolpath::Thread>& decls,
                                const std::vector<ThreadVerdict>& judged) {
  std::vector<float> floor(grid.z.size(), std::numeric_limits<float>::max());
  for (size_t n = 0; n < decls.size() && n < judged.size(); n++) {
    const Toolpath::Thread& t = decls[n];
    const ThreadVerdict& v = judged[n];
    if (!v.judged)
      continue;
    const double k = CrestFactor(t.tool.tipAngleDeg);
    const double H = t.pitch * k;
    // Inside: out to the widest tip the class allows. Outside: in to the
    // narrowest, and out to where the head can reach.
    const double rIn =
        t.internal ? 0.0 : t.pdMin * 0.5 - H * 0.5 + t.tool.tipFlat * k;
    const double rOut = t.internal
                            ? t.pdMax * 0.5 + H * 0.5 - t.tool.tipFlat * k +
                                  kCutterWallTolerance
                            : t.boreDia * 0.5 + t.tool.diameter + grid.res;
    const int i0 =
        std::max(0, static_cast<int>((t.centre.x - rOut - grid.x0) / grid.res));
    const int i1 = std::min(
        grid.nx - 1,
        static_cast<int>((t.centre.x + rOut - grid.x0) / grid.res) + 1);
    const int j0 =
        std::max(0, static_cast<int>((t.centre.y - rOut - grid.y0) / grid.res));
    const int j1 = std::min(
        grid.ny - 1,
        static_cast<int>((t.centre.y + rOut - grid.y0) / grid.res) + 1);
    for (int j = j0; j <= j1; j++)
      for (int i = i0; i <= i1; i++) {
        const double r =
            glm::length(glm::dvec2(grid.CellX(i), grid.CellY(j)) - t.centre);
        if (r < rIn - kCutterWallTolerance || r > rOut)
          continue;
        const size_t idx =
            static_cast<size_t>(j) * static_cast<size_t>(grid.nx) +
            static_cast<size_t>(i);
        floor[idx] = std::min(floor[idx], static_cast<float>(v.tipLowest));
      }
  }
  return floor;
}

} // namespace cam
